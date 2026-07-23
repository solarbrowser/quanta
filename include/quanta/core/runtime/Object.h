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
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <array>
#include <string>
#include <memory>
#include <functional>
#include <chrono>

namespace Quanta {

class PropertyDescriptor;
class HybridDescriptorMap;
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
    uint32_t shape_capacity = 0;
    uint32_t reserved = 0;
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

    static thread_local Context* current_context_;

    // Monotonic; bumped by set_prototype() and by property add/remove/
    // attribute-change on a used_as_prototype() object. SetNamed's
    // transition-cache trusts a cached "no [[Set]] blocker on this chain"
    // answer only while this hasn't moved since it was validated.
    static thread_local uint64_t proto_epoch_;
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
    static thread_local uint64_t descriptor_epoch_;
public:
    static uint64_t descriptor_epoch() { return descriptor_epoch_; }
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
        static constexpr uintptr_t kMask = 0x3;
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
    uint32_t shape_capacity() const { return butterfly_ ? butterfly_header()->shape_capacity : 0; }
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
    
    // Only Proxy overrides this -- out-of-line in Object.cpp.
    uint32_t get_length() const;
    void set_length(uint32_t length);
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

    PropertyDescriptor* find(const std::string& key) {
        for (size_t i = 0; i < inline_count_; i++) {
            if (inline_[i].key == key) return &inline_[i].desc;
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
        if (!overflow_ && inline_count_ < kInlineCapacity) {
            inline_[inline_count_].key = key;
            return inline_[inline_count_++].desc;
        }
        if (!overflow_) {
            // Spillover: move every inline entry into a fresh heap map, then
            // fall through to insert the new key there too.
            overflow_ = std::make_unique<OverflowMap>();
            for (size_t i = 0; i < inline_count_; i++) {
                (*overflow_)[inline_[i].key] = inline_[i].desc;
            }
            inline_count_ = 0;
        }
        return (*overflow_)[key];
    }

    bool erase(const std::string& key) {
        for (size_t i = 0; i < inline_count_; i++) {
            if (inline_[i].key == key) {
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
            if (pred(inline_[i].key, inline_[i].desc)) {
                if (out_key) *out_key = inline_[i].key;
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
    const std::string& inline_key(size_t i) const { return inline_[i].key; }
    const PropertyDescriptor& inline_value(size_t i) const { return inline_[i].desc; }
    const OverflowMap* overflow() const { return overflow_.get(); }

private:
    struct Entry {
        std::string key;
        PropertyDescriptor desc;
    };
    std::array<Entry, kInlineCapacity> inline_;
    size_t inline_count_ = 0;
    std::unique_ptr<OverflowMap> overflow_;
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

private:
    FunctionKind function_kind_ = FunctionKind::Plain;
protected:
    void set_function_kind(FunctionKind kind) { function_kind_ = kind; }
public:
    FunctionKind get_function_kind() const { return function_kind_; }

private:
    std::string name_;
    std::vector<std::string> parameters_;
    std::vector<std::unique_ptr<class Parameter>> parameter_objects_;
    std::unique_ptr<class ASTNode> body_;
    class Context* closure_context_;
    class Environment* closure_environment_;  // lexical environment captured at creation time
    mutable Object* prototype_;  // Mutable to allow lazy initialization in get_property
    bool is_native_;
    bool is_constructor_;  // Whether this function has [[Construct]] internal method
    bool is_arrow_;        // Arrow functions have lexical this binding
    bool is_class_constructor_;  // Class constructors must be called with new
    bool is_strict_;       // Function runs in strict mode (e.g. class methods)
    bool is_param_default_;  // Created as a default param expression; uses param scope as outer env
    // Set ONLY by setup_mapped_arguments() on the getter/setter closures it
    // creates -- a C++-only trust bit (no public setter) so Object.cpp's
    // mapped-arguments fast paths can never be fooled by a JS-settable
    // property of the same name into invoking an attacker-authored Function.
    bool is_mapped_arguments_accessor_ = false;
    uint32_t construct_slot_hint_ = 0;  // Class field count: pre-sizes new instances' shape slots
    // "name"/"length" are lazy: no real descriptor/shape-slot is installed at
    // construction (see get_property/get_property_descriptor/has_own_property
    // overrides below) -- these track whether each has been explicitly
    // deleted, since a never-installed and a deleted property must be told
    // apart (the former still virtually reads as present, the latter must
    // not). declared_length_ is the spec length (may differ from
    // parameters_.size() for rest/default params -- see each constructor).
    bool name_deleted_ = false;
    bool length_deleted_ = false;
    size_t declared_length_ = 0;
    // Lazy AST->bytecode result, shared by every call. vm_incompatible_ is the
    // negative cache: one failed compile routes this function through the
    // tree-walker forever (function-level fallback, see vm-architecture.md).
    // shared_ptr: instances sharing a declaration site (nested_chunk_cache_
    // below) share one compiled chunk instead of each compiling its own.
    std::shared_ptr<const BytecodeChunk> bytecode_chunk_;
    bool vm_incompatible_ = false;
    // Per-instance lookup cache, used in place of BytecodeChunk::lookup_cache
    // when bytecode_chunk_ is shared -- instances differ in captured
    // environment, so a chunk-level cache would serve stale results. Pooled
    // (unlike chunk.lookup_cache, deliberately left on the default allocator
    // -- see Interpreter.cpp's lookup_cache_data comment): a fresh instance
    // is resized exactly once, on its first call, and this is the dominant
    // allocation for single-use clone-elided closures.
    mutable std::vector<BytecodeChunk::LookupCacheEntry,
        SmallMapAllocator<BytecodeChunk::LookupCacheEntry>> instance_lookup_cache_;
    // Compiled-chunk cache for closures declared in this function's own body,
    // keyed by the declaration-site AST node (stable: body_ clones once at
    // construction, never again). A present key with a null chunk means
    // "known VM-incompatible," avoiding a doomed recompile every instantiation.
    std::unordered_map<const ASTNode*, std::shared_ptr<const BytecodeChunk>> nested_chunk_cache_;
    // Owner + borrow pair for clone elision: when decl_site_ is set, this
    // instance never cloned body_/parameter_objects_ and reads them through
    // decl_site_ instead (see ast_body()). body_owner_ is the Function whose
    // AST decl_site_ (and bytecode_chunk_'s embedded pointers) point into --
    // pinned here so it outlives this instance (traced in Function::trace).
    Function* body_owner_ = nullptr;
    const class FunctionExpression* decl_site_ = nullptr;
    std::string source_text_;
    std::function<Value(Context&, const std::vector<Value>&)> native_fn_;

    mutable uint32_t execution_count_;
    mutable bool is_hot_;
    // Once-per-function facts cached on first call: the 'use strict' directive
    // is static per body, and __closure_ self-reference props are installed at
    // class-evaluation time -- re-deriving either on every call showed up hard
    // in call-heavy profiles. -1 unknown, 0 no, 1 yes.
    mutable int8_t strict_directive_state_ = -1;
    mutable int8_t closure_props_state_ = -1;
    // -1 unknown, 0 body never mentions the function's own name (skip the
    // per-call self-reference binding), 1 mentions it.
    mutable int8_t self_name_state_ = -1;
    // -1 unknown, else bitmask: bit0=__super_constructor__, bit1=__super_is_null__,
    // bit2=__private_brands__ present. Same monotonic-cache precedent as
    // closure_props_state_ above -- these markers are installed once at
    // creation time and never removed afterward.
    mutable int8_t super_marker_state_ = -1;
    // Set eagerly the moment a genuine (non-const-marker) __closure_ property
    // is installed (see Function::set_property) -- has_closure_props()
    // becomes an O(1) check instead of scanning every own property key on
    // demand, which used to run on EVERY first call to EVERY function: fresh,
    // single-use closures never benefit from closure_props_state_'s cache
    // above (there's no second call to pay it off). Monotonic: only ever
    // flips false->true, never reset -- if the property is later deleted
    // (astronomically unlikely for this mangled internal name), the flag
    // stays stale-true, which only costs one redundant scan in
    // Function::call's own install-check below, not a correctness issue.
    bool has_closure_props_hint_ = false;

    // Tree-walker insurance for borrowed-AST instances: clones body_ AND
    // parameter_objects_ from decl_site_ (both or neither -- a body-only
    // materialization would silently mis-bind default/rest params).
    void materialize_from_decl_site();
    // Detection-only half of the __closure_* scan in Function::call (no
    // Context needed) -- used to resolve closure_props_state_ before a
    // Context exists; the call-site scan still runs separately to apply
    // the bindings when this returns true.
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
             
    Function(const std::string& name,
             std::function<Value(Context&, const std::vector<Value>&)> native_fn,
             bool create_prototype = false);

    Function(const std::string& name,
             std::function<Value(Context&, const std::vector<Value>&)> native_fn,
             uint32_t arity,
             bool create_prototype = false);
    
    // Non-virtual: the GC sweep (Collector.cpp) reads get_function_kind()
    // and destructs through the correct concrete type itself, same pattern
    // as Object's own destructor dispatch.
    ~Function() = default;

    // Non-virtual: switches on get_function_kind() to reach AsyncFunction/
    // GeneratorFunction/AsyncGeneratorFunction's own extra cell references;
    // trace_default() below is the plain-Function body, also the fallback
    // each of those three chains to instead of calling this (which would
    // re-enter the switch and recurse).
    void trace(Visitor& v);

    const std::string& get_name() const { return name_; }
    void set_name(const std::string& name);
    // Out-of-line: clone-elided instances borrow decl_site_'s cached param
    // names instead of owning a copy -- needs FunctionExpression, only
    // forward-declared here (see ast_body()'s identical rationale).
    const std::vector<std::string>& get_parameters() const;
    const std::vector<std::unique_ptr<class Parameter>>& get_parameter_objects() const { return parameter_objects_; }
    const ASTNode* get_body() const { return ast_body(); }
    size_t get_arity() const { return get_parameters().size(); }
    bool is_native() const { return is_native_; }
    bool is_constructor() const { return is_constructor_; }
    void set_is_constructor(bool value) { is_constructor_ = value; }
    bool is_arrow() const { return is_arrow_; }
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
    void set_construct_slot_hint(uint32_t count) { construct_slot_hint_ = count; }
    // Out-of-line: clone-elided instances borrow decl_site_'s own source
    // text instead of owning a copy -- see get_parameters()'s identical
    // rationale. Needs FunctionExpression, only forward-declared here.
    const std::string& get_source_text() const;
    void set_source_text(const std::string& s) { source_text_ = s; }

    // Lazily sized by the caller (Interpreter.cpp) to chunk.names.size().
    std::vector<BytecodeChunk::LookupCacheEntry,
        SmallMapAllocator<BytecodeChunk::LookupCacheEntry>>& instance_lookup_cache() const { return instance_lookup_cache_; }

    // Adopts an already-compiled (possibly shared) chunk instead of letting
    // Function::call compile its own. `owner` must be pinned (see body_owner_).
    void attach_precompiled_chunk(std::shared_ptr<const BytecodeChunk> chunk, Function* owner) {
        bytecode_chunk_ = std::move(chunk);
        body_owner_ = owner;
    }
    // Declaration-site AST borrow (clone elision); requires attach above first.
    void set_decl_site(const class FunctionExpression* site) { decl_site_ = site; }
    // Out-of-line: needs FunctionExpression, only forward-declared here.
    class ASTNode* ast_body() const;

    // `out`/return convention: true+chunk on hit, true+null on known-incompatible, false on miss.
    bool lookup_nested_chunk(const ASTNode* key, std::shared_ptr<const BytecodeChunk>& out) const {
        auto it = nested_chunk_cache_.find(key);
        if (it == nested_chunk_cache_.end()) return false;
        out = it->second;
        return true;
    }
    void store_nested_chunk(const ASTNode* key, std::shared_ptr<const BytecodeChunk> chunk) {
        nested_chunk_cache_[key] = std::move(chunk);
    }

    uint32_t get_execution_count() const { return execution_count_; }
    bool is_hot_function() const { return is_hot_; }
    void mark_as_hot() const { is_hot_ = true; }
    void reset_performance_stats() const { execution_count_ = 0; is_hot_ = false; }
    
    // Non-virtual: switches on get_function_kind(), same reasoning as
    // trace() above. call_default() is the plain-Function body.
    Value call(Context& ctx, const std::vector<Value>& args, Value this_value = Value());
    Value construct(Context& ctx, const std::vector<Value>& args);
    
    // None of these seven are virtual on Object anymore -- Object's own
    // get_property()/etc. switch on get_type() and dispatch here directly.
    Value get_property(const std::string& key) const;
    bool set_property(const std::string& key, const Value& value, PropertyAttributes attrs = PropertyAttributes::Default);
    std::vector<std::string> get_own_property_keys() const;
    std::vector<std::string> get_internal_property_keys() const;
    bool has_own_property(const std::string& key) const {
        if (key == "prototype" && prototype_ != nullptr) return true;
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
    // from parameters_.size(), which includes every param. See callers in
    // FunctionExpression::evaluate's clone-elision path.
    void set_declared_length(size_t len) { declared_length_ = len; }

    Object* get_function_prototype() const { return prototype_; }
    void set_function_prototype(Object* proto);

    static Function* create_function_prototype();

    std::string to_string() const;

    // The %ThrowTypeError% intrinsic, shared by Function.prototype.caller/.arguments and arguments.callee.
    static thread_local Object* s_throw_type_error_;

protected:
    void scan_for_var_declarations(class ASTNode* node, Context& ctx);
    // ES2015 9.4.4.7: wires live getter/setter accessors so arguments[i] aliases parameter i.
    void setup_mapped_arguments(Context& fn_ctx, const std::vector<Value>& args, class Object* arguments_obj);
    // Builds the full arguments object (mapped/unmapped, callee, iterator)
    // and binds it as "arguments" in fn_ctx.
    void create_arguments_object(Context& fn_ctx, const std::vector<Value>& args);
    // Base bodies -- see trace()/call()'s own doc comments above.
    void trace_default(Visitor& v);
    Value call_default(Context& ctx, const std::vector<Value>& args, Value this_value = Value());
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
