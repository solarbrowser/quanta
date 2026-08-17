/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_OBJECT_H
#define QUANTA_OBJECT_H

#include "quanta/core/runtime/Value.h"
#include "quanta/core/runtime/Shape.h"
#include "quanta/core/runtime/SmallMapPool.h"
#include "quanta/core/vm/Bytecode.h"
#include "quanta/parser/FunctionExecutable.h"
#include <unordered_map>
#include <unordered_set>
#include <span>
#include <vector>
#include <array>
#include <string>
#include <memory>
#include <functional>
#include <chrono>

namespace Quanta {

class PropertyDescriptor;
class HybridDescriptorMap;
class ScriptUnit;
struct RareExtras;

// Fixed-position header for Object::butterfly_ -- always exactly 3
// Value-widths so it sits at a capacity-independent offset from the
// butterfly pointer (`reinterpret_cast<ButterflyHeader*>(butterfly_) - 1`).
// Never accessed as a Value, never GC-traced (plain counters + a raw,
// explicitly-owned RareExtras*: Object::free_butterfly() deletes it,
// Object::realloc_butterfly() transplants it to the new header instead --
// see both for why the distinction matters).
struct ButterflyHeader {
    uint32_t elements_length = 0;
    uint32_t elements_capacity = 0;
    // Bit 31 carries dense_verified; the shape system caps a slot count at
    // kMaxSlots (128), so the rest of the word is permanently free.
    uint32_t shape_capacity = 0;
    // The array's JS length, which is NOT the element count: trailing holes
    // are exactly the indices between elements_length and this. It lives here
    // rather than in a descriptor because a descriptor map costs hundreds of
    // bytes and drops the object into dictionary mode, and every array used to
    // get one at birth just to hold this number.
    uint32_t array_length = 0;
    RareExtras* extras = nullptr;
};
static_assert(sizeof(ButterflyHeader) == 3 * sizeof(Value), "must be exactly 3 Value-widths");

class Context;
class Environment;
class ASTNode;
class Parameter;
class Visitor;

class Object {
public:
    enum class ObjectType : uint8_t {
        Ordinary,
        Array,
        Arguments,
        Function,
        String,
        Number,
        Boolean,
        Date,
        RegExp,
        Error,
        Promise,
        Proxy,
        Map,
        Set,
        WeakMap,
        WeakSet,
        WeakRef,
        FinalizationRegistry,
        ArrayBuffer,
        TypedArray,
        DataView,
        Symbol,
        BigInt,
        Custom
    };

    // constinit so every translation unit knows the initializer is a constant:
    // without it a thread_local defined elsewhere is reached through a lazy
    // init wrapper, and the check plus its call land on the call path three
    // times per invocation.
    static constinit thread_local Context* current_context_;

    // Monotonic; bumped by set_prototype() and by property add/remove/
    // attribute-change on a used_as_prototype() object. SetNamed's
    // transition-cache trusts a cached "no [[Set]] blocker on this chain"
    // answer only while this hasn't moved since it was validated.
    static constinit thread_local uint64_t proto_epoch_;
public:
    static uint64_t proto_epoch() { return proto_epoch_; }
private:
    static void bump_proto_epoch() { ++proto_epoch_; }

    // Same idea as proto_epoch_ for a different question: has ANY object
    // anywhere gained a NEW descriptors_ entry (getter/setter install,
    // defineProperty, dictionary-mode migration)? get_named/set_named's
    // own-property cache trusts a learned "no descriptor override for this
    // key" answer only while this hasn't moved since it was validated --
    // descriptors_ is per-OBJECT, not per-shape, so two instances sharing a
    // shape can disagree; bumping globally on ANY object's change is always
    // safe (worst case, an unrelated object's change forces one extra real
    // check), just occasionally more conservative than a per-object signal
    // would be.
    static constinit thread_local uint64_t descriptor_epoch_;
public:
    static uint64_t descriptor_epoch() { return descriptor_epoch_; }

    // Same idea as the epochs above but one-way: a "protector" for array
    // spread's fast path. True while nothing has redefined how arrays
    // iterate, which lets a plain Array be bulk-copied instead of driven
    // through the iterator protocol (an iterator object plus a result object
    // and a next() call per element). See Object.cpp for what clears it.
    static bool array_iterator_protector_intact();
    // Registered once by Iterator's setup so a later write to
    // %ArrayIteratorPrototype%.next can be recognised as invalidating.
    static void watch_array_iterator_prototype(Object* proto);
    // Called once after the intrinsics are installed: their own @@iterator
    // definitions would otherwise leave the protector permanently cleared
    // before any user code has run.
    static void arm_array_iterator_protector();
private:
    static void bump_descriptor_epoch() { ++descriptor_epoch_; }

    // [[Prototype]] + 2 status bits (extensibility, "ever used as a
    // prototype"), tagged into the pointer's own low bits. GC heap cells are
    // at least 16-byte aligned (HeapBlock::kCellAlign), so bits 0-3 of any
    // real, non-null Object* are always zero. Transparent proxy for Object*:
    // every existing `proto_->x`, `if (proto_)`, `proto_ == y` call site
    // keeps compiling unchanged; only flag access needs the explicit
    // flag()/set_flag()/clear_flag() API, and assignment (`proto_ = ptr`)
    // preserves the existing flag bits rather than wiping them.
    class TaggedProto {
    public:
        TaggedProto() = default;
        TaggedProto(Object* p) : bits_(reinterpret_cast<uintptr_t>(p)) {}
        Object* get() const { return reinterpret_cast<Object*>(bits_ & ~kMask); }
        operator Object*() const { return get(); }
        Object* operator->() const { return get(); }
        TaggedProto& operator=(Object* p) {
            bits_ = (reinterpret_cast<uintptr_t>(p) & ~kMask) | (bits_ & kMask);
            return *this;
        }
        bool flag(uintptr_t m) const { return (bits_ & m) != 0; }
        void set_flag(uintptr_t m) { bits_ |= m; }
        void clear_flag(uintptr_t m) { bits_ &= ~m; }
        void clear_flags() { bits_ &= ~kMask; }
    private:
        // Objects are at least 16-byte aligned (HeapBlock::kCellAlign), so the
        // low four bits are always free; three are in use.
        static constexpr uintptr_t kMask = 0x7;
        uintptr_t bits_ = 0;
    };
    TaggedProto proto_;

    // Named (non-index) fast-path properties: shape describes the layout
    // (which keys, in what slot), the positive side of butterfly_ holds
    // this instance's own values at those slots. Every plain object starts
    // at Shape::root() (no properties) and transitions as keys are added.
    // A property that needs non-default attributes, or any delete, moves
    // everything into descriptors_ and sets shape_ to nullptr -- see
    // migrate_to_dictionary_mode.
    //
    // ObjectType is ALSO tagged into shape_'s low 5 bits (Shape is
    // alignas(32), Shape.h) instead of its own header field -- same
    // transparent-proxy trick as TaggedProto above; `shape_->x`, `if
    // (shape_)`, `shape_ = next` all keep compiling unchanged.
    class TaggedShapePtr {
    public:
        TaggedShapePtr() = default;
        TaggedShapePtr(Shape* s) : bits_(reinterpret_cast<uintptr_t>(s)) {}
        Shape* get() const { return reinterpret_cast<Shape*>(bits_ & ~kMask); }
        operator Shape*() const { return get(); }
        Shape* operator->() const { return get(); }
        TaggedShapePtr& operator=(Shape* s) {
            bits_ = (reinterpret_cast<uintptr_t>(s) & ~kMask) | (bits_ & kMask);
            return *this;
        }
        ObjectType type() const { return static_cast<ObjectType>(bits_ & kMask); }
        void set_type(ObjectType t) { bits_ = (bits_ & ~kMask) | static_cast<uintptr_t>(t); }
    private:
        static constexpr uintptr_t kMask = 0x1F;
        uintptr_t bits_ = 0;
    };
    TaggedShapePtr shape_ = Shape::root();
    // Shape.h hardcodes alignas(32) as a literal (deliberately standalone,
    // no Object.h dependency) -- this keeps the two files' coupling
    // compiler-enforced instead of just commented.
    static_assert(alignof(Shape) >= 32, "TaggedShapePtr needs 5 spare low bits from Shape's alignment");

    // is_extensible()/prevent_extensions()/reopen_extensible()'s bit in proto_.
    static constexpr uintptr_t kNotExtensible = 0x1;

    // Single allocation backing both dense array elements and shape-slot
    // values, one pointer instead of two std::vectors' own ptr+size+capacity
    // (the shape-slot count is already available from the shared `shape_`).
    // Layout, low to high address:
    //   [element[capacity-1]]...[element[0]] [ButterflyHeader] [shape slot 0]...[shape slot N-1]
    //                                                           ^ butterfly_ points here
    // `butterfly_[i]` is shape slot i; `butterfly_[-4-i]` is element i (the
    // header occupies the three slots right before butterfly_, including the
    // RareExtras* -- see ButterflyHeader above). nullptr until the object
    // needs any of: array elements, shape-mode properties, or RareExtras.
    Value* butterfly_ = nullptr;

    // butterfly_ helpers. Trivial ones inline; growth/free in Object.cpp.
    ButterflyHeader* butterfly_header() const {
        return reinterpret_cast<ButterflyHeader*>(butterfly_) - 1;
    }
    uint32_t elements_capacity() const { return butterfly_ ? butterfly_header()->elements_capacity : 0; }
    uint32_t elements_length() const { return butterfly_ ? butterfly_header()->elements_length : 0; }
    // Caches the whole has_only_dense_elements() answer, not just its length
    // comparison: that check runs on every indexed access and every .length
    // read, and its holes/sparse/descriptor probes each chase
    // butterfly -> header -> RareExtras, which is a cache miss per array once
    // the working set is large.
    // It lives in the butterfly header's spare word, not in Object, which
    // sits on a Heap size class shared by every object type -- the header is
    // already there for any object with elements, at a fixed negative offset.
    // Staleness can only cost speed, never correctness: a cleared flag just
    // re-runs the full check. Every way an array can stop being dense funnels
    // through one function, and each clears it -- bump_array_length,
    // ArraySetLength and resize_elements for the length, ensure_deleted_elements
    // before a hole, ensure_sparse_overflow before a spill, and
    // note_descriptor_key when an index first gets attributes. Clearing on the
    // ensure_ accessors is deliberately early: a clear with no insert behind it
    // costs one re-check, a missed clear answers wrongly.
    static constexpr uint32_t kDenseVerifiedBit = 1u << 31;
    bool dense_verified() const {
        return butterfly_ && (butterfly_header()->shape_capacity & kDenseVerifiedBit);
    }
    void mark_dense_verified() const {
        if (butterfly_) butterfly_header()->shape_capacity |= kDenseVerifiedBit;
    }
    void invalidate_dense() {
        if (butterfly_) butterfly_header()->shape_capacity &= ~kDenseVerifiedBit;
    }
    uint32_t shape_capacity() const {
        return butterfly_ ? (butterfly_header()->shape_capacity & ~kDenseVerifiedBit) : 0;
    }
    // An array's JS length. Zero without a butterfly, which is right: an array
    // with no elements and no explicit length is empty.
    uint32_t array_length() const { return butterfly_ ? butterfly_header()->array_length : 0; }
    Value* shape_slot_ptr(uint32_t i) const { return butterfly_ + i; }
    Value* element_ptr(uint32_t i) const { return butterfly_ - (sizeof(ButterflyHeader) / sizeof(Value)) - 1 - i; }
    // Amortized-doubling growth (like std::vector) so at least `needed`
    // shape slots resp. elements exist. New slots are NOT zero-initialized
    // -- callers must fill every newly-visible slot before it's traced or read.
    void ensure_shape_capacity(uint32_t needed);
    void ensure_elements_capacity(uint32_t needed);
    // std::vector<Value>::resize(new_length) equivalent.
    void resize_elements(uint32_t new_length);
    // Common core of both: allocates a new block sized for the given
    // capacities, copies both regions' existing contents across (at their
    // OLD capacities -- the caller has already established the new ones
    // are >=), frees the old block, and repoints butterfly_.
    void realloc_butterfly(uint32_t new_elements_capacity, uint32_t new_shape_capacity);
    void free_butterfly();
    // Shared tail of free_butterfly()/realloc_butterfly(): returns the block
    // to SmallMapPool without touching butterfly_header()->extras --
    // free_butterfly() deletes it first (real ownership release, object is
    // going away), realloc_butterfly() transplants it to the new header
    // first (still owned, just relocated to a bigger/smaller block).
    void release_butterfly_block();

public:
    // GC cell protocol: every Object (and subclass) lives in the active
    // Heap's block space. `delete` is an explicit free back to the block
    // free-list, which keeps existing unique_ptr ownership correct while
    // release()'d cells wait for the collector.
    static void* operator new(size_t size);
    static void  operator delete(void* p) noexcept;
    static void* operator new[](size_t) = delete;
    static void  operator delete[](void*) = delete;

    Object(ObjectType type = ObjectType::Ordinary);
    explicit Object(Object* prototype, ObjectType type = ObjectType::Ordinary);
    // Non-virtual: Object carries no vtable at all. The GC sweep
    // (Collector.cpp run_sweep(), CellKind::Object case) switches on
    // get_type() and destroys through the correct concrete type instead --
    // Function/TypedArrayBase/CustomObjectBase keep their own small vtables
    // so THEIR subclasses still destruct correctly via ordinary virtual
    // dispatch from that one cast.
    ~Object();

    // Reports every cell reference this object holds to the collector.
    // Non-virtual: switch on get_type() dispatches to Function/TypedArray/
    // Custom (their own small vtables reach further subclasses) or directly
    // to the ~7 leaf classes with extra cell references (Map/Set/WeakMap/
    // WeakSet/WeakRef/FinalizationRegistry/Promise/Proxy/DataView);
    // trace_default() above is the plain-Object body, also the fallback
    // every override chains to instead of calling this (which would
    // re-enter the switch and recurse). Values captured only inside
    // std::function lambdas are invisible here -- such state must also
    // live in a property or traced field (the existing pin-property
    // discipline).
    void trace(Visitor& v);

    // Where this object's traced storage lives, for the collector's mark
    // pipeline to request the cache line ahead of time (Collector.cpp,
    // MarkVisitor::step). Everything trace() walks -- elements, shape slots,
    // the butterfly header -- sits in that block, reached by a load out of
    // this cell, so the miss on it is dependent on the miss on the cell and
    // cannot be hidden by prefetching the cell alone. Returned as an opaque
    // address on purpose: it is a prefetch hint and is never dereferenced
    // through this accessor.
    const void* gc_storage_hint() const { return butterfly_; }

    friend class Function;
    Object(const Object& other) = delete;
    Object& operator=(const Object& other) = delete;
    // Never actually invoked (every Object lives behind a GC pointer) --
    // deleted rather than defaulted now that butterfly_ is a raw owned
    // pointer a default move would shallow-copy (double-free risk).
    Object(Object&& other) = delete;
    Object& operator=(Object&& other) = delete;

    ObjectType get_type() const { return shape_.type(); }
    void set_type(ObjectType type) { shape_.set_type(type); }
    bool is_array() const { return get_type() == ObjectType::Array; }
    bool is_function() const { return get_type() == ObjectType::Function; }
    bool is_primitive_wrapper() const {
        return get_type() == ObjectType::String ||
               get_type() == ObjectType::Number ||
               get_type() == ObjectType::Boolean;
    }

    bool is_array_buffer() const { return get_type() == ObjectType::ArrayBuffer; }
    bool is_typed_array() const { return get_type() == ObjectType::TypedArray; }
    bool is_data_view() const { return get_type() == ObjectType::DataView; }
    // ArrayBuffer and SharedArrayBuffer share ObjectType::ArrayBuffer (no
    // separate tag) -- out-of-line in Object.cpp, which already includes
    // ArrayBuffer.h, so it can check ArrayBuffer::is_shared() directly.
    bool is_shared_array_buffer() const;

    // Only Proxy overrides this (getPrototypeOf trap) -- out-of-line in
    // Object.cpp, which already includes ProxyReflect.h.
    Object* get_prototype() const;
    // Non-virtual: reads the internal [[Prototype]] slot directly, bypassing Proxy's getPrototypeOf trap.
    // For internal bookkeeping (e.g. checking whether a freshly-constructed object already has a prototype) where invoking a user trap would be observably wrong.
    Object* get_prototype_raw() const { return proto_; }
    void set_prototype(Object* prototype);
    // set_prototype for an object that has not been handed to JS yet: no site
    // can have cached a lookup through a chain no one has seen, so this skips
    // the proto_epoch bump. Only call it before the object escapes.
    void initialize_prototype(Object* prototype);
    bool has_prototype(Object* prototype) const;
    
    // Non-virtual: switch on get_type() dispatches to TypedArray/Proxy/Custom
    // (CustomObjectBase); *_default() below is the plain-Object body, also
    // the fallback every override chains to instead of calling these two
    // (which would re-enter the switch and recurse).
    bool has_property(const std::string& key) const;
    bool has_own_property(const std::string& key) const;
    bool has_private_slot(const std::string& key) const;
    std::string find_private_slot_key(const std::string& prefix) const;
    Value get_private_slot_value(const std::string& key) const;
    // Direct storage pointer for a plain data field in sparse overflow, or
    // nullptr (absent, or descriptor-based: accessors/statics need the full
    // path). Backbone of the VM's private inline cache -- the pointer is only
    // used immediately, never stored.
    Value* private_field_slot(const std::string& key);
    bool get_private_slot_descriptor(const std::string& key, PropertyDescriptor& out) const;
    void set_private_slot_value(const std::string& key, const Value& value);
    void add_private_field(const std::string& key, const Value& value = Value());

    // Non-virtual, see has_property()'s comment above for the pattern.
    Value get_property(const std::string& key) const;
    Value get_property(const Value& key) const;
    Value get_own_property(const std::string& key) const;

    bool set_property(const std::string& key, const Value& value, PropertyAttributes attrs = PropertyAttributes::Default);
    bool set_property(const Value& key, const Value& value, PropertyAttributes attrs = PropertyAttributes::Default);
    // Engine bookkeeping parked on an object: writable and configurable like an
    // ordinary property, but never enumerable. for-in and JSON.stringify visit
    // only enumerable own keys, so this is what keeps them out of both without
    // anyone filtering by name -- a filter that also swallowed every user key
    // that happened to start with two underscores.
    bool set_internal_property(const std::string& key, const Value& value) {
        return set_property(key, value,
            static_cast<PropertyAttributes>(PropertyAttributes::Writable |
                                            PropertyAttributes::Configurable));
    }
    // What the spec calls an internal slot: state a builtin keeps on an
    // object, with no key in any shape, no attributes, and no way to reach
    // it from script -- getOwnPropertyNames, Object.keys, defineProperty and
    // delete all behave as if it were not there, and script cannot forge one
    // to impersonate the object a builtin expects. set_internal_property
    // above only takes bookkeeping out of *enumeration*; this takes it off
    // the property table entirely, and is where bookkeeping belongs unless
    // something outside the engine genuinely has to see it.
    void set_internal_slot(const std::string& key, const Value& value);
    Value get_internal_slot(const std::string& key) const;
    bool has_internal_slot(const std::string& key) const;
    void delete_internal_slot(const std::string& key);
    bool ordinary_set(const std::string& key, const Value& value);
    // CreateDataProperty (spec 7.3.5): installs an own, Default-attrs data
    // property WITHOUT consulting the prototype chain -- unlike set_property()
    // (which implements [[Set]] semantics: inherited accessors, Proxy traps,
    // non-writable checks), CreateDataProperty never looks at the prototype
    // chain at all. Caller must already know `key` has no own property on
    // `this` (true by construction right after Op::CreateObject).
    bool create_own_data_property(const std::string& key, const Value& value);
    bool delete_property(const std::string& key);
    void remove_own_property(const std::string& key); // force-remove ignoring configurable
    
    // Only Proxy overrides this -- out-of-line in Object.cpp.
    Value get_element(uint32_t index) const;

    // fp: unchecked array access
    inline Value get_element_unchecked(uint32_t index) const {
        return *element_ptr(index);
    }

    // Moves `count` elements from index `src` to index `dst` within the dense
    // store, overlapping ranges included. Callers must have established
    // has_only_dense_elements() and that both ranges lie inside
    // element_count(); nothing here checks, which is the whole point -- the
    // keyed form of the same move builds an index string per element and looks
    // it up, and that is what a shift or an unshift actually costs.
    void move_elements(uint32_t dst, uint32_t src, uint32_t count);

    // Copies a run out of another array's dense store, growing this one's to
    // fit. The source must have passed has_only_dense_elements() with the run
    // inside its element_count(); this side is meant to be an array the caller
    // just created, which nothing else can observe yet.
    //
    // One write barrier covers the whole run. That is the point of both of
    // these: set_element pays a barrier, a receiver-type test and a
    // per-index descriptor probe on every single element, and on a bulk copy
    // the first two are loop invariant while the third is exactly what the
    // dense gate already answered.
    void copy_elements_from(const Object& src, uint32_t src_i, uint32_t dst_i, uint32_t count);
    // Same, in reverse order, which is a loop rather than a block move.
    void copy_elements_reversed_from(const Object& src, uint32_t count);

    // True when every index this object claims to have really is in the dense
    // element vector: a genuine Array, no per-index attributes, no holes,
    // nothing spilled to sparse storage, and length matching the storage. Only
    // then may get_element_unchecked stand in for a keyed read -- a hole has to
    // resolve against the prototype, and an index with a descriptor is not
    // where the raw slot says it is.
    // Inline on purpose: this gates every indexed access and every .length
    // read, and out of line in Object.cpp it was a real call from
    // Interpreter.cpp on each one (no LTO). The verified case is two loads and
    // a branch; everything that has to look at holes, sparse storage or the
    // length stays behind the out-of-line half below.
    bool has_only_dense_elements() const {
        if (get_type() != ObjectType::Array) return false;
        if (dense_verified()) return true;
        return dense_check_slow();
    }

    // True when nothing on this object's prototype chain carries an index
    // property. [[Set]] on an index the receiver does not own consults that
    // chain -- an inherited setter has to run, an inherited non-writable data
    // property has to block -- so a direct element write may only stand in for
    // it while the chain is clear.
    // has_only_dense_elements()'s uncached half; sets the flag when it passes.
    bool dense_check_slow() const;

    bool proto_chain_has_no_indices() const;

    bool set_element(uint32_t index, const Value& value);
    bool delete_element(uint32_t index);
    
    std::vector<std::string> get_own_property_keys() const;
    std::vector<std::string> get_enumerable_keys() const;
    // Only Function overrides this -- out-of-line in Object.cpp.
    std::vector<std::string> get_internal_property_keys() const;
    std::vector<std::string> get_own_property_keys_unfiltered() const;
    std::vector<uint32_t> get_element_indices() const;

    PropertyDescriptor get_property_descriptor(const std::string& key) const;
    bool set_property_descriptor(const std::string& key, const PropertyDescriptor& desc);
    
    bool is_extensible() const;
    void prevent_extensions();
    void reopen_extensible();
    void seal();
    void freeze();
    bool is_sealed() const;
    bool is_frozen() const;

    // Set once (never cleared) the first time set_prototype() installs this
    // object as someone's [[Prototype]] -- gates whether this object's own
    // mutations need to bump proto_epoch(), so ordinary objects never pay it.
    static constexpr uintptr_t kUsedAsPrototype = 0x02;
    bool used_as_prototype() const { return proto_.flag(kUsedAsPrototype); }
    void mark_used_as_prototype() { proto_.set_flag(kUsedAsPrototype); }

    // Sticky: set the first time any descriptors_ entry is made under an index
    // key. Without it every element read and write has to build the index's
    // string form just to ask whether such an entry exists -- and it almost
    // never does, while `length` keeps descriptors_ itself non-empty on every
    // array. Only ever set, never cleared except by clear_properties, so a
    // stale true costs one redundant probe rather than a wrong answer.
    static constexpr uintptr_t kHasIndexDescriptor = 0x04;
    bool has_index_descriptor() const { return proto_.flag(kHasIndexDescriptor); }
    void note_descriptor_key(const std::string& key) {
        if (!proto_.flag(kHasIndexDescriptor) && is_array_index(key)) {
            proto_.set_flag(kHasIndexDescriptor);
            invalidate_dense();  // an index's value no longer lives where the vector says
        }
    }
    
    // Only Proxy overrides this -- out-of-line in Object.cpp.
    uint32_t get_length() const;
    void set_length(uint32_t length);
    bool set_array_length_coerced(uint32_t new_length);
    // True when this array's length is the ordinary header-backed one, with no
    // attribute recorded for it by defineProperty. Only then is a direct store
    // what a keyed write would have done: a length made non-writable has to
    // refuse, and the keyed path is what knows to refuse.
    bool has_plain_array_length() const;
    void push(const Value& value);
    Value pop();
    void unshift(const Value& value);
    Value shift();
    
    std::unique_ptr<Object> map(Function* callback, Context& ctx, const Value& thisArg = Value());
    std::unique_ptr<Object> filter(Function* callback, Context& ctx, const Value& thisArg = Value());
    void forEach(Function* callback, Context& ctx, const Value& thisArg = Value());
    Value reduce(Function* callback, const Value& initial_value, Context& ctx);
    Value reduceRight(Function* callback, const Value& initial_value, Context& ctx);
    std::unique_ptr<Object> flat(uint32_t depth = 1);
    std::unique_ptr<Object> flatMap(Function* callback, Context& ctx, const Value& thisArg = Value());
    Object* copyWithin(int32_t target, int32_t start, int32_t end = -1);
    Value findLast(Function* callback, Context& ctx, const Value& thisArg = Value());
    Value findLastIndex(Function* callback, Context& ctx, const Value& thisArg = Value());
    std::unique_ptr<Object> toSpliced(uint32_t start, uint32_t deleteCount, const std::vector<Value>& items);
    Object* fill(const Value& value, int32_t start = 0, int32_t end = -1);
    std::unique_ptr<Object> toSorted(Function* compareFn, Context& ctx);
    std::unique_ptr<Object> with_method(uint32_t index, const Value& value);
    Value at(int32_t index);
    std::unique_ptr<Object> toReversed();

    Value groupBy(Function* callback, Context& ctx);
    
    Value to_primitive(const std::string& hint = "") const;
    std::string to_string() const;
    double to_number() const;
    bool to_boolean() const;
    
    size_t element_count() const { return elements_length(); }

    // VM inline-cache fast path. A shape match alone doesn't prove "plain
    // data slot, no override" -- defineProperty can attach non-default
    // attributes to an existing shape-mode key without changing the shape.
    // Callers must also check has_descriptor_override(key) before trusting
    // the slot value.
    Shape* get_shape() const { return shape_.get(); }
    const Value* get_shape_slot_unchecked(uint32_t index) const {
        return index < shape_capacity() ? shape_slot_ptr(index) : nullptr;
    }
    Value* get_shape_slot_unchecked(uint32_t index) {
        return index < shape_capacity() ? shape_slot_ptr(index) : nullptr;
    }
    // Out-of-line: descriptors_'s value type (PropertyDescriptor) is only
    // forward-declared this early in the header.
    bool has_descriptor_override(const std::string& key) const;
    // Whether ANY key on this object could have an override. False is the
    // strong answer the inline cache wants: no map, so no override, for any
    // key, right now -- a per-object fact, unlike the global epoch, which only
    // records that no object had one at the moment an entry was learned.
    bool has_any_descriptor_override() const;
    // One shape probe for "is `key` a plain own data property of this ordinary
    // object, readable straight from its slot?". Answers false for anything
    // that needs the general path -- an accessor, a descriptor override, a
    // non-ordinary object -- so a false is never a claim that the key is
    // absent, only that this shortcut cannot serve it.
    bool try_read_own_data_slot(const std::string& key, Value& out) const;
    // try_read_own_data_slot's slot index, for a caller that wants to cache it.
    bool cacheable_data_slot(const std::string& key, uint32_t& slot_index) const;
    // Single descriptors_ lookup shared by get_named's cacheable-gate and
    // accessor branch -- calling has_descriptor_override() then
    // get_property_descriptor() back to back re-scans the same map for the
    // same key with no mutation in between.
    PropertyDescriptor* find_descriptor_override(const std::string& key) const;

    // SetNamed's transition-cache fast path: adds `key` using an
    // already-resolved destination shape, skipping both Shape::transition(key)'s
    // hash lookup and the prototype-chain walk. Caller guarantees every
    // precondition store_in_overflow's "new property" branch would have
    // checked (Ordinary, extensible, no descriptor override, shape lacks key).
    void add_shape_property_cached(const std::string& key, const Value& value, Shape* to_shape);

    // Accessor sibling of add_shape_property_cached: `to_shape` must come
    // from Shape::transition_accessor(key), reserving two consecutive
    // shape_slots_ entries (getter, then setter -- either may be Value(),
    // an absent half). Never touches descriptors_ at all -- this is the
    // whole point (see get_property_descriptor's shape-accessor-slot
    // branch for the read side). Caller guarantees key has no existing own
    // property and this object is Ordinary/extensible.
    void add_accessor_shape_property_cached(const std::string& key, const Value& getter,
                                             const Value& setter, Shape* to_shape);

protected:
    // Plain-Object bodies of the switch-dispatched methods above -- every
    // override (TypedArrayBase, Proxy, Function, CustomObjectBase's own
    // Custom-family defaults, ModuleNamespaceObject/DeferredNamespaceObject)
    // chains to these instead of the public switch-dispatched names, or it
    // would recurse straight back into its own case.
    bool has_property_default(const std::string& key) const;
    bool has_own_property_default(const std::string& key) const;
    Value get_property_default(const std::string& key) const;
    bool set_property_default(const std::string& key, const Value& value, PropertyAttributes attrs = PropertyAttributes::Default);
    bool delete_property_default(const std::string& key);
    std::vector<std::string> get_own_property_keys_default() const;
    std::vector<std::string> get_enumerable_keys_default() const;
    PropertyDescriptor get_property_descriptor_default(const std::string& key) const;
    bool set_property_descriptor_default(const std::string& key, const PropertyDescriptor& desc);
    // Base trace body (prototype + element/shape-slot/overflow/descriptor
    // storage); every override chains to this instead of trace() (which would
    // re-enter the switch and recurse).
    void trace_default(Visitor& v);

    void ensure_element_capacity(uint32_t capacity);
    void compact_elements();

    bool store_in_overflow(const std::string& key, const Value& value);

    // Shape-mode fast-path helpers (named, non-index keys only -- see
    // shape_/shape_slots_). Every one is a no-op/miss once shape_ is
    // nullptr (object already migrated to dictionary mode).
    Value* find_shape_slot(const std::string& key);
    const Value* find_shape_slot(const std::string& key) const;
    // Adds or updates `key`. False means a NEW key would exceed the shape's
    // transition cap -- caller must migrate_to_dictionary_mode() and store
    // it there instead.
    bool set_shape_slot(const std::string& key, const Value& value);
    bool has_shape_slot(const std::string& key) const;
    // Moves every shape-mode property into descriptors_ (default attributes,
    // unless the key already has a descriptor entry -- then only the data
    // value is synced, so real attributes and accessors survive) and clears
    // shape_/shape_slots_. One-way: an object that migrates never re-enters
    // shape mode. Triggered by delete or a shape-cap miss.
    void migrate_to_dictionary_mode();
    // ArraySetLength side-effect helper: bumps the stored "length" value in
    // place (shape slot or, post-migration, descriptors_) iff candidate is larger.
    void bump_array_length(double candidate);

public:
    void clear_properties();
    // Pre-sizes shape-mode slot storage when the property count is known at
    // creation (object literals, class field lists) so the first N adds
    // don't reallocate. Capacity only -- the shape chain itself still grows
    // one transition per property.
    void reserve_property_slots(size_t count);

private:

    static thread_local std::unordered_map<std::string, std::string> interned_keys_;
    static const std::string& intern_key(const std::string& key);

    bool is_array_index(const std::string& key, uint32_t* index = nullptr) const;
    PropertyDescriptor create_data_descriptor(const Value& value, PropertyAttributes attrs) const;

    // Would growing the dense elements_ region to cover `index` (from the
    // current `old_size`) allocate something wildly out of proportion to
    // what's actually being stored? Either the absolute index is huge, or --
    // the more common trap -- this ONE write would create a huge gap of
    // holes relative to what's already there (e.g. writing index 999999 on
    // an otherwise-empty object: one assignment, megabytes of dense storage
    // for a single value). Both are the same waste/DoS vector; callers fall
    // back to store_in_overflow (sparse_overflow_) instead of resizing.
    static bool sparse_growth_too_costly(uint32_t index, uint32_t old_size);

    // RareExtras lives in the butterfly header (ButterflyHeader::extras)
    // instead of its own Object field -- peek_extras() mirrors the old bare
    // `if (extras_)` check (nullptr if no butterfly, or a butterfly with no
    // RareExtras yet); ensure_extras() allocates a header-only butterfly
    // first if needed, then the RareExtras itself on first use.
    RareExtras* peek_extras() const { return butterfly_ ? butterfly_header()->extras : nullptr; }
    RareExtras& ensure_extras();

    // RareExtras accessors: "peek" forms return nullptr without allocating
    // (mirrors the old field's bare `if (sparse_overflow_)` check); "ensure"
    // forms allocate (and the specific sub-member) on first use, matching
    // the old `if (!sparse_overflow_) sparse_overflow_ =
    // std::make_unique<...>();` pattern.
    std::unordered_map<std::string, Value>* sparse_overflow() const;
    std::unordered_map<std::string, Value>& ensure_sparse_overflow();
    std::unordered_set<uint32_t>* deleted_elements() const;
    std::unordered_set<uint32_t>& ensure_deleted_elements();
    HybridDescriptorMap* descriptors() const;
    HybridDescriptorMap& ensure_descriptors();
    std::unordered_map<std::string, Value>* internals() const;
    std::unordered_map<std::string, Value>& ensure_internals();
    // Enumeration order for extras-resident (sparse/dictionary-mode)
    // properties only -- shape-resident property order comes from
    // Shape::properties_in_order() instead, see get_own_property_keys.
    void push_extra_property_order(const std::string& key);
    void erase_extra_property_order(const std::string& key);
    // Shared by get_own_property_keys/get_own_property_keys_unfiltered:
    // merges shape-resident and extras-resident property order by logical-
    // clock snapshot, since a plain concatenation can misorder the two.
    // Named (non-index) keys only; numeric/element keys are handled
    // separately by both callers.
    void collect_named_keys_in_order(std::vector<std::string>& out) const;
};

// Common base for every ObjectType::Custom-tagged class (Generator,
// AsyncGenerator, AsyncIterator, Iterator + its 4 subclasses,
// ModuleNamespaceObject, DeferredNamespaceObject) -- ObjectType::Custom alone
// can't tell these apart (ModuleNamespaceObject isn't even Iterator-derived),
// so the GC sweep destroys every Custom-tagged cell through THIS class's
// virtual destructor instead: one static_cast + virtual call here correctly
// reaches whichever concrete type it actually is, without Object itself
// needing a vtable. Existing dynamic_cast<Generator*> etc. call sites are
// unaffected (Generator is still, transitively, a CustomObjectBase).
class CustomObjectBase : public Object {
public:
    // Distinguishes the concrete type sharing ObjectType::Custom, without a
    // vtable: trace()/destructor/the 9 property-access methods all switch
    // on this instead of virtual dispatch (a vtable here would misalign the
    // Object subobject, same reasoning as Object.h's own note on why Object
    // itself carries no vtable). ModuleNamespace/DeferredNamespace are
    // otherwise .cpp-local classes (ModuleLoader.cpp/language.cpp) --
    // moved into ModuleLoader.h so this switch (defined in Object.cpp) can
    // name them directly.
    enum class CustomKind : uint8_t {
        Generator, AsyncGenerator, AsyncIterator,
        ArrayIterator, StringIterator, MapIterator, SetIterator,
        ModuleNamespace, DeferredNamespace
    };

private:
    CustomKind custom_kind_ = CustomKind::Generator;
protected:
    void set_custom_kind(CustomKind kind) { custom_kind_ = kind; }
public:
    CustomKind get_custom_kind() const { return custom_kind_; }

    using Object::Object;
    // Non-virtual: the GC sweep (Collector.cpp) reads get_custom_kind() and
    // destructs through the correct concrete type itself, same pattern as
    // Object's own destructor dispatch.
    ~CustomObjectBase() = default;

    // Non-virtual: switches on get_custom_kind() to reach Generator/
    // AsyncGenerator/ArrayIterator/MapIterator/SetIterator's own extra cell
    // references (AsyncIterator/StringIterator/ModuleNamespace/
    // DeferredNamespace hold none and use trace_default() as-is).
    void trace(Visitor& v);

    // Property-access hooks for this subtree specifically -- Object's own
    // switch dispatches ObjectType::Custom here, then this switches again
    // on get_custom_kind(). Only ModuleNamespaceObject/DeferredNamespaceObject
    // actually override any of these nine (Generator/AsyncGenerator/
    // AsyncIterator/Iterator's own subclasses don't need to) -- every other
    // kind falls through to the plain-Object *_default() body.
    bool has_property(const std::string& key) const;
    bool has_own_property(const std::string& key) const;
    Value get_property(const std::string& key) const;
    bool set_property(const std::string& key, const Value& value, PropertyAttributes attrs = PropertyAttributes::Default);
    bool delete_property(const std::string& key);
    std::vector<std::string> get_own_property_keys() const;
    std::vector<std::string> get_enumerable_keys() const;
    PropertyDescriptor get_property_descriptor(const std::string& key) const;
    bool set_property_descriptor(const std::string& key, const PropertyDescriptor& desc);
};

/**
 * Property descriptor for defineProperty operations
 */
class PropertyDescriptor {
public:
    enum Type {
        Data,
        Accessor,
        Generic
    };

private:
    Type type_;
    Value value_;
    Object* getter_;
    Object* setter_;
    PropertyAttributes attributes_;
    bool has_value_ : 1;
    bool has_getter_ : 1;
    bool has_setter_ : 1;
    bool has_writable_ : 1;
    bool has_enumerable_ : 1;
    bool has_configurable_ : 1;

public:
    PropertyDescriptor();
    explicit PropertyDescriptor(const Value& value, PropertyAttributes attrs = PropertyAttributes::Default);
    PropertyDescriptor(Object* getter, Object* setter, PropertyAttributes attrs = PropertyAttributes::Default);

    Type get_type() const { return type_; }
    bool is_data_descriptor() const { return type_ == Data; }
    bool is_accessor_descriptor() const { return type_ == Accessor; }
    bool is_generic_descriptor() const { return type_ == Generic; }

    const Value& get_value() const { return value_; }
    void set_value(const Value& value);
    
    Object* get_getter() const { return getter_; }
    void set_getter(Object* getter);
    
    Object* get_setter() const { return setter_; }
    void set_setter(Object* setter);

    PropertyAttributes get_attributes() const { return attributes_; }
    bool is_writable() const { return attributes_ & PropertyAttributes::Writable; }
    bool is_enumerable() const { return attributes_ & PropertyAttributes::Enumerable; }
    bool is_configurable() const { return attributes_ & PropertyAttributes::Configurable; }
    
    void set_writable(bool writable);
    void set_enumerable(bool enumerable);
    void set_configurable(bool configurable);

    bool has_value() const { return has_value_; }
    bool has_getter() const { return has_getter_; }
    bool has_setter() const { return has_setter_; }
    bool has_writable() const { return has_writable_; }
    bool has_enumerable() const { return has_enumerable_; }
    bool has_configurable() const { return has_configurable_; }

    bool is_complete() const;
    void complete_with_defaults();
    PropertyDescriptor merge_with(const PropertyDescriptor& other) const;

    std::string to_string() const;
};

// Replaces Object::descriptors_'s old unordered_map<string, PropertyDescriptor>.
// Most objects that ever need this store 1-2 entries (a single accessor from
// an object-literal getter, or a non-default-attribute override) -- paying
// for a real hashtable (bucket-array malloc + per-node allocs + hashing the
// same key on every op) for that common case dominated a real profile
// (tests/benchmark/object_literals.js). kInlineCapacity entries are stored
// inline (linear-scanned, no allocation beyond the map object itself);
// exceeding that spills ALL entries into a heap unordered_map (this is also
// where migrate_to_dictionary_mode() lands -- a real dictionary-mode object
// can hold many entries, so the spilled path must stay hashtable-fast).
//
// Weaker guarantee than unordered_map: erase() may relocate OTHER, unerased
// inline entries (compaction), and operator[]'s spillover relocates every
// inline entry into the new heap map. No PropertyDescriptor*/& obtained from
// this class may be held across another call into it, or across any call
// that could run arbitrary JS (Function::call) -- Object.cpp's callers were
// specifically audited for this (see the capture-before-call/re-find-after
// pattern in get_property_descriptor/set_property_descriptor).
class HybridDescriptorMap {
public:
    static constexpr size_t kInlineCapacity = 4;

    // Pooled: fixed sizeof (no virtuals, no variable-length tail), never
    // GC-managed (always reached via Object::descriptors_'s unique_ptr on
    // the plain C++ heap) -- a single, permanent SmallMapPool size class.
    static void* operator new(std::size_t sz) { return SmallMapPool::take(sz); }
    static void operator delete(void* p, std::size_t sz) noexcept { SmallMapPool::give(sz, p); }

    // Pooled allocator (SmallMapPool): every consumer of overflow_ (find/
    // find_if/Object::trace's GC loop) either returns a fresh copy or uses
    // the pointer within the same call, never caches it across calls, so
    // swapping the allocator carries none of the pointer-caching risk
    // Environment::slots_ had to be checked for.
    using OverflowMap = std::unordered_map<std::string, PropertyDescriptor, std::hash<std::string>,
                                            std::equal_to<std::string>,
                                            SmallMapAllocator<std::pair<const std::string, PropertyDescriptor>>>;

    // A summary of the keys present, one bit each. Two thirds of the lookups
    // this map is asked to do are for a key it does not hold, and answering
    // those took a full string hash and a probe; the summary answers them
    // from the length and the two end bytes. It can only ever say "maybe" --
    // a key that is present always has its bit set -- so a hit still does the
    // real lookup and a miss is exact.
    static uint64_t key_bit(const std::string& key) {
        const size_t n = key.size();
        if (n == 0) return 1ull;
        const unsigned char a = static_cast<unsigned char>(key[0]);
        const unsigned char b = static_cast<unsigned char>(key[n - 1]);
        return 1ull << ((n * 31u + a * 7u + b) & 63u);
    }

    PropertyDescriptor* find(const std::string& key) {
        if (!(key_bits_ & key_bit(key))) return nullptr;
        for (size_t i = 0; i < inline_count_; i++) {
            if (*inline_[i].key == key) return &inline_[i].desc;
        }
        if (overflow_) {
            auto it = overflow_->find(key);
            if (it != overflow_->end()) return &it->second;
        }
        return nullptr;
    }
    const PropertyDescriptor* find(const std::string& key) const {
        return const_cast<HybridDescriptorMap*>(this)->find(key);
    }
    bool count(const std::string& key) const { return find(key) != nullptr; }

    PropertyDescriptor& operator[](const std::string& key) {
        if (PropertyDescriptor* existing = find(key)) return *existing;
        key_bits_ |= key_bit(key);
        if (!overflow_ && inline_count_ < kInlineCapacity) {
            inline_[inline_count_].key = Shape::intern(key);
            return inline_[inline_count_++].desc;
        }
        if (!overflow_) {
            // Spillover: move every inline entry into a fresh heap map, then
            // fall through to insert the new key there too.
            overflow_ = std::make_unique<OverflowMap>();
            for (size_t i = 0; i < inline_count_; i++) {
                (*overflow_)[*inline_[i].key] = inline_[i].desc;
            }
            inline_count_ = 0;
        }
        return (*overflow_)[key];
    }

    // erase leaves the bit set. The summary is allowed to claim a key that is
    // gone -- that only costs a lookup which then fails -- but never to deny
    // one that is present.
    bool erase(const std::string& key) {
        for (size_t i = 0; i < inline_count_; i++) {
            if (*inline_[i].key == key) {
                // Swap-with-last to compact -- relocates the last entry's
                // address if it isn't the one being erased.
                inline_[i] = std::move(inline_[inline_count_ - 1]);
                inline_count_--;
                return true;
            }
        }
        if (overflow_) return overflow_->erase(key) > 0;
        return false;
    }

    // Early-exits when pred returns true, writing the matching key to
    // *out_key. For scans that only need the first match (e.g. a prefix
    // search walked once per object up a prototype chain), avoiding a full
    // scan matters -- unlike GC tracing, which always visits every entry.
    bool find_if(const std::function<bool(const std::string&, const PropertyDescriptor&)>& pred,
                 std::string* out_key) const {
        for (size_t i = 0; i < inline_count_; i++) {
            if (pred(*inline_[i].key, inline_[i].desc)) {
                if (out_key) *out_key = *inline_[i].key;
                return true;
            }
        }
        if (overflow_) {
            for (const auto& kv : *overflow_) {
                if (pred(kv.first, kv.second)) {
                    if (out_key) *out_key = kv.first;
                    return true;
                }
            }
        }
        return false;
    }

    void clear() {
        inline_count_ = 0;
        overflow_.reset();
    }

    // Plain accessors for Object::trace's GC-hot loop -- a direct span +
    // overflow-map walk, no lambda/callback indirection in that path.
    size_t inline_size() const { return inline_count_; }
    const std::string& inline_key(size_t i) const { return *inline_[i].key; }
    const PropertyDescriptor& inline_value(size_t i) const { return inline_[i].desc; }
    const OverflowMap* overflow() const { return overflow_.get(); }

private:
    // The key is interned (Shape::intern, the pool Shape/Context/Environment
    // already share), so an entry carries an 8-byte pointer instead of a
    // 32-byte std::string. find()/erase() still take a plain, possibly
    // uninterned key and compare against the pointee by value -- interning on
    // a lookup would cost more than the compare it replaces. Only insertion
    // interns. Same split Shape::SlotMap uses.
    struct Entry {
        const std::string* key = nullptr;
        PropertyDescriptor desc;
    };
    std::array<Entry, kInlineCapacity> inline_;
    size_t inline_count_ = 0;
    std::unique_ptr<OverflowMap> overflow_;
    uint64_t key_bits_ = 0;
};

// Everything an object needs only rarely: sparse array-index overflow,
// deleted-element tombstones, non-default-attribute/accessor descriptors,
// and the enumeration order of whichever of those a given object actually
// has (shape-resident properties order via Shape::properties_in_order()
// instead). Bundled behind one ButterflyHeader::extras pointer (see
// Object::peek_extras/ensure_extras) instead of four separate fields.
struct RareExtras {
    std::unique_ptr<std::unordered_map<std::string, Value>> sparse_overflow;
    std::unique_ptr<std::unordered_set<uint32_t>> deleted_elements;
    std::unique_ptr<HybridDescriptorMap> descriptors;
    // (key, logical-insertion-clock) for extras-resident properties.
    // defineProperty can add one of these while the object is still
    // shape-mode for everything else, so plain concatenation would get
    // chronological order wrong; the clock (shape_->slot_count() at
    // insertion time, or next_order_snapshot once fully dictionary-mode)
    // lets get_own_property_keys interleave the two correctly.
    std::vector<std::pair<std::string, uint32_t>> extra_property_order;
    uint32_t next_order_snapshot = 0;
    // Engine bookkeeping a builtin parked on this object -- see
    // Object::set_internal_slot. Deliberately outside the property table
    // (and outside extra_property_order), so no reflective surface can
    // reach it and no enumeration has to skip it.
    std::unique_ptr<std::unordered_map<std::string, Value>> internals;
};

/**
 * JavaScript Function object implementation
 */
class Function : public Object {
public:
    enum class CallType {
        Normal,
        Constructor,
        Method
    };

    // Distinguishes AsyncFunction/GeneratorFunction/AsyncGeneratorFunction
    // from plain Function and from each other, without a vtable: call()/
    // trace()/the GC sweep's destructor dispatch all switch on this instead
    // of virtual dispatch (a Function-level vtable would misalign the
    // Object subobject, same reasoning as Object.h's own note on why Object
    // itself carries no vtable).
    enum class FunctionKind : uint8_t { Plain, Async, Generator, AsyncGenerator };

protected:
    void set_function_kind(FunctionKind kind) { function_kind_ = kind; }
public:
    FunctionKind get_function_kind() const { return function_kind_; }

private:
    // Shared, decl-site-scoped data (AST body/params, compiled bytecode,
    // decl-site-invariant caches) -- see FunctionExecutable's own doc
    // comment. Null only for native functions (no AST/decl site at all).
    ExecutableRef<const FunctionExecutable> executable_;
    class Context* closure_context_;
    class Environment* closure_environment_;  // lexical environment captured at creation time
    mutable Object* prototype_;  // Mutable to allow lazy initialization in get_property
    Value arrow_this_;   // see arrow_this(); fits the padding this class already had

    // Every single-bit/tag field on Function, packed into one 16-bit word
    // (13 bits used) instead of 12 separate byte-sized members scattered
    // across the class -- pure storage change, every existing accessor and
    // every direct internal read/write (is_arrow_ = true, if (is_strict_),
    // etc.) keeps compiling unchanged, since bit-field member access is
    // syntactically identical to a plain data member and none of these are
    // ever address-taken or bound to a reference. Declared consecutively
    // (no non-bitfield member in between) so the compiler packs them into
    // shared storage; this also removes the ~14 bytes of alignment padding
    // these used to force by being split across two separate byte clusters.
    FunctionKind function_kind_ : 2 = FunctionKind::Plain;
    bool is_native_ : 1 = false;
    // Whether calling this native could make the CALLER's context outlive the
    // call. Natives run on the caller's context rather than one of their own,
    // so one that stores it (queueMicrotask, a timer, a promise reaction)
    // leaves it reachable after the call returns. Conservative by default:
    // only a native proven to keep nothing clears it.
    bool native_captures_ctx_ : 1 = true;
    bool is_constructor_ : 1 = false;  // Whether this function has [[Construct]] internal method
    bool is_arrow_ : 1 = false;        // Arrow functions have lexical this binding
    bool is_class_constructor_ : 1 = false;  // Class constructors must be called with new
    bool is_strict_ : 1 = false;       // Function runs in strict mode (e.g. class methods)
    bool is_param_default_ : 1 = false;  // Created as a default param expression; uses param scope as outer env
    // "this function should have a .prototype, and it has not been built yet"
    // -- distinct from prototype_ == nullptr, which means it never has one
    // (arrows, methods, accessors). Mutable for the same reason prototype_ is.
    mutable bool prototype_pending_ : 1 = false;
    // Set ONLY by setup_mapped_arguments() on the getter/setter closures it
    // creates -- a C++-only trust bit (no public setter) so Object.cpp's
    // mapped-arguments fast paths can never be fooled by a JS-settable
    // property of the same name into invoking an attacker-authored Function.
    bool is_mapped_arguments_accessor_ : 1 = false;
    // Direct eval is the only caller that ever needs the receiver its caller
    // had, so only eval's own invocation has to preserve it. Every other
    // native call used to create and then delete a binding for it.
    bool is_eval_native_ : 1 = false;
    // "name"/"length" are lazy: no real descriptor/shape-slot is installed at
    // construction (see get_property/get_property_descriptor/has_own_property
    // overrides below) -- these track whether each has been explicitly
    // deleted, since a never-installed and a deleted property must be told
    // apart (the former still virtually reads as present, the latter must
    // not).
    bool name_deleted_ : 1 = false;
    bool length_deleted_ : 1 = false;
    // This instance's name is the empty string, whatever the declaration site
    // it shares says. A method keyed by a description-less symbol is named ""
    // by SetFunctionName, and so is a literal instantiated in NamedEvaluation
    // position before the name arrives; both used to record it as a
    // per-instance override, which meant allocating an instance data block per
    // closure to hold nothing. A bit here says the same thing for free.
    bool name_is_empty_ : 1 = false;
    // An arrow's captured `this` used to be stored as a property named
    // __arrow_this__, which every call had to look up by that string twice
    // and which `in` and hasOwnProperty could both see -- Node reports
    // neither. It lives in arrow_this_ below instead; this says it was
    // captured at all.
    bool has_arrow_this_ : 1 = false;
    // Set eagerly the moment a genuine (non-const-marker) __closure_ property
    // is installed (see Function::set_property) -- has_closure_props()
    // becomes an O(1) check instead of scanning every own property key on
    // demand, which used to run on EVERY first call to EVERY function: fresh,
    // single-use closures never benefit from executable_'s closure_props_state
    // cache (there's no second call to pay it off). Monotonic: only ever
    // flips false->true, never reset -- if the property is later deleted
    // (astronomically unlikely for this mangled internal name), the flag
    // stays stale-true, which only costs one redundant scan in
    // Function::call's own install-check below, not a correctness issue.
    bool has_closure_props_hint_ : 1 = false;
    // Per-instance IC state, used in place of BytecodeChunk::lookup_cache/
    // private_feedback when the executable's bytecode_chunk is shared --
    // instances differ in captured environment/declaring brand, so a
    // chunk-level cache would serve stale results across sibling instances
    // (see lookup_cache_data/private_feedback_data in Interpreter.cpp).
    // Bundled behind one pointer (mirrors V8's own JSFunction::feedback_cell,
    // a single per-closure IC vector covering every kind of inline cache) --
    // an unused vector inside costs only its own empty 24-byte header (no
    // heap allocation for elements until actually resized), so a function
    // needing only one of the two pays a small, fixed cost for the other's
    // header rather than a full separate allocation. Null until first call;
    // each vector keeps using the pooled allocator (see Interpreter.cpp's
    // lookup_cache_data comment) once allocated, resized exactly once per
    // instance.
    struct InstanceFeedback {
        std::vector<BytecodeChunk::LookupCacheEntry,
            SmallMapAllocator<BytecodeChunk::LookupCacheEntry>> lookup_cache;
        std::vector<PrivateFeedback, SmallMapAllocator<PrivateFeedback>> private_feedback;
    };
    // Per-instance overrides for get_source_text()/get_name(): the common
    // case (decl-site defaults, identical for every instance sharing one
    // executable) lives on executable_->source_text/name instead -- see
    // set_source_text()/assign_decl_site_name(). Bundled together because
    // both represent the exact same kind of rare event -- a genuine
    // per-instance deviation from the shared decl-site default -- so
    // whichever one fires, the other was already about equally likely to;
    // has_source_text/has_name distinguish "never overridden" from
    // "overridden to an empty string"
    // (the pointer's own null-ness only means "neither override has ever
    // been needed on this instance").
    struct InstanceOverrides {
        std::string source_text;
        bool has_source_text = false;
        std::string name;
        bool has_name = false;
    };
    // Bundles native-only per-instance state: all three fields only exist
    // for native functions (is_native_, no executable_ to derive a decl-site
    // default from -- natives never share, so there's no sharing benefit to
    // a separate override).
    struct NativeFunctionData {
        std::function<Value(Context&, const std::vector<Value>&)> fn;
        size_t declared_length = 0;
        std::string name;
    };
    // InstanceOverrides/InstanceFeedback above only exist for non-native
    // functions (need executable_ to have a decl-site default to deviate
    // from / share a chunk with); NativeFunctionData above only exists for
    // native ones (is_native_, no executable_ at all). is_native_ never
    // changes after construction, so these two shapes are truly mutually
    // exclusive for the whole lifetime of any one instance -- one tagged
    // pointer covers both, discriminated by the is_native_ bit the class
    // already carries (no separate tag needed, unlike e.g. std::variant,
    // which would cost an extra word for its own discriminant and erase
    // this saving entirely). Manual new/delete instead of unique_ptr: the
    // pointee's static type depends on a runtime flag, not a compile-time
    // one, so ~Function() below does the delete by hand instead.
public:
    // The spec's internal slots for a method or class constructor. These used
    // to be string properties on the function object, which also left
    // `"__super_constructor__" in C` answering true where V8 says false.
    struct ClassSlots {
        Object* home_object = nullptr;      // [[HomeObject]]
        Function* super_ctor = nullptr;     // the parent constructor super() calls
        Object* private_brands = nullptr;
        std::string pm_brand_slot;
        bool is_default_ctor = false;
        bool super_is_null = false;         // `extends null`: derived, but super() cannot succeed
        bool is_static_method = false;
    };
private:
    // Only `feedback` is held inline. The other two are for rare events (a
    // per-instance name/source override, and the class-related slots a plain
    // function has none of), and carry a std::string each, so keeping them
    // inline made every instance that merely caches a name lookup pay for
    // both. They are allocated when something actually writes one.
    struct NonNativeInstanceData {
        InstanceFeedback feedback;
        std::unique_ptr<InstanceOverrides> overrides;
        std::unique_ptr<ClassSlots> class_slots;
    };
    mutable void* instance_data_ = nullptr;
    // Every one of these four checks is_native_ itself (not just
    // instance_data_'s null-ness) before casting -- the whole point of a
    // tagged union is that a wrong cast is silent type-confusion, not a
    // compile error, so the safety has to live in these four call sites
    // instead. ensure_native_data() must only ever be called when
    // is_native_ is already known true by the caller (the two native
    // constructors); ensure_instance_data() only when executable_ is
    // non-null (which already implies !is_native_, see the class-header
    // comment on executable_).
    NativeFunctionData& ensure_native_data() const {
        if (!instance_data_) instance_data_ = new NativeFunctionData();
        return *static_cast<NativeFunctionData*>(instance_data_);
    }
    NativeFunctionData* native_data() const {
        return (is_native_ && instance_data_) ? static_cast<NativeFunctionData*>(instance_data_) : nullptr;
    }
    NonNativeInstanceData& ensure_instance_data() const {
        if (!instance_data_) instance_data_ = new NonNativeInstanceData();
        return *static_cast<NonNativeInstanceData*>(instance_data_);
    }
    NonNativeInstanceData* instance_data() const {
        return (!is_native_ && instance_data_) ? static_cast<NonNativeInstanceData*>(instance_data_) : nullptr;
    }
    InstanceOverrides& ensure_overrides() const {
        NonNativeInstanceData& d = ensure_instance_data();
        if (!d.overrides) d.overrides = std::make_unique<InstanceOverrides>();
        return *d.overrides;
    }


    // Detection-only half of the __closure_* scan in Function::call (no
    // Context needed) -- used to resolve executable_'s closure_props_state
    // before a Context exists; the call-site scan still runs separately to
    // apply the bindings when this returns true.
    bool has_closure_props() const;

public:
    Function(const std::string& name,
             const std::vector<std::string>& params,
             std::unique_ptr<class ASTNode> body,
             class Context* closure_context,
             bool create_prototype = true);

    Function(const std::string& name,
             std::vector<std::unique_ptr<class Parameter>> params,
             std::unique_ptr<class ASTNode> body,
             class Context* closure_context,
             bool create_prototype = true);

    // Shares an already-built FunctionExecutable instead of wrapping a fresh
    // one -- every instance from the same decl site (e.g. a closure
    // recreated on each call of an enclosing function) takes this
    // constructor and reuses the identical body/params/compiled-chunk
    // instead of re-cloning the AST and recompiling.
    Function(const std::string& name,
             ExecutableRef<const FunctionExecutable> executable,
             class Context* closure_context,
             bool create_prototype = true);

    Function(const std::string& name,
             std::function<Value(Context&, const std::vector<Value>&)> native_fn,
             bool create_prototype = false);

    Function(const std::string& name,
             std::function<Value(Context&, const std::vector<Value>&)> native_fn,
             uint32_t arity,
             bool create_prototype = false);
    
    // Non-virtual: the GC sweep (Collector.cpp) reads get_function_kind()
    // and destructs through the correct concrete type itself, same pattern
    // as Object's own destructor dispatch. Out-of-line (Function.cpp):
    // manually deletes instance_data_ through whichever type is_native_
    // says it actually is (see that field's own doc comment) -- this is
    // the one place that has to know both shapes.
    ~Function();

    // Non-virtual: switches on get_function_kind() to reach AsyncFunction/
    // GeneratorFunction/AsyncGeneratorFunction's own extra cell references;
    // trace_default() below is the plain-Function body, also the fallback
    // each of those three chains to instead of calling this (which would
    // re-enter the switch and recurse).
    void trace(Visitor& v);

    const std::string& get_name() const {
        static const std::string empty;
        if (name_is_empty_) return empty;
        if (auto* d = instance_data()) { if (d->overrides && d->overrides->has_name) return d->overrides->name; }
        if (executable_) return executable_->name;
        auto* nd = native_data();
        return nd ? nd->name : empty;
    }
    void set_name(const std::string& name);
    // Populates the decl-site default (executable_->name) the first time, or
    // allocates a per-instance override if the executable already holds a
    // DIFFERENT value -- shared by the constructors (a fresh name at
    // construction) and set_name() (a later NamedEvaluation rename). No-op
    // for native functions (native_data()->name is set directly by whichever
    // constructor built it, never shared).
    void assign_decl_site_name(const std::string& name) {
        if (!executable_) return;
        // An empty name needs no storage of its own: the bit answers for it,
        // and it has to win over the declaration site, which a sibling
        // instance may already have filled in with a real name.
        if (name.empty()) {
            name_is_empty_ = true;
            if (executable_->name.empty()) return;
            if (auto* d = instance_data(); d && d->overrides) {
                d->overrides->name.clear();
                d->overrides->has_name = false;
            }
            return;
        }
        name_is_empty_ = false;
        if (executable_->name.empty()) { executable_->name = name; return; }
        if (executable_->name == name) {
            // Agrees with the shared decl-site value, so drop any override --
            // a later instantiation of a literal in NamedEvaluation position
            // is constructed anonymously and records an empty name here, which
            // would otherwise keep shadowing the shared one.
            if (auto* d = instance_data(); d && d->overrides) {
                d->overrides->name.clear();
                d->overrides->has_name = false;
            }
            return;
        }
        auto& overrides = ensure_overrides();
        overrides.name = name;
        overrides.has_name = true;
    }
    const std::vector<std::string>& get_parameters() const {
        static const std::vector<std::string> empty;
        return executable_ ? executable_->parameters : empty;
    }
    // Out-of-line (Function.cpp): the static empty-vector fallback's
    // destructor needs Parameter complete, which this header can't provide
    // (only forward-declared here to avoid a runtime->parser header cycle).
    const std::vector<std::unique_ptr<class Parameter>>& get_parameter_objects() const;
    const ASTNode* get_body() const { return ast_body(); }
    size_t get_arity() const { return get_parameters().size(); }
    bool is_native() const { return is_native_; }
    bool native_captures_ctx() const { return native_captures_ctx_; }
    // For natives that read and compute only: no closure over the context, no
    // queued job, nothing stored anywhere the call does not own.
    void mark_native_context_safe() { native_captures_ctx_ = false; }
    bool is_constructor() const { return is_constructor_; }
    void set_is_constructor(bool value) { is_constructor_ = value; }
    bool is_arrow() const { return is_arrow_; }
    // Captured at arrow creation, immutable for the arrow's life. Traced:
    // it can be the only reference left to the object `this` named.
    const Value& arrow_this() const { return arrow_this_; }
    bool has_arrow_this() const { return has_arrow_this_; }
    void set_arrow_this(const Value& v) { arrow_this_ = v; has_arrow_this_ = true; }
    class Context* get_closure_context() const { return closure_context_; }
    class Environment* get_closure_environment() const { return closure_environment_; }
    void set_closure_environment(class Environment* env);
    // The constructor captures closure_environment_'s POINTER unconditionally
    // (Function::call's fallback chain needs it regardless) but no longer
    // marks it escaped by itself -- callers that can't prove the closure is
    // capture-free (see closure_needs_outer_environment) call this
    // explicitly, preserving today's behavior exactly. A missed call here
    // only risks the environment being freed while still referenced --
    // never called when in doubt, see every non-optimized creation site.
    void mark_closure_environment_escaped() const;
    void set_is_arrow(bool value) { is_arrow_ = value; }
    bool is_class_constructor() const { return is_class_constructor_; }
    void set_is_class_constructor(bool value) { is_class_constructor_ = value; }
    bool is_strict() const { return is_strict_; }
    void set_is_strict(bool value) { is_strict_ = value; }
    bool is_param_default() const { return is_param_default_; }
    void set_is_param_default(bool v) { is_param_default_ = v; }
    bool is_mapped_arguments_accessor() const { return is_mapped_arguments_accessor_; }
    bool is_eval_native() const { return is_eval_native_; }
    void mark_eval_native() { is_eval_native_ = true; }
    // Decl-site-invariant class field count -- see FunctionExecutable::
    // construct_slot_hint's own doc comment. No-op for native functions
    // (never class constructors with instance fields).
    void set_construct_slot_hint(uint32_t count) { if (executable_) executable_->construct_slot_hint = count; }
    uint32_t get_construct_slot_hint() const { return executable_ ? executable_->construct_slot_hint : 0; }
    const std::string& get_source_text() const {
        if (auto* d = instance_data()) { if (d->overrides && d->overrides->has_source_text) return d->overrides->source_text; }
        static const std::string empty;
        return executable_ ? executable_->source_text : empty;
    }
    // Decl-site-invariant source text populates the shared executable's own
    // copy directly (safe: every instance from the same site sets the exact
    // same value, same idiom as strict_directive_state_ etc). A call that
    // would actually change an already-different value falls back to a
    // per-instance override instead of corrupting every sibling sharing
    // that executable. Native functions have no override home for this (no
    // NativeFunctionData field for it) and never consult get_source_text()
    // themselves (to_string() special-cases is_native_ before ever reaching
    // it), so this is simply a no-op for them rather than risk writing
    // through instance_data_ as the wrong pointee type.
    void set_source_text(const std::string& s) {
        if (executable_) {
            if (executable_->source_text.empty()) { executable_->source_text = s; return; }
            if (executable_->source_text == s) return;
        } else if (is_native_) {
            return;
        }
        auto& overrides = ensure_overrides();
        overrides.source_text = s;
        overrides.has_source_text = true;
    }

    // Lazily allocated + sized by the caller (Interpreter.cpp) to chunk.names.size().
    // Never called for native functions (they never reach Interpreter::run()).
    std::vector<BytecodeChunk::LookupCacheEntry,
        SmallMapAllocator<BytecodeChunk::LookupCacheEntry>>& instance_lookup_cache() const {
        return ensure_instance_data().feedback.lookup_cache;
    }

    // Lazily allocated + sized by the caller (Interpreter.cpp) to
    // chunk.ic_feedback->private_feedback.size() (0 if chunk.ic_feedback is null).
    // Never called for native functions (they never reach Interpreter::run()).
    std::vector<PrivateFeedback,
        SmallMapAllocator<PrivateFeedback>>& instance_private_feedback() const {
        return ensure_instance_data().feedback.private_feedback;
    }

    // The spec's internal slots (see ClassSlots). Reading never allocates: a
    // function that is neither a method nor a class constructor answers from
    // one shared empty instance, so the common call pays a null check.
    const ClassSlots& class_slots() const {
        static const ClassSlots kNone;
        NonNativeInstanceData* d = instance_data();
        return (d && d->class_slots) ? *d->class_slots : kNone;
    }
    ClassSlots& mutable_class_slots() {
        NonNativeInstanceData& d = ensure_instance_data();
        if (!d.class_slots) d.class_slots = std::make_unique<ClassSlots>();
        return *d.class_slots;
    }

    // Out of line: the three pointer slots need a write barrier, which would
    // pull the collector into this header.
    void set_home_object(Object* home);
    void set_super_constructor(Function* super_ctor);
    void set_private_brands(Object* brands);
    void set_super_is_null() { mutable_class_slots().super_is_null = true; }
    void set_default_ctor() { mutable_class_slots().is_default_ctor = true; }
    void set_static_method() { mutable_class_slots().is_static_method = true; }
    void set_pm_brand_slot(const std::string& slot) { mutable_class_slots().pm_brand_slot = slot; }

    Object* home_object() const { return class_slots().home_object; }
    Function* super_constructor() const { return class_slots().super_ctor; }
    Object* private_brands() const { return class_slots().private_brands; }
    const std::string& pm_brand_slot() const { return class_slots().pm_brand_slot; }
    bool is_default_ctor() const { return class_slots().is_default_ctor; }
    bool super_is_null() const { return class_slots().super_is_null; }
    bool is_static_method() const { return class_slots().is_static_method; }
    // Derived in the spec sense: `extends <anything>`, `extends null` included.
    bool is_derived_ctor() const { const ClassSlots& s = class_slots(); return s.super_ctor || s.super_is_null; }

    // GetPrototypeFromConstructor's read. get_property("prototype") answers
    // from prototype_ when it is set, but only after comparing the key against
    // "name" and "length" on the way in.
    Value constructor_prototype() const {
        if (prototype_ || prototype_pending_) return Value(ensure_prototype());
        return get_property("prototype");
    }

    // Shared decl-site data (null only for native functions).
    const ExecutableRef<const FunctionExecutable>& get_executable() const { return executable_; }
    // Point this function's body at a tree owned by a unit instead of copying
    // it. For entry points that parse their own source (the Function
    // constructor family): construct with a null body, then call this. The
    // unit keeps the tree alive for as long as the executable needs it, and
    // the literals nested in that tree stay stamped, so they lend their own
    // bodies out too rather than each starting a fresh round of copying.
    void borrow_body_from(const ExecutableRef<ScriptUnit>& unit, class ASTNode* body);
    // Materializes a deferred body (FunctionExecutable::ensure_body): a leaf
    // body is dropped once its analyses are cached and re-parsed from the
    // unit's tokens the first time anything actually wants the tree. Every
    // consumer of the body reaches it through here, which is what keeps that
    // one-way transition invisible to them.
    class ASTNode* ast_body() const { return executable_ ? executable_->ensure_body() : nullptr; }

    // Non-virtual: switches on get_function_kind(), same reasoning as
    // trace() above. call_default() is the plain-Function body.
    Value call(Context& ctx, const std::vector<Value>& args, Value this_value = Value());
    // Arguments that live in the CALLER'S VM REGISTERS, passed as a view
    // instead of a fresh vector. Two things follow from that restriction and
    // neither holds for an arbitrary span, so nothing else may use this:
    // the values are already GC roots (a register bank is either on the C++
    // stack, which probe_word scans through NaN-boxing, or in VM::run's
    // rooted spill vector), and the view stays valid for the whole call.
    Value call_register_args(Context& ctx, std::span<const Value> args, Value this_value);
    Value construct(Context& ctx, const std::vector<Value>& args);
    
    // None of these seven are virtual on Object anymore -- Object's own
    // get_property()/etc. switch on get_type() and dispatch here directly.
    Value get_property(const std::string& key) const;
    bool set_property(const std::string& key, const Value& value, PropertyAttributes attrs = PropertyAttributes::Default);
    std::vector<std::string> get_own_property_keys() const;
    std::vector<std::string> get_internal_property_keys() const;
    bool has_own_property(const std::string& key) const {
        if (key == "prototype" && (prototype_ != nullptr || prototype_pending_)) return true;
        // "name"/"length" are virtually present (own, just not materialized
        // into descriptors_/shape yet) unless explicitly deleted -- see the
        // lazy-installation comment on name_deleted_/length_deleted_ above.
        auto* d = descriptors();
        if (key == "name" && !name_deleted_ && !(d && d->count("name"))) return true;
        if (key == "length" && !length_deleted_ && !(d && d->count("length")) && !has_shape_slot("length")) return true;
        return Object::has_own_property_default(key);
    }
    PropertyDescriptor get_property_descriptor(const std::string& key) const;
    bool delete_property(const std::string& key);
    bool set_property_descriptor(const std::string& key, const PropertyDescriptor& desc);
    // Spec length (ES6: params before the first rest/default) -- decoupled
    // from get_parameters().size(), which includes every param. Decl-site-
    // invariant for non-native functions (a pure function of params), so it
    // lives on the shared executable; native functions have no executable,
    // so theirs lives in native_data() instead.
    void set_declared_length(size_t len) {
        if (executable_) executable_->declared_length = len;
        else if (auto* nd = native_data()) nd->declared_length = len;
    }
    size_t get_declared_length() const {
        if (executable_) return executable_->declared_length;
        auto* nd = native_data();
        return nd ? nd->declared_length : 0;
    }

    Object* get_function_prototype() const { return prototype_; }
    // Builds the deferred .prototype (and its "constructor") on first demand.
    Object* ensure_prototype() const;
    void set_function_prototype(Object* proto);

    static Function* create_function_prototype();

    std::string to_string() const;

    // The %ThrowTypeError% intrinsic, shared by Function.prototype.caller/.arguments and arguments.callee.
    static constinit thread_local Object* s_throw_type_error_;

protected:
    void scan_for_var_declarations(class ASTNode* node, Context& ctx, class Environment* param_env = nullptr);
    // ES2015 9.4.4.7: wires live getter/setter accessors so arguments[i] aliases parameter i.
    void setup_mapped_arguments(Context& fn_ctx, std::span<const Value> args, class Object* arguments_obj);
    // Builds the full arguments object (mapped/unmapped, callee, iterator)
    // and binds it as "arguments" in fn_ctx.
    void create_arguments_object(Context& fn_ctx, std::span<const Value> args);
    // Base bodies -- see trace()/call()'s own doc comments above.
    void trace_default(Visitor& v);
    Value call_default(Context& ctx, const std::vector<Value>& args, Value this_value = Value());
    // args_vec is the caller's own vector when it had one, so a native callee
    // (whose signature demands a vector) reuses it instead of rebuilding it;
    // null means the args came from registers and only a native forces a
    // materialization. It also decides the GC root: a vector's storage is
    // malloc'd and invisible to the stack scan, registers are not.
    Value call_default_impl(Context& ctx, std::span<const Value> args, Value this_value,
                            const std::vector<Value>* args_vec);
    Value call_tree_walker(Context& ctx, std::span<const Value> args, Value this_value);
    Value call_native(Context& ctx, std::span<const Value> args, Value this_value,
                      const std::vector<Value>* args_vec, bool is_construct_invocation);
};

// get_type()-based replacement for dynamic_cast<Function*>: Object is no
// longer polymorphic, so RTTI-based downcasting from a plain Object* isn't
// available. nullptr in, nullptr out (mirrors dynamic_cast's own behavior).
inline Function* as_function(Object* obj) {
    return (obj && obj->get_type() == Object::ObjectType::Function) ? static_cast<Function*>(obj) : nullptr;
}
inline const Function* as_function(const Object* obj) {
    return (obj && obj->get_type() == Object::ObjectType::Function) ? static_cast<const Function*>(obj) : nullptr;
}

namespace ObjectFactory {
    void initialize_memory_pools();
    std::unique_ptr<Object> get_pooled_object();
    std::unique_ptr<Object> get_pooled_array();
    void return_to_pool(std::unique_ptr<Object> obj);
    
    std::unique_ptr<Object> create_object(Object* prototype = nullptr);
    std::unique_ptr<Object> create_array(uint32_t length = 0);
    std::unique_ptr<Object> create_function();
    
    void set_object_prototype(Object* prototype);
    Object* get_object_prototype();
    void set_array_prototype(Object* prototype);
    Object* get_array_prototype();
    void set_function_prototype(Object* prototype);
    Object* get_function_prototype();
    std::unique_ptr<Function> create_js_function(const std::string& name,
                                                 const std::vector<std::string>& params,
                                                 std::unique_ptr<class ASTNode> body,
                                                 class Context* closure_context,
                                                 bool create_prototype = true);
    std::unique_ptr<Function> create_js_function(const std::string& name,
                                                 std::vector<std::unique_ptr<class Parameter>> params,
                                                 std::unique_ptr<class ASTNode> body,
                                                 class Context* closure_context,
                                                 bool create_prototype = true);
    std::unique_ptr<Function> create_native_function(const std::string& name,
                                                     std::function<Value(Context&, const std::vector<Value>&)> fn);
    std::unique_ptr<Function> create_native_function(const std::string& name,
                                                     std::function<Value(Context&, const std::vector<Value>&)> fn,
                                                     uint32_t arity);
    std::unique_ptr<Function> create_native_constructor(const std::string& name,
                                                        std::function<Value(Context&, const std::vector<Value>&)> fn,
                                                        uint32_t arity = 1);
    std::unique_ptr<Function> create_array_method(const std::string& method_name);
    std::unique_ptr<Object> create_string(const std::string& value);
    std::unique_ptr<Object> create_number(double value);
    std::unique_ptr<Object> create_boolean(bool value);
    std::unique_ptr<Object> create_error(const std::string& message);
    std::unique_ptr<Object> create_promise(Context* ctx = nullptr);
    // ES5 10.4.3, extended to Symbol/BigInt: box a primitive `this` for a sloppy-mode call.
    // Returns this_value unchanged if it isn't a primitive needing boxing.
    Value box_primitive_this_sloppy(Context& ctx, const Value& this_value);
}

}

#endif
