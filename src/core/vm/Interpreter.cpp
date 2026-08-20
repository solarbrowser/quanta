/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <array>
#include "quanta/core/vm/Interpreter.h"
#include "quanta/core/vm/BytecodeCompiler.h"
#include "quanta/core/engine/CallStack.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/gc/Collector.h"
#include "quanta/core/runtime/BigInt.h"
#include "quanta/core/runtime/Generator.h"
#include "quanta/core/runtime/ProxyReflect.h"
#include "quanta/core/runtime/String.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/core/runtime/TypedArray.h"
#include "quanta/parser/AST.h"
#include <cmath>
#include <cstdlib>
#include <cstring>

#ifndef LIKELY
#ifdef __GNUC__
#define LIKELY(x) __builtin_expect(!!(x), 1)
#else
#define LIKELY(x) (x)
#endif
#endif

namespace Quanta {

// Defined in the tree-walker's call.cpp: one shared definition of what a
// spread expands to, so Op::SpreadInto and the tree-walker cannot drift.
void append_spread_values(Context& ctx, const Value& spread_value, std::vector<Value>& out);
// Defined in literals.cpp: the single definition of what an object spread
// copies, shared with the tree-walker (see Op::ObjectSpreadInto).
bool object_spread_into(Context&, Object*, const Value&);
// From the tree-walker's misc.cpp, backing Op::CreateRegExp.
Value create_regexp_literal(Context& ctx, const std::string& pattern, const std::string& flags);
// Likewise from binary.cpp, backing Op::HasPrivate.
Value private_name_in(Context& ctx, const std::string& private_name, const Value& target);
// And from assignment.cpp, backing Op::CopyRestProperties.
Value build_rest_object(Context& ctx, const Value& source_value, Object* source_obj,
                        const std::vector<std::string>& assigned_keys);
// And from language.cpp, backing Op::CreateClosure / Op::DeclareFunction.
Value instantiate_closure(Context& ctx, const ClosureTemplate& tpl);
Value declare_function(Context& ctx, const ClosureTemplate& tpl);
// And from call.cpp, backing Op::SuperCall.
Value perform_super_call(Context& ctx, std::span<const Value> arg_values, bool super_already_called);
// And from member.cpp, backing the Op::GetSuper family.
Object* resolve_super_base(Context& ctx);
Value super_get(Context& ctx, const std::string& prop_name);
Value super_get_on(Context& ctx, Object* base, const std::string& prop_name);
void super_set(Context& ctx, const std::string& prop_name, const Value& value);
void super_set_on(Context& ctx, Object* base, const std::string& prop_name, const Value& value);

namespace VM {

bool enabled() {
    static const bool on = [] {
        const char* env = std::getenv("QUANTA_VM");
        if (!env) return true;
        return env[0] != '0';
    }();
    return on;
}

namespace {

using BinOp = BinaryExpression::Operator;

inline int16_t read_i16(const uint8_t* code, uint32_t pc) {
    uint16_t raw = static_cast<uint16_t>(code[pc]) |
                   (static_cast<uint16_t>(code[pc + 1]) << 8);
    return static_cast<int16_t>(raw);
}

inline uint16_t read_u16(const uint8_t* code, uint32_t pc) {
    return static_cast<uint16_t>(code[pc]) |
           (static_cast<uint16_t>(code[pc + 1]) << 8);
}

// Routes through the shared apply_operator so VM and tree-walker semantics can't drift.
inline Value binary_slow(Context& ctx, BinOp op, const Value& l, const Value& r) {
    return BinaryExpression::apply_operator(ctx, op, l, r);
}

inline Object* as_object_like(const Value& v) {
    if (v.is_function()) return static_cast<Object*>(v.as_function());
    if (v.is_object()) return v.as_object();
    return nullptr;
}

// A canonical array-index string per spec 6.1.7 (CanonicalNumericIndexString-ish
// for strings): no leading zero (except "0" itself), digits only, round-trips.
bool key_is_canonical_index(const std::string& s, size_t& out_index) {
    if (s.empty()) return false;
    if (s == "0") { out_index = 0; return true; }
    if (s[0] < '1' || s[0] > '9') return false;
    for (char c : s) if (c < '0' || c > '9') return false;
    char* end;
    unsigned long v = std::strtoul(s.c_str(), &end, 10);
    if (*end != '\0') return false;
    out_index = static_cast<size_t>(v);
    return true;
}

// Forward decl: defined below, needed here since get_primitive_named's new
// prototype-property cache reuses the same (shape, slot_index) learn helper
// GetNamed's own receiver-shape cache uses.
void learn_feedback(FeedbackSlot* fb, Shape* shape, uint32_t slot_index,
                     bool is_accessor = false);

// Mirrors MemberExpression::evaluate's primitive-receiver branch.
Value get_primitive_named(Context& ctx, const Value& prim, const std::string& name,
                           FeedbackSlot* fb, Function* owner, bool rooted) {
    if (prim.is_string()) {
        // Straight off the String, not through to_string(): that returns by
        // value, so every `.length` read used to copy the whole buffer before
        // scanning it, and both halves are O(length).
        if (name == "length") {
            return Value(static_cast<double>(prim.as_string()->utf16_length()));
        }
        size_t idx;
        if (key_is_canonical_index(name, idx)) {
            int32_t unit = prim.as_string()->code_unit_at(idx);
            if (unit < 0) return Value();  // out of range: no such own property
            return Value(encode_utf16_unit(static_cast<uint32_t>(unit)));
        }
    }
    using PK = Context::PrimitiveKind;
    PK kind = prim.is_string() ? PK::String
        : prim.is_number() ? PK::Number
        : prim.is_bigint() ? PK::BigInt
        : prim.is_boolean() ? PK::Boolean
        : prim.is_symbol() ? PK::Symbol
        : PK::Count;
    if (kind == PK::Count) return Value();
    // The realm intrinsic, not `globalThis.String.prototype`: reassigning the
    // global binding must not change what a string's methods resolve to, and
    // going through it cost a scope lookup and a property read every time.
    Object* proto_obj = Context::primitive_prototype(kind);
    if (!proto_obj) return Value();

    // Mono/poly cache keyed on proto_obj's OWN shape (every string shares the
    // same String.prototype, etc.) -- not the receiver's shape, which is why
    // this is a separate check from GetNamed's own receiver-shape cache even
    // though it shares the same fb/kMaxEntries budget (proto_obj's Shape* is
    // always distinct from any real receiver's shape, so entries from each
    // never collide/alias, they just compete for the same 4 slots).
    // A builtin prototype method lives in the descriptor map, so the two
    // lookups below are what every `"str".method` in the program pays. The
    // descriptor's address is stable while the epoch holds -- insert, attribute
    // change and erase all move it -- so the site can point straight at it.
    // Only the lookups are skipped, not the prototype resolution above: nothing
    // guards `String.prototype` itself being reassigned.
    if (fb && fb->prim_valid && fb->prim_proto == proto_obj &&
        fb->prim_desc_epoch == Object::descriptor_epoch()) {
        if (!fb->prim_is_getter) return fb->prim_value;
        Function* getter = as_function(as_object_like(fb->prim_value));
        return getter ? getter->call_register_args(ctx, {}, prim) : Value();
    }

    // One descriptors_ lookup, reused for the descriptor below: asking
    // has_descriptor_override() and then get_property_descriptor() scanned the
    // same map for the same key with no mutation in between, and a builtin
    // prototype's methods all live in that map, so this ran on every
    // `"str".method` in the program. Same consolidation get_named already made.
    PropertyDescriptor* override_desc = proto_obj->find_descriptor_override(name);
    Shape* proto_shape = proto_obj->get_shape();
    bool cacheable = fb && !fb->mega && proto_obj->get_type() == Object::ObjectType::Ordinary &&
                      !override_desc &&
                      !(proto_shape && proto_shape->is_accessor_slot(name));
    if (cacheable) {
        Shape* shape = proto_obj->get_shape();
        for (uint8_t i = 0; i < fb->count; i++) {
            if (fb->entries[i].shape == shape) {
                if (!fb->entries[i].is_accessor) {
                    const Value* slot = proto_obj->get_shape_slot_unchecked(fb->entries[i].slot_index);
                    if (slot) return *slot;
                }
                break;
            }
        }
    }
    PropertyDescriptor desc = proto_obj->get_property_descriptor(name);
    // Learn from what that call produced, rather than from the descriptor it
    // read: only this value has been through whatever the call does on the way.
    if (fb && rooted && (desc.is_accessor_descriptor() || desc.has_value())) {
        Collector::write_barrier(owner);
        fb->prim_proto = proto_obj;
        fb->prim_is_getter = desc.is_accessor_descriptor();
        fb->prim_value = fb->prim_is_getter ? Value(desc.get_getter()) : desc.get_value();
        fb->prim_desc_epoch = Object::descriptor_epoch();
        fb->prim_valid = true;
    }
    if (desc.is_accessor_descriptor()) {
        if (!desc.has_getter()) return Value();
        Function* getter = as_function(desc.get_getter());
        return getter ? getter->call_register_args(ctx, {}, prim) : Value();
    }
    if (cacheable && desc.has_value()) {
        Shape* s = proto_shape;
        int32_t idx = s ? s->find_slot(name) : -1;
        if (idx >= 0) learn_feedback(fb, s, static_cast<uint32_t>(idx));
    }
    return desc.has_value() ? desc.get_value() : proto_obj->get_property(name);
}

// Same shape as the tree-walker's primitive assignment fallback.
void set_primitive_named(Context& ctx, const Value& prim, const std::string& name, const Value& value) {
    const char* ctor_name = prim.is_string() ? "String"
        : prim.is_number() ? "Number"
        : prim.is_bigint() ? "BigInt"
        : prim.is_boolean() ? "Boolean"
        : prim.is_symbol() ? "Symbol"
        : nullptr;
    if (ctor_name) {
        Value ctor = ctx.get_binding(ctor_name);
        if (!ctx.has_exception() && ctor.is_function()) {
            Value proto = ctor.as_function()->get_property("prototype");
            Object* level = (!ctx.has_exception() && proto.is_object()) ? proto.as_object() : nullptr;
            while (level) {
                if (level->get_type() == Object::ObjectType::Proxy) {
                    static_cast<Proxy*>(level)->set_trap(Value(name), value, prim);
                    return;
                }
                PropertyDescriptor desc = level->get_property_descriptor(name);
                if (desc.is_accessor_descriptor()) {
                    if (desc.has_setter()) {
                        Function* setter = as_function(desc.get_setter());
                        if (setter) setter->call(ctx, {value}, prim);
                    }
                    return;
                }
                if (desc.has_value()) break;
                level = level->get_prototype();
            }
        }
    }
    if (ctx.has_exception()) return;
    if (ctx.is_strict_mode()) {
        ctx.throw_type_error("Cannot create property on primitive value");
    }
}

// Records a newly-observed (shape, slot) pair for a call site. Dedups
// against entries already present: SetNamed in particular can re-derive the
// same shape it already has cached (e.g. many objects from one constructor
// converging on one shape after their last field is added), and inserting a
// duplicate would burn through the fixed budget without ever caching an
// actually-distinct shape, tripping mega early for no benefit.
void learn_feedback(FeedbackSlot* fb, Shape* shape, uint32_t slot_index, bool is_accessor) {
    // A site that gave up stays given up, which is what lets going mega empty
    // the table below.
    if (fb->mega) return;
    for (uint8_t i = 0; i < fb->count; i++) {
        if (fb->entries[i].shape == shape) {
            fb->entries[i].is_accessor = is_accessor;
            return;
        }
    }
    if (fb->count < FeedbackSlot::kMaxEntries) {
        fb->entries[fb->count++] = {shape, slot_index, is_accessor};
    } else {
        // Emptied as well as marked, so a live first entry is by itself proof
        // that the site is still caching: the read path then asks one question
        // where it asked three. Every other reader already tests mega before
        // it looks at the table.
        fb->mega = true;
        fb->count = 0;
        fb->entries[0].shape = nullptr;
    }
}

// Like learn_feedback, but a duplicate from_shape REFRESHES the entry rather
// than no-opping: to_shape/slot_index are deterministic for (from_shape, key),
// but proto_epoch goes stale and a re-hit needs the current value to trust it.
//
// `prototype` is recorded only by SetNamed, whose cached fact ("no [[Set]]
// blocker on the chain") depends on which chain the receiver has. It is a real
// GC cell, so recording one needs an owner to barrier against and to keep the
// entry traced -- the gate learn_proto already uses.
void learn_transition(FeedbackSlot* fb, Shape* from_shape, Shape* to_shape,
                       Object* prototype, uint32_t slot_index, uint64_t epoch,
                       Function* owner) {
    if (prototype) {
        if (!owner) return;
        Collector::write_barrier(owner);
    }
    for (uint8_t i = 0; i < fb->transition_count; i++) {
        if (fb->transitions[i].from_shape == from_shape) {
            fb->transitions[i] = {from_shape, to_shape, prototype, slot_index, epoch};
            return;
        }
    }
    if (fb->transition_count < FeedbackSlot::kMaxEntries) {
        fb->transitions[fb->transition_count++] = {from_shape, to_shape, prototype, slot_index, epoch};
    } else {
        fb->transition_mega = true;
    }
}

// GetNamed's inherited-property read cache. Dedups on {receiver_shape,
// prototype} (both must match -- see FeedbackSlot::ProtoEntry): a duplicate
// refreshes epoch/holder/slot in place, same reasoning as learn_transition.
// write_barrier(owner) covers holder/prototype being real GC cells stored
// into owner's (possibly already-old) BytecodeChunk.
void learn_proto(FeedbackSlot* fb, Shape* receiver_shape, Object* prototype,
                  Object* holder, uint32_t slot_index, uint64_t epoch, Function* owner,
                  bool from_descriptor = false, const Value& cached = Value(),
                  uint64_t desc_epoch = 0, bool absent = false, bool is_getter = false) {
    FeedbackSlot::ProtoEntry fresh;
    fresh.receiver_shape = receiver_shape;
    fresh.prototype = prototype;
    fresh.proto_epoch = epoch;
    fresh.holder = holder;
    fresh.slot_index = slot_index;
    fresh.from_descriptor = from_descriptor;
    fresh.absent = absent;
    fresh.is_getter = is_getter;
    fresh.desc_epoch = desc_epoch;
    fresh.cached_value = cached;
    for (uint8_t i = 0; i < fb->proto_count; i++) {
        auto& pe = fb->proto_entries[i];
        if (pe.receiver_shape == receiver_shape && pe.prototype == prototype) {
            Collector::write_barrier(owner);
            pe = fresh;
            return;
        }
    }
    if (fb->proto_count < FeedbackSlot::kMaxEntries) {
        Collector::write_barrier(owner);
        fb->proto_entries[fb->proto_count++] = fresh;
    } else {
        fb->proto_mega = true;
    }
}

// The remembered set records an old-to-young EDGE, and Collector::write_barrier
// opens with a heap probe to find out whether the container is an old cell at
// all. A number, a boolean or a nullish value creates no edge whatever the
// container turns out to be, so that probe can only ever conclude "nothing to
// record" for them. Only those four are waived -- a tag not named here,
// including any added later, still takes the barrier. Removing an edge never
// needs one: the collector's insertion barrier is Dijkstra-style.
inline void write_barrier_for(Object* obj, const Value& value) {
    if (value.is_number() || value.is_boolean() || value.is_undefined() || value.is_null()) return;
    Collector::write_barrier(obj);
}

// A shape match alone doesn't prove "plain data slot, no override" -- both
// the hit check and the miss-path refill re-verify has_descriptor_override.
// has_descriptor_override is a fact about (obj, name), independent of which
// entry matches, so it's checked once per call (in `cacheable`) rather than
// once per scanned entry.
Value get_named(Context& ctx, const Value& receiver, const std::string& name,
                 FeedbackSlot* fb, Function* owner, bool rooted) {
    if (receiver.is_null() || receiver.is_undefined()) {
        ctx.throw_type_error("Cannot read property of null or undefined");
        return Value();
    }
    Object* obj = as_object_like(receiver);
    if (!obj) return get_primitive_named(ctx, receiver, name, fb, owner, rooted);

    // Array.length is always live-computed from elements_ (never reliably
    // mirrored in a shape slot -- push/index-growth never syncs one, only
    // an explicit `arr.length = N` assignment does), so it can never be
    // cached as a shape-slot value. Skip the whole descriptor dance below
    // and go straight to it, same as Object::get_property's own Array
    // branch -- but a defineProperty override (e.g. non-writable length)
    // still must win, so that rare case still falls through.
    // A dense array's length is its element count, and `length` can never be
    // an accessor -- defineProperty refuses to turn a non-configurable data
    // property into one -- so the count answers the read exactly. A length
    // moved away from the storage fails the check and takes the path below.
    if (obj->get_type() == Object::ObjectType::Array && name == "length" &&
        obj->has_only_dense_elements()) {
        if (fb) fb->array_length = true;
        return Value(static_cast<double>(obj->element_count()));
    }

    Shape* obj_shape = obj->get_shape();
    bool ordinary = obj->get_type() == Object::ObjectType::Ordinary;
    uint64_t cur_epoch = Object::descriptor_epoch();

    // An own property answered out of the receiver's descriptor map, learned
    // below. Keyed on the receiver itself because that map is its own; the
    // epoch is what says the value in it has not moved since.
    if (fb && obj == fb->own_desc_receiver && fb->own_desc_epoch == cur_epoch) {
        return fb->own_desc_value;
    }

    // Asked before the receiver's own descriptor is built, not after: that
    // query walks own properties and, on an Array, recomputes length -- all of
    // it wasted once this hits. The gate is what makes the order safe: the
    // shape fixes which own properties exist, has_descriptor_override covers
    // the ones a shape cannot show, and an Array receiver is admitted only for
    // a key that is neither length nor an index, so elements cannot answer it
    // either. Which is the same thing the descriptor query would have
    // established, only without doing it.
    const bool exotic_proto_ok =
        obj->get_type() == Object::ObjectType::Array &&
        name != "length" && !(!name.empty() && name[0] >= '0' && name[0] <= '9');
    if (rooted && fb && !fb->proto_mega &&
        (obj->get_type() == Object::ObjectType::Ordinary || exotic_proto_ok) &&
        !obj->has_descriptor_override(name)) {
        Shape* shape = obj->get_shape();
        Object* proto0 = obj->get_prototype();
        uint64_t epoch = Object::proto_epoch();
        for (uint8_t i = 0; i < fb->proto_count; i++) {
            const auto& pe = fb->proto_entries[i];
            if (pe.receiver_shape == shape && pe.prototype == proto0 && pe.proto_epoch == epoch) {
                // Nothing on the chain carries this key, so the read is
                // undefined. The gate above has already re-established the
                // receiver's own side (its shape is the entry's key, and the
                // descriptor map its shape cannot show has just been asked).
                if (pe.absent) return Value();
                if (pe.is_getter) {
                    if (pe.desc_epoch == cur_epoch) {
                        Function* getter_fn = as_function(as_object_like(pe.cached_value));
                        return getter_fn ? getter_fn->call_register_args(ctx, {}, receiver) : Value();
                    }
                    break;
                }
                if (pe.from_descriptor) {
                    if (pe.desc_epoch == cur_epoch) return pe.cached_value;
                    break;
                }
                const Value* slot = pe.holder->get_shape_slot_unchecked(pe.slot_index);
                if (slot) return *slot;
                break;
            }
        }
    }


    // Fast path: a previously-learned entry for this exact shape already
    // confirmed "no descriptors_ override for this key" -- if the global
    // descriptor epoch hasn't moved since, trust that without rescanning
    // descriptors_ at all (skip find_descriptor_override entirely). Safe
    // even though descriptors_ is per-object, not per-shape: ANY object
    // anywhere gaining a new override bumps the epoch, invalidating this
    // for every shape, not just the one that changed -- see
    // Object::descriptor_epoch_'s doc comment.
    // An object with no descriptor map at all cannot hold an override for any
    // key, so the entry is good for THIS receiver whatever the epoch says.
    // That matters because the epoch is global: one defineProperty anywhere
    // retires every entry, and nothing ever revives them -- measured on a real
    // workload, this gate went stale and stayed stale, and the fast path then
    // missed on the overwhelming majority of reads. Note the two facts are not
    // interchangeable: the stamp says "no object had an override when this was
    // learned", the map says "this object has none now", and only the second
    // is safe to re-establish from a single receiver.
    if (fb && !fb->mega && ordinary && obj_shape) {
        for (uint8_t i = 0; i < fb->count; i++) {
            if (fb->entries[i].shape == obj_shape) {
                if (!obj->has_any_descriptor_override()) {
                    const Value* slot = obj->get_shape_slot_unchecked(fb->entries[i].slot_index);
                    if (slot) {
                        if (!fb->entries[i].is_accessor) return *slot;
                        // An accessor slot holds the getter; a setter-only one
                        // holds nothing callable and the property reads as
                        // undefined, which is what the uncached path returns too.
                        Function* getter_fn = as_function(as_object_like(*slot));
                        return getter_fn ? getter_fn->call_register_args(ctx, {}, receiver) : Value();
                    }
                }
                break;
            }
        }
    }

    // Single descriptors_ lookup, reused below for both the cacheable-gate
    // and the accessor branch -- has_descriptor_override()+
    // get_property_descriptor() back to back would otherwise re-scan the
    // same map for the same key with no mutation in between.
    PropertyDescriptor* override_desc = obj->find_descriptor_override(name);
    const bool own_descriptor = override_desc != nullptr;
    bool cacheable = fb && !fb->mega && ordinary && !override_desc;
    if (cacheable) {
        Shape* shape = obj_shape;
        for (uint8_t i = 0; i < fb->count; i++) {
            if (fb->entries[i].shape == shape) {
                if (!fb->entries[i].is_accessor) {
                    const Value* slot = obj->get_shape_slot_unchecked(fb->entries[i].slot_index);
                    if (slot) return *slot;
                }
                break;
            }
        }
    }
    // An own accessor must run before get_property()'s type-specific
    // shortcuts, which don't know about one installed via defineProperty.
    if (obj->get_type() != Object::ObjectType::Proxy) {
        PropertyDescriptor desc = override_desc ? *override_desc : obj->get_property_descriptor(name);
        if (desc.is_accessor_descriptor()) {
            // Cacheable on exactly the same terms as a data property: an own
            // accessor-kind shape slot with nothing in descriptors_ shadowing
            // it. Without this every read rebuilt a whole descriptor first,
            // which cost more than the getter call it was preparing.
            if (cacheable && obj_shape && obj_shape->is_accessor_slot(name)) {
                int32_t idx = obj_shape->find_slot(name);
                if (idx >= 0) {
                    learn_feedback(fb, obj_shape, static_cast<uint32_t>(idx),
                                   /*is_accessor=*/true);
                }
            }
            if (!desc.has_getter()) return Value();
            Function* getter_fn = as_function(desc.get_getter());
            return getter_fn ? getter_fn->call_register_args(ctx, {}, receiver) : Value();
        }
        // `desc` already fully answers "is this an own property, and if so
        // what's its value" (get_property_descriptor's own miss-path already
        // did has_own_property+get_own_property internally) -- for the
        // common Ordinary-object case, skip the redundant has_own_property
        // below AND the final get_property() call, both of which would only
        // re-derive this same answer. Restricted to Ordinary because Array/
        // TypedArray/etc. have type-specific logic in get_property() (e.g.
        // Array.length is computed live, not from a descriptor) that this
        // shortcut must not bypass.
        if (desc.has_value() && obj->get_type() == Object::ObjectType::Ordinary) {
            if (cacheable) {
                Shape* s = obj->get_shape();
                int32_t idx = s ? s->find_slot(name) : -1;
                if (idx >= 0) learn_feedback(fb, s, static_cast<uint32_t>(idx));
            }
            // Learn only what the descriptor map itself answered. A shape-slot
            // value belongs to the entries above, which track it through shape
            // changes this entry knows nothing about.
            else if (override_desc && fb && rooted) {
                Collector::write_barrier(owner);
                fb->own_desc_receiver = obj;
                fb->own_desc_value = desc.get_value();
                fb->own_desc_epoch = cur_epoch;
            }
            return desc.get_value();
        }
        // A prototype-only accessor (e.g. byteOffset) isn't own, but a
        // TypedArray's own numeric index (9.4.5.4) never checks the prototype.
        double numeric_index;
        bool is_integer_indexed_key = obj->is_typed_array() &&
            TypedArrayBase::canonical_numeric_index(name, numeric_index);
        if (!is_integer_indexed_key && !desc.has_value()) {
            // Prototype-chain read cache: skips this walk AND the
            // obj->get_property(name) call below entirely on a hit. Gated on
            // owner != nullptr -- run_script's ownerless chunk (see
            // VM::run_script) never populates or trusts this cache, since
            // its holder/prototype fields would have no GC root otherwise.
            // An exotic receiver may use this cache too, for a key it answers
            // ordinarily. The restriction to Ordinary belongs to the
            // receiver-side cache above, where an array's length and its
            // indices come from type-specific logic rather than a shape slot;
            // by the time control reaches here the receiver has been shown to
            // have no own property for this key, so what is left is the plain
            // prototype walk. Those two keys are excluded outright.
            bool walked_to_end = true;
            for (Object* proto = obj->get_prototype(); proto; proto = proto->get_prototype()) {
                // A Proxy answers a read through its "get" trap, which a
                // descriptor query does not run -- the walk cannot conclude
                // anything about a chain containing one.
                if (proto->get_type() == Object::ObjectType::Proxy) { walked_to_end = false; break; }
                PropertyDescriptor proto_desc = proto->get_property_descriptor(name);
                if (proto_desc.is_accessor_descriptor()) {
                    if (!proto_desc.has_getter()) return Value();
                    Function* getter_fn = as_function(proto_desc.get_getter());
                    // Learn the getter itself. `get x()` on a class puts the
                    // accessor in the prototype's descriptor map, so finding it
                    // again costs a rebuilt descriptor on the receiver, the walk
                    // and a second rebuilt descriptor on the holder -- all
                    // before the call that does the actual work. The entry
                    // caches what the walk arrived at, on the same terms a
                    // from_descriptor entry does: the receiver is Ordinary with
                    // no own override for the key (established above), and
                    // desc_epoch retires the entry if the accessor is
                    // redefined.
                    if (rooted && fb && !fb->proto_mega && getter_fn && obj->get_shape() &&
                        obj->get_type() == Object::ObjectType::Ordinary &&
                        proto->get_type() == Object::ObjectType::Ordinary &&
                        !(!name.empty() && name[0] >= '0' && name[0] <= '9')) {
                        learn_proto(fb, obj->get_shape(), obj->get_prototype(), proto, 0,
                                    Object::proto_epoch(), owner, /*from_descriptor=*/false,
                                    Value(getter_fn), Object::descriptor_epoch(),
                                    /*absent=*/false, /*is_getter=*/true);
                    }
                    return getter_fn ? getter_fn->call_register_args(ctx, {}, receiver) : Value();
                }
                if (proto_desc.has_value()) {
                    // Learn: proto is the holder. Only cacheable as a plain
                    // shape slot (Ordinary, no descriptor override) -- same
                    // trust rule as the receiver-side cache.
                    // The holder may be exotic too. Array.prototype is itself an
                    // array, as the spec requires, so insisting on an Ordinary
                    // holder ruled out every array method there is. What the
                    // cache needs from the holder is only that the value it
                    // hands back is the one a read would produce, which the
                    // descriptor and shape-slot branches below each establish
                    // on their own terms.
                    const bool holder_ok =
                        proto->get_type() == Object::ObjectType::Ordinary ||
                        (proto->get_type() == Object::ObjectType::Array &&
                         name != "length" &&
                         !(!name.empty() && name[0] >= '0' && name[0] <= '9'));
                    if (rooted && fb && !fb->proto_mega &&
                        (obj->get_type() == Object::ObjectType::Ordinary || exotic_proto_ok) &&
                        holder_ok) {
                        Shape* hs = proto->get_shape();
                        int32_t hidx = proto->has_descriptor_override(name)
                                           ? -1
                                           : (hs ? hs->find_slot(name) : -1);
                        if (hidx >= 0) {
                            learn_proto(fb, obj->get_shape(), obj->get_prototype(), proto,
                                        static_cast<uint32_t>(hidx), Object::proto_epoch(), owner);
                        } else if (!proto_desc.is_accessor_descriptor() && proto_desc.has_value()) {
                            // The holder keeps it in its descriptor map, which is
                            // where every builtin prototype method lives. Cache
                            // the value and let the descriptor epoch invalidate.
                            learn_proto(fb, obj->get_shape(), obj->get_prototype(), proto,
                                        0, Object::proto_epoch(), owner,
                                        /*from_descriptor=*/true, proto_desc.get_value(),
                                        Object::descriptor_epoch());
                        }
                    }
                    // The walk already has the value. Falling through to
                    // get_property below would start the same walk over from
                    // the receiver and arrive at this descriptor again.
                    // Ordinary only: an exotic receiver reaches its prototype
                    // through logic of its own, and this shortcut must not
                    // stand in for it.
                    if (obj->get_type() == Object::ObjectType::Ordinary) {
                        return proto_desc.get_value();
                    }
                    walked_to_end = false;
                    break;
                }
            }
            // Same shortcut for the other outcome: the walk ran to the end of
            // the chain, so the key is on neither the receiver nor any
            // prototype, and get_property below would redo the walk to reach
            // that same answer. Any '#' key is handed over instead of decided
            // here -- private storage is deliberately not reported as an own
            // property (Object::has_own_property_default), so an absence
            // observed for one of those keys is not an absence.
            if (walked_to_end && obj->get_type() == Object::ObjectType::Ordinary &&
                (name.empty() || name[0] != '#')) {
                // Record the absence, so the next read of this key on this
                // shape answers without walking. What the walk just proved --
                // no own property, no Proxy anywhere, nothing on the chain --
                // is exactly what a ProtoEntry's key already validates, so the
                // absence rides in the cache the hit paths already scan.
                // Beyond what the walk established:
                //  - a shapeless (dictionary-mode) receiver is refused, since a
                //    null receiver_shape would match every other shapeless
                //    object rather than identifying this one;
                //  - a digit-leading key is refused, because an exotic object
                //    on the chain can answer one out of its elements, and an
                //    element write does not move proto_epoch.
                if (rooted && fb && !fb->proto_mega && obj->get_shape() &&
                    !(!name.empty() && name[0] >= '0' && name[0] <= '9')) {
                    learn_proto(fb, obj->get_shape(), obj->get_prototype(), nullptr, 0,
                                Object::proto_epoch(), owner, /*from_descriptor=*/false,
                                Value(), /*desc_epoch=*/0, /*absent=*/true);
                }
                return Value();
            }
        }
    }
    Value result = obj->get_property(name);
    if (ctx.has_exception()) return Value();
    if (cacheable) {
        Shape* s = obj->get_shape();
        int32_t idx = s ? s->find_slot(name) : -1;
        if (idx >= 0) learn_feedback(fb, s, static_cast<uint32_t>(idx));
    }
    // A function object holding the key in its own descriptor map: every
    // constructor-namespace builtin is one -- Number.isInteger, Object.keys,
    // Date.now -- and none of them reaches a cache above, since the shape-slot
    // entries want an Ordinary receiver and the prototype entries want the key
    // to be absent from the receiver. For the answer to keep coming out of
    // that map, Function::get_property must not answer the key from somewhere
    // else first, and there are exactly two it does: `name`, which it takes
    // from the function's own name field, and `length`, which falls back to
    // the arity. Those two are refused rather than tracked.
    if (own_descriptor && fb && rooted &&
        obj->get_type() == Object::ObjectType::Function &&
        name != "name" && name != "length") {
        Collector::write_barrier(owner);
        fb->own_desc_receiver = obj;
        fb->own_desc_value = result;
        fb->own_desc_epoch = cur_epoch;
    }
    return result;
}

void set_named(Context& ctx, const Value& receiver, const std::string& name,
               const Value& value, FeedbackSlot* fb, Function* owner) {
    if (receiver.is_null() || receiver.is_undefined()) {
        ctx.throw_type_error(std::string("Cannot set properties of ") +
            (receiver.is_null() ? "null" : "undefined"));
        return;
    }
    Object* obj = as_object_like(receiver);
    if (!obj) { set_primitive_named(ctx, receiver, name, value); return; }

    write_barrier_for(obj, value);
    bool ordinary_recv = obj->get_type() == Object::ObjectType::Ordinary;
    // Same epoch-trusting fast path as get_named's own -- skips
    // has_descriptor_override entirely on a hit. See Object::
    // descriptor_epoch_'s doc comment for why a global epoch is safe here
    // even though descriptors_ is per-object.
    if (fb && !fb->mega && ordinary_recv) {
        Shape* shape = obj->get_shape();
        for (uint8_t i = 0; i < fb->count; i++) {
            if (fb->entries[i].shape == shape) {
                // See get_named's own guard: a receiver with no descriptor map
                // cannot shadow the slot, whatever the global stamp says.
                if (!fb->entries[i].is_accessor && !obj->has_any_descriptor_override()) {
                    Value* slot = obj->get_shape_slot_unchecked(fb->entries[i].slot_index);
                    if (slot) { *slot = value; return; }
                }
                break;
            }
        }
    }
    if (fb && !fb->mega && ordinary_recv &&
        !obj->has_descriptor_override(name)) {
        Shape* shape = obj->get_shape();
        for (uint8_t i = 0; i < fb->count; i++) {
            if (fb->entries[i].shape == shape) {
                if (fb->entries[i].is_accessor) break;
                Value* slot = obj->get_shape_slot_unchecked(fb->entries[i].slot_index);
                if (slot) { *slot = value; return; }
                break;
            }
        }
    }

    // Transition-cache: adding a brand-new own property. A hit skips both
    // Shape::transition(key)'s hash lookup and ordinary_set's prototype-chain
    // walk below -- safe only while proto_epoch() still matches what it was
    // when learned. is_extensible() is checked fresh, not folded into the
    // epoch: a non-extensible object just stops hitting this path.
    if (fb && !fb->transition_mega && obj->get_type() == Object::ObjectType::Ordinary &&
        !obj->has_descriptor_override(name) && obj->is_extensible()) {
        Shape* shape = obj->get_shape();
        Object* proto0 = obj->get_prototype_raw();
        uint64_t epoch = Object::proto_epoch();
        for (uint8_t i = 0; i < fb->transition_count; i++) {
            const auto& te = fb->transitions[i];
            if (te.from_shape == shape && te.prototype == proto0 && te.proto_epoch == epoch) {
                obj->add_shape_property_cached(name, value, te.to_shape);
                return;
            }
        }
    }

    Shape* shape_before = obj->get_shape();
    bool was_new = fb && obj->get_type() == Object::ObjectType::Ordinary &&
                    !obj->has_descriptor_override(name) &&
                    shape_before && shape_before->find_slot(name) < 0;

    // ordinary_set (not set_property): checks inherited non-writable/Proxy
    // targets first, unlike set_property which would just shadow them.
    bool ok = obj->ordinary_set(name, value);
    if (ctx.has_exception()) return;
    if (!ok && ctx.is_strict_mode()) {
        ctx.throw_type_error("Cannot assign to read only property '" + name + "'");
        return;
    }
    // Re-checked fresh, not reusing a pre-call flag: ordinary_set can migrate
    // obj to dictionary mode (shape transition cap hit while adding a new
    // property), which changes has_descriptor_override's answer for `name`.
    if (fb && !fb->mega && obj->get_type() == Object::ObjectType::Ordinary &&
        !obj->has_descriptor_override(name)) {
        Shape* s = obj->get_shape();
        int32_t idx = s ? s->find_slot(name) : -1;
        if (idx >= 0) learn_feedback(fb, s, static_cast<uint32_t>(idx));
    }
    // Transition learn: has_descriptor_override re-checked post-call (not the
    // pre-call value) so a no-trap Proxy's set forward -- which can transition
    // obj's shape via set_property_descriptor, always leaving a descriptors_
    // entry behind -- is naturally excluded from being learned here.
    if (was_new && ok && fb && !fb->transition_mega &&
        obj->get_type() == Object::ObjectType::Ordinary && !obj->has_descriptor_override(name)) {
        Shape* s = obj->get_shape();
        int32_t idx = s ? s->find_slot(name) : -1;
        if (idx >= 0) {
            learn_transition(fb, shape_before, s, obj->get_prototype_raw(),
                              static_cast<uint32_t>(idx), Object::proto_epoch(), owner);
        }
    }
}

// CreateDataProperty shape-transition cache, shared by Op::DefineOwn and
// Op::FinalizeStaticProperty's Method case -- both always install a STATIC
// (compile-time-constant) key, exactly like SetNamed's own transition-cache
// assumes (TransitionEntry has no key field). Op::FinalizeComputedProperty
// is deliberately NOT covered -- its key varies per execution of the same
// site, which the key-less TransitionEntry cannot safely validate (same
// hazard KeyedFeedback solves for GetKeyed/SetKeyed).
void define_own_cached(Object* obj, const std::string& key, const Value& value, FeedbackSlot* fb) {
    if (!obj) return;
    write_barrier_for(obj, value);
    bool plain = obj->get_type() == Object::ObjectType::Ordinary && obj->is_extensible();
    // from_shape alone is the whole shape-side guard, unlike SetNamed's
    // version: CreateDataProperty never consults the prototype chain, and a
    // matching from_shape already proves the key has no slot there at all --
    // accessor slots included, since a transition is only ever learned for a
    // key the shape lacked. That leaves descriptors_, which is per-object and
    // usually absent outright, as the one override still worth asking about.
    if (plain && fb && !fb->transition_mega && !obj->find_descriptor_override(key)) {
        Shape* shape = obj->get_shape();
        for (uint8_t i = 0; i < fb->transition_count; i++) {
            const auto& te = fb->transitions[i];
            if (te.from_shape == shape) {
                obj->add_shape_property_cached(key, value, te.to_shape);
                return;
            }
        }
    }
    // Anything not plain has to go through [[DefineOwnProperty]] proper, which
    // can run a Proxy trap, evaluate a deferred module, or fail -- and a failure
    // throws. An object literal is always plain; `this` in a class field
    // initialiser is whatever the base constructor returned.
    if (!plain || obj->has_descriptor_override(key)) {
        PropertyDescriptor fdesc(value, static_cast<PropertyAttributes>(
            PropertyAttributes::Writable | PropertyAttributes::Enumerable |
            PropertyAttributes::Configurable));
        if (!obj->set_property_descriptor(key, fdesc) &&
            Object::current_context_ && !Object::current_context_->has_exception()) {
            Object::current_context_->throw_type_error("Cannot define field '" + key + "'");
        }
        return;
    }
    Shape* shape_before = obj->get_shape();
    bool was_new = fb && shape_before && shape_before->find_slot(key) < 0;
    obj->create_own_data_property(key, value);
    if (was_new && fb && !fb->transition_mega && !obj->has_descriptor_override(key)) {
        Shape* s = obj->get_shape();
        int32_t idx = s ? s->find_slot(key) : -1;
        if (idx >= 0) {
            learn_transition(fb, shape_before, s, nullptr, static_cast<uint32_t>(idx), 0, nullptr);
        }
    }
}

// FinalizeStaticProperty's Getter/Setter shape-transition cache, mirroring
// define_own_cached above. Only offered to a genuinely new own key (a
// getter+setter pair sharing a key routes its second accessor through the
// slow-path merge below instead, same as before). Unlike the old design,
// a brand-new single accessor (getter XOR setter, default attrs, not an
// array index) now lives entirely in a pair of shape_slots_ entries via
// Shape::transition_accessor -- descriptors_ is never touched for this
// case at all (see Object::add_accessor_shape_property_cached and
// Object::has_descriptor_override's shape-accessor-slot check). A cache
// hit skips transition_accessor's own hash lookup; a cache miss still
// calls it directly (not the general/slow path) since it's the same O(1)
// memoized operation transition() already is for plain data properties.
void define_accessor_cached(Object* obj, const std::string& key, Function* fn, bool is_getter, FeedbackSlot* fb) {
    if (!obj) return;
    Collector::write_barrier(obj);
    size_t unused_idx;
    bool fast_path_eligible = obj->get_type() == Object::ObjectType::Ordinary &&
        obj->is_extensible() && !obj->has_own_property(key) && !key_is_canonical_index(key, unused_idx);

    if (fast_path_eligible) {
        Value getter_v = is_getter ? Value(fn) : Value();
        Value setter_v = is_getter ? Value() : Value(fn);
        // from_shape alone, same reasoning as define_own_cached's.
        if (fb && !fb->transition_mega) {
            Shape* shape = obj->get_shape();
            for (uint8_t i = 0; i < fb->transition_count; i++) {
                const auto& te = fb->transitions[i];
                if (te.from_shape == shape) {
                    obj->add_accessor_shape_property_cached(key, getter_v, setter_v, te.to_shape);
                    return;
                }
            }
        }
        Shape* shape_before = obj->get_shape();
        Shape* to_shape = shape_before ? shape_before->transition_accessor(key) : nullptr;
        if (to_shape) {
            obj->add_accessor_shape_property_cached(key, getter_v, setter_v, to_shape);
            if (fb && !fb->transition_mega) {
                learn_transition(fb, shape_before, to_shape, nullptr, 0, 0, nullptr);
            }
            return;
        }
        // transition_accessor refused (kMaxSlots/kMaxTransitions exceeded)
        // -- fall through to the general path below.
    }

    PropertyDescriptor desc = obj->has_own_property(key)
        ? obj->get_property_descriptor(key) : PropertyDescriptor();
    if (is_getter) desc.set_getter(fn); else desc.set_setter(fn);
    desc.set_enumerable(true);
    desc.set_configurable(true);
    obj->set_property_descriptor(key, desc);
}

// Dedups on (shape, key): unlike learn_feedback (FeedbackSlot, one fixed
// name per site), the same GetKeyed/SetKeyed site can legitimately learn
// several different keys against the same shape (e.g. `obj[k]` in a loop
// where k varies over a small bounded set) -- each (shape, key) pair is a
// separate slot in the fixed budget.
void learn_keyed(KeyedFeedback* fb, Shape* shape, const std::string& key, uint32_t slot_index) {
    for (uint8_t i = 0; i < fb->count; i++) {
        if (fb->entries[i].shape == shape && fb->entries[i].key == key) return;
    }
    if (fb->count < KeyedFeedback::kMaxEntries) {
        fb->entries[fb->count++] = {shape, key, slot_index};
    } else {
        fb->mega = true;
    }
}

// A key that is already a number and is exactly a canonical array index.
// Everything else, including fractions, negatives and the out-of-range
// doubles, falls through to ToPropertyKey. -0 lands on 0, which is what
// ToPropertyKey produces for it as well.
inline bool array_index_key(const Value& v, uint32_t& out) {
    if (!v.is_number()) return false;
    double d = v.as_number();
    // Bound first: casting a double outside the uint32 range is undefined.
    if (!(d >= 0.0 && d < 4294967295.0)) return false;
    uint32_t i = static_cast<uint32_t>(d);
    if (static_cast<double>(i) != d) return false;
    out = i;
    return true;
}

// Whether an indexed read or write may go straight to the dense element
// vector, skipping the key's string form entirely. has_only_dense_elements
// already rules out holes, per-index attributes, sparse storage and every
// receiver that is not a plain Array, so an in-bounds index here is an own
// data property: the read cannot owe anything to the prototype chain, and
// the write shadows it.
inline bool dense_element_slot(const Value& receiver, uint32_t index, Object*& out) {
    Object* obj = as_object_like(receiver);
    if (!obj || !obj->has_only_dense_elements() || index >= obj->element_count()) return false;
    out = obj;
    return true;
}

// The same question for a WRITE, which may also extend the array by one.
// A read at the element count is out of bounds and answers undefined, which is
// why the two cannot share a bound -- but a write there appends, and appending
// through the dense vector is what push does.
//
// Overwriting and appending are not the same question, though. An index the
// array already owns shadows whatever the prototype chain holds, so nothing
// there can intervene. Creating one does not: [[Set]] walks the chain looking
// for a setter to run or a non-writable data property to refuse on, and the
// array has to be willing to take a new property at all. Both are asked here
// and only for the appending case.
inline bool dense_element_store_slot(const Value& receiver, uint32_t index, Object*& out) {
    Object* obj = as_object_like(receiver);
    if (!obj || !obj->has_only_dense_elements()) return false;
    const uint32_t count = obj->element_count();
    if (index > count) return false;  // past the end: a hole, for the general path to place
    if (index == count &&
        (!obj->is_extensible() || !obj->proto_chain_has_no_indices())) {
        return false;
    }
    out = obj;
    return true;
}

// The same question for a typed array. current_length() is the spec-current
// length, and already answers 0 for a detached buffer and for a view whose
// window no longer fits its resizable buffer, so one bound test covers those.
// Fractional, negative and out-of-range keys deliberately stay on the generic
// path: there a canonical numeric index reads as undefined and writes as a
// silent no-op without ever consulting the prototype, which is not something
// this shortcut should be trying to reproduce.
inline bool typed_element_slot(const Value& receiver, uint32_t index, TypedArrayBase*& out) {
    Object* obj = as_object_like(receiver);
    if (!obj || obj->get_type() != Object::ObjectType::TypedArray) return false;
    auto* ta = static_cast<TypedArrayBase*>(obj);
    if (index >= ta->current_length()) return false;
    out = ta;
    return true;
}

// GetKeyed's own cache: get_named()'s FeedbackSlot-based cache can't be
// reused directly here because GetNamed's `name` is a compile-time constant
// per bytecode site, while GetKeyed's key comes from a register and can
// differ on every execution of the SAME instruction -- a shape-only hit
// would return the wrong property's value for a different key on an object
// of the same shape. Entries validate both. Falls through to the ordinary,
// unmodified get_named() slow path (fb=nullptr) on any miss -- own-property
// only, no inherited-property (proto) cache here.
Value get_keyed(Context& ctx, const Value& receiver, const std::string& key, KeyedFeedback* fb) {
    Object* obj = as_object_like(receiver);
    if (obj && fb && !fb->mega && obj->get_type() == Object::ObjectType::Ordinary &&
        !obj->has_descriptor_override(key)) {
        Shape* shape = obj->get_shape();
        for (uint8_t i = 0; i < fb->count; i++) {
            if (fb->entries[i].shape == shape && fb->entries[i].key == key) {
                const Value* slot = obj->get_shape_slot_unchecked(fb->entries[i].slot_index);
                if (slot) return *slot;
                break;
            }
        }
    }
    Value result = get_named(ctx, receiver, key, nullptr, nullptr, false);
    if (!ctx.has_exception() && obj && fb && !fb->mega &&
        obj->get_type() == Object::ObjectType::Ordinary && !obj->has_descriptor_override(key)) {
        Shape* s = obj->get_shape();
        int32_t idx = s ? s->find_slot(key) : -1;
        if (idx >= 0) learn_keyed(fb, s, key, static_cast<uint32_t>(idx));
    }
    return result;
}

// SetKeyed's own cache, symmetric to get_keyed above. Falls through to the
// ordinary, unmodified set_named() slow path (fb=nullptr) on any miss.
void set_keyed(Context& ctx, const Value& receiver, const std::string& key,
                const Value& value, KeyedFeedback* fb) {
    Object* obj = as_object_like(receiver);
    if (obj && fb && !fb->mega && obj->get_type() == Object::ObjectType::Ordinary &&
        !obj->has_descriptor_override(key)) {
        Shape* shape = obj->get_shape();
        for (uint8_t i = 0; i < fb->count; i++) {
            if (fb->entries[i].shape == shape && fb->entries[i].key == key) {
                Value* slot = obj->get_shape_slot_unchecked(fb->entries[i].slot_index);
                if (slot) { write_barrier_for(obj, value); *slot = value; return; }
                break;
            }
        }
    }
    set_named(ctx, receiver, key, value, nullptr, nullptr);
    if (!ctx.has_exception() && obj && fb && !fb->mega &&
        obj->get_type() == Object::ObjectType::Ordinary && !obj->has_descriptor_override(key)) {
        Shape* s = obj->get_shape();
        int32_t idx = s ? s->find_slot(key) : -1;
        if (idx >= 0) learn_keyed(fb, s, key, static_cast<uint32_t>(idx));
    }
}

// Literal `.#name` access, mirroring the tree-walker's private member paths
// (MemberExpression::evaluate / AssignmentExpression's private branch).
// The IC caches the site's resolved qualified key: private fields live in
// sparse overflow (not shape slots), and a present qualified slot IS the
// brand proof, so the fast path is one map lookup with no brand walk.
Value get_private(Context& ctx, const Value& receiver, const std::string& name, PrivateFeedback* pf) {
    Object* obj = as_object_like(receiver);
    if (!obj) {
        ctx.throw_type_error("Cannot read private member " + name + " from an object whose class did not declare it");
        return Value();
    }
    if (pf && !pf->qualified.empty()) {
        if (const Value* slot = obj->private_field_slot(pf->qualified)) return *slot;
    }
    if (!private_brand_check(ctx, obj, name)) {
        if (!ctx.has_exception()) {
            ctx.throw_type_error("Cannot read private member " + name + " from an object whose class did not declare it");
        }
        return Value();
    }
    std::string qualified = resolve_private_storage_key(name, obj);
    if (obj->has_private_slot(qualified)) {
        PropertyDescriptor own_d;
        if (obj->get_private_slot_descriptor(qualified, own_d) && own_d.is_accessor_descriptor()) {
            if (!own_d.has_getter()) {
                ctx.throw_type_error("'" + name + "' accessor has no getter");
                return Value();
            }
            Function* getter_fn = as_function(own_d.get_getter());
            return getter_fn ? getter_fn->call_register_args(ctx, {}, receiver) : Value();
        }
        if (pf) {
            if (const Value* slot = obj->private_field_slot(qualified)) {
                pf->qualified = qualified;
                return *slot;
            }
        }
        return obj->get_private_slot_value(qualified);
    }
    // Methods/accessors live under the qualified key on the declaring
    // prototype/constructor.
    if (Object* owner = resolve_private_accessor_owner(name)) {
        PropertyDescriptor d = owner->get_property_descriptor(qualified);
        bool use_qualified = d.is_accessor_descriptor() || d.has_value();
        if (!use_qualified) d = owner->get_property_descriptor(name);
        const std::string& used = use_qualified ? qualified : name;
        if (d.is_accessor_descriptor()) {
            if (!d.has_getter()) {
                ctx.throw_type_error("'" + used + "' accessor has no getter");
                return Value();
            }
            Function* getter_fn = as_function(d.get_getter());
            return getter_fn ? getter_fn->call_register_args(ctx, {}, receiver) : Value();
        }
        if (d.has_value()) return owner->get_property(used);
        return Value();
    }
    // No declaring frame (e.g. resumed past an await/yield): scan the chain.
    for (Object* lookup = obj; lookup; lookup = lookup->get_prototype()) {
        PropertyDescriptor d = lookup->get_property_descriptor(qualified);
        bool use_qualified = d.is_accessor_descriptor() || d.has_value();
        if (!use_qualified) d = lookup->get_property_descriptor(name);
        const std::string& used = use_qualified ? qualified : name;
        if (d.is_accessor_descriptor()) {
            if (!d.has_getter()) {
                ctx.throw_type_error("'" + used + "' accessor has no getter");
                return Value();
            }
            Function* getter_fn = as_function(d.get_getter());
            return getter_fn ? getter_fn->call_register_args(ctx, {}, receiver) : Value();
        }
        if (d.has_value()) return lookup->get_property(used);
    }
    return Value();
}

void set_private(Context& ctx, const Value& receiver, const std::string& name,
                 const Value& value, PrivateFeedback* pf) {
    Object* obj = as_object_like(receiver);
    if (!obj) {
        ctx.throw_type_error("Cannot write private member " + name + " to an object whose class did not declare it");
        return;
    }
    write_barrier_for(obj, value);
    if (pf && !pf->qualified.empty()) {
        if (Value* slot = obj->private_field_slot(pf->qualified)) { *slot = value; return; }
    }
    if (!private_brand_check(ctx, obj, name, /*require_exists=*/false)) {
        if (!ctx.has_exception()) {
            ctx.throw_type_error("Cannot write private member " + name + " to an object whose class did not declare it");
        }
        return;
    }
    std::string qualified = resolve_private_storage_key(name, obj);
    if (obj->has_private_slot(qualified)) {
        PropertyDescriptor own_pd;
        if (obj->get_private_slot_descriptor(qualified, own_pd) && own_pd.is_accessor_descriptor()) {
            if (!own_pd.has_setter()) {
                ctx.throw_type_error("'" + qualified + "' was defined without a setter");
                return;
            }
            Function* setter_fn = as_function(own_pd.get_setter());
            if (setter_fn) setter_fn->call(ctx, {value}, receiver);
            return;
        }
        if (own_pd.has_value() && own_pd.get_value().is_function()) {
            Function* mfn = own_pd.get_value().as_function();
            if (mfn && mfn->has_internal_slot("__private_class_brand__")) {
                ctx.throw_type_error("'" + qualified + "' is a private method and cannot be assigned to");
                return;
            }
        }
        if (pf) {
            if (Value* slot = obj->private_field_slot(qualified)) {
                pf->qualified = qualified;
                *slot = value;
                return;
            }
        }
        obj->set_private_slot_value(qualified, value);
        return;
    }
    // Method/accessor on the declaring prototype/constructor.
    PropertyDescriptor pd;
    bool found = false;
    std::string used = qualified;
    if (Object* owner = resolve_private_accessor_owner(name)) {
        pd = owner->get_property_descriptor(qualified);
        found = pd.has_value() || pd.is_accessor_descriptor();
        if (!found) {
            pd = owner->get_property_descriptor(name);
            found = pd.has_value() || pd.is_accessor_descriptor();
            if (found) used = name;
        }
    }
    if (!found) {
        for (Object* proto = obj->get_prototype(); proto; proto = proto->get_prototype()) {
            pd = proto->get_property_descriptor(qualified);
            if (pd.has_value() || pd.is_accessor_descriptor()) { found = true; break; }
            pd = proto->get_property_descriptor(name);
            if (pd.has_value() || pd.is_accessor_descriptor()) { found = true; used = name; break; }
        }
    }
    if (!found) {
        ctx.throw_type_error("Cannot set private field " + name + " on an object that has not been initialized");
        return;
    }
    if (pd.is_accessor_descriptor()) {
        if (!pd.has_setter()) {
            ctx.throw_type_error("'" + used + "' was defined without a setter");
            return;
        }
        Function* setter_fn = as_function(pd.get_setter());
        if (setter_fn) setter_fn->call(ctx, {value}, receiver);
        return;
    }
    if (pd.has_value() && pd.get_value().is_function()) {
        ctx.throw_type_error("'" + used + "' is a private method and cannot be assigned to");
        return;
    }
    obj->ordinary_set(used, value);
}

}

// Everything the dispatch loop reads or writes, so the loop can live in a
// function of its own with no exception-handling region in it. run() keeps
// the try/catch, the register bank and the env side-stack; this only hands
// the loop pointers to them.
struct Frame {
    const BytecodeChunk& chunk;
    Context& ctx;
    std::span<const Value> args;
    Function* owner;
    // Whether this frame's inline caches may hold cells: true when a function
    // owns the chunk and traces it, and true for the top-level script, whose
    // chunk run_script roots for the duration. Separate from `owner`, which
    // also decides ROUTING -- a chunk shared across instances needs each
    // instance's own lookup/private caches, and only a function has those.
    bool feedback_rooted;
    Value* regs;
    Environment** env_saves;
    BytecodeChunk::LookupCacheEntry* lookup_cache_data;
    PrivateFeedback* private_feedback_data;
    const uint8_t* code;
    const Value* constants;
    Environment* entry_env;
    // Written by the loop and read by run() after an exception unwinds out of
    // it, so these cannot be locals of the dispatch function.
    Value this_value;
    Value acc;
    uint32_t pc;
    uint32_t instr_pc;
    uint8_t env_save_top;
    bool this_resolved;
};

// Tail-call threaded dispatch.
//
// This started as a single switch over every opcode, which kept the
// interpreter's state on the stack: run's frame had 133 distinct slots and
// even `code`, a pointer that never changes, was reloaded per opcode -- 48
// instructions and ~15 loads for an opcode like Ldar that needs about four. A
// handler per opcode carries the hot state in argument registers instead, and
// musttail makes each one reuse the same machine frame rather than growing
// the stack.
//
// `pc` names the opcode byte itself, never its operands, so a handler that
// only covers the common shape can hand that same pc to a slower one.
using Handler = Value (*)(Frame&, uint32_t, Value);

extern const std::array<Handler, 256> kHandlers;

#define DISPATCH() [[clang::musttail]] return kHandlers[f.code[pc]](f, pc, acc)

// Opcodes that cannot raise: nothing but the dispatch separates one from the
// next, no exception check in between.
#define CONST_HANDLER(name, val)                                           \
    Value name(Frame& f, uint32_t pc, Value acc) {                         \
        acc = (val);                                                       \
        pc += 1;                                                           \
        DISPATCH();                                                        \
    }

CONST_HANDLER(h_LdaZero, Value(0.0))
CONST_HANDLER(h_LdaUndefined, Value())
CONST_HANDLER(h_LdaNull, Value::null())
CONST_HANDLER(h_LdaTrue, Value(true))
CONST_HANDLER(h_LdaFalse, Value(false))

Value h_Ldar(Frame& f, uint32_t pc, Value acc) {
    acc = f.regs[f.code[pc + 1]];
    pc += 2;
    DISPATCH();
}

Value h_Star(Frame& f, uint32_t pc, Value acc) {
    f.regs[f.code[pc + 1]] = acc;
    pc += 2;
    DISPATCH();
}

Value h_Mov(Frame& f, uint32_t pc, Value acc) {
    f.regs[f.code[pc + 2]] = f.regs[f.code[pc + 1]];
    pc += 3;
    DISPATCH();
}

Value h_LdaSmi(Frame& f, uint32_t pc, Value acc) {
    acc = Value(static_cast<double>(static_cast<int8_t>(f.code[pc + 1])));
    pc += 2;
    DISPATCH();
}

// The pair fuse_store_pairs rewrites: produce, then park. Each does exactly
// what its two instructions did, in the same order, so the accumulator is
// left holding the same value and only one dispatch is paid for.
Value h_LdarStar(Frame& f, uint32_t pc, Value acc) {
    acc = f.regs[f.code[pc + 1]];
    f.regs[f.code[pc + 2]] = acc;
    pc += 3;
    DISPATCH();
}

Value h_LdaSmiStar(Frame& f, uint32_t pc, Value acc) {
    acc = Value(static_cast<double>(static_cast<int8_t>(f.code[pc + 1])));
    f.regs[f.code[pc + 2]] = acc;
    pc += 3;
    DISPATCH();
}

Value h_LdaZeroStar(Frame& f, uint32_t pc, Value acc) {
    acc = Value(0.0);
    f.regs[f.code[pc + 1]] = acc;
    pc += 2;
    DISPATCH();
}

Value h_LdaConstStar(Frame& f, uint32_t pc, Value acc) {
    acc = f.constants[read_u16(f.code, pc + 1)];
    f.regs[f.code[pc + 3]] = acc;
    pc += 4;
    DISPATCH();
}

Value h_LdaConst(Frame& f, uint32_t pc, Value acc) {
    acc = f.constants[read_u16(f.code, pc + 1)];
    pc += 3;
    DISPATCH();
}

Value h_Return(Frame& f, uint32_t pc, Value acc) {
    (void)f; (void)pc;
    return acc;
}

Value h_Jump(Frame& f, uint32_t pc, Value acc) {
    int16_t off = read_i16(f.code, pc + 1);
    pc += 3 + off;
    if (off < 0) Collector::safepoint();
    DISPATCH();
}

#define BRANCH_HANDLER(name, cond)                                         \
    Value name(Frame& f, uint32_t pc, Value acc) {                         \
        int16_t off = read_i16(f.code, pc + 1);                            \
        pc += 3;                                                           \
        if (cond) {                                                        \
            pc += off;                                                     \
            if (off < 0) Collector::safepoint();                           \
        }                                                                  \
        DISPATCH();                                                        \
    }

BRANCH_HANDLER(h_JumpIfFalse, !acc.to_boolean())
BRANCH_HANDLER(h_JumpIfTrue, acc.to_boolean())

// Numeric fast paths only. Anything else re-enters the switch at this same
// opcode and runs through the shared slow path exactly as before, so a
// handler never duplicates the coercion, the BigInt case or the raise check.

#undef CONST_HANDLER
#undef BRANCH_HANDLER

// On exception, find the innermost handler covering instr_pc and move pc
// to it. NOT a do-while(0) macro at the point of use: the `continue` is
// meant to leave the do/while(0) that wraps a generated handler body, so
// the rest of that body stops running with pc already pointed at the
// handler -- which would clobber the exception value in acc (found via
// ~{valueOf(){throw}} in a try).
#define CHECK_EXC()                                                        \
    if (ctx.has_exception()) {                                            \
        int32_t handler_pc = -1;                                           \
        uint32_t best_width = UINT32_MAX;                                  \
        if (chunk.handlers) for (const auto& h : *chunk.handlers) {       \
            if (instr_pc >= h.start_pc && instr_pc < h.end_pc) {           \
                uint32_t width = h.end_pc - h.start_pc;                    \
                if (width < best_width) { best_width = width; handler_pc = static_cast<int32_t>(h.handler_pc); } \
            }                                                              \
        }                                                                  \
        if (handler_pc < 0) return Value();                               \
        acc = ctx.get_exception();                                        \
        ctx.clear_exception();                                            \
        pc = static_cast<uint32_t>(handler_pc);                           \
        continue;                                                          \
    } else ((void)0)

#define BINARY_OP(binop, expr)                                             \
    do {                                                                   \
        const Value& lhs = regs[code[pc]];                                 \
        pc += 1;                                                           \
        if (LIKELY(lhs.is_finite_double() && acc.is_finite_double())) {    \
            double l = lhs.as_finite_double();                             \
            double r = acc.as_finite_double();                             \
            (void)l; (void)r;                                              \
            acc = (expr);                                                  \
        } else {                                                           \
            acc = binary_slow(ctx, binop, lhs, acc);                       \
            CHECK_EXC();                                                   \
        }                                                                  \
    } while (0)

// The check the switch ran after every case, for an opcode that set an
// exception on the context without saying so. Unlike CHECK_EXC there is no
// loop to continue to: it only moves pc, and the DISPATCH that follows
// carries on from the handler. CHECK_EXC itself stays usable inside a
// generated body -- its `continue` binds to the do/while(0) wrapper and
// leaves it, which is exactly where the epilogue picks up.
#define CHECK_EXC_TAIL()                                                   \
    if (ctx.has_exception()) {                                            \
        int32_t handler_pc = -1;                                           \
        uint32_t best_width = UINT32_MAX;                                  \
        if (chunk.handlers) for (const auto& h : *chunk.handlers) {       \
            if (instr_pc >= h.start_pc && instr_pc < h.end_pc) {           \
                uint32_t width = h.end_pc - h.start_pc;                    \
                if (width < best_width) { best_width = width; handler_pc = static_cast<int32_t>(h.handler_pc); } \
            }                                                              \
        }                                                                  \
        if (handler_pc < 0) return Value();                               \
        acc = ctx.get_exception();                                        \
        ctx.clear_exception();                                            \
        pc = static_cast<uint32_t>(handler_pc);                           \
    } else ((void)0)

// Two finite doubles is the whole fast form; a string, a BigInt or an object
// with valueOf goes to binary_slow, and the split matters because the fast
// half stays small enough to keep its operands in registers. Putting both in
// one handler made every iteration of a numeric loop carry the prologue the
// slow half needs -- chunk, ctx, instr_pc -- to reach a call it never makes.
#define NUMERIC_BINARY_HANDLER(name, binop, expr)                          \
    Value name##_slow(Frame& f, uint32_t pc, Value acc) {                  \
        const BytecodeChunk& chunk = f.chunk;                              \
        Context& ctx = f.ctx;                                              \
        uint32_t& instr_pc = f.instr_pc;                                   \
        instr_pc = pc;                                                     \
        acc = binary_slow(ctx, binop, f.regs[f.code[pc + 1]], acc);        \
        pc += 2;                                                           \
        CHECK_EXC_TAIL();                                                  \
        DISPATCH();                                                        \
    }                                                                      \
    Value name(Frame& f, uint32_t pc, Value acc) {                         \
        const Value& lhs = f.regs[f.code[pc + 1]];                         \
        if (LIKELY(lhs.is_finite_double() && acc.is_finite_double())) {    \
            double l = lhs.as_finite_double();                             \
            double r = acc.as_finite_double();                             \
            (void)l; (void)r;                                              \
            acc = (expr);                                                  \
            pc += 2;                                                       \
            DISPATCH();                                                    \
        }                                                                  \
        [[clang::musttail]] return name##_slow(f, pc, acc);                \
    }

NUMERIC_BINARY_HANDLER(h_Add, BinOp::ADD, Value(l + r))
NUMERIC_BINARY_HANDLER(h_Sub, BinOp::SUBTRACT, Value(l - r))
NUMERIC_BINARY_HANDLER(h_Mul, BinOp::MULTIPLY, Value(l * r))
NUMERIC_BINARY_HANDLER(h_TestLt, BinOp::LESS_THAN, Value(l < r))
NUMERIC_BINARY_HANDLER(h_TestGt, BinOp::GREATER_THAN, Value(l > r))
NUMERIC_BINARY_HANDLER(h_TestLe, BinOp::LESS_EQUAL, Value(l <= r))
NUMERIC_BINARY_HANDLER(h_TestGe, BinOp::GREATER_EQUAL, Value(l >= r))
NUMERIC_BINARY_HANDLER(h_TestEq, BinOp::EQUAL, Value(l == r))
NUMERIC_BINARY_HANDLER(h_TestNe, BinOp::NOT_EQUAL, Value(l != r))
NUMERIC_BINARY_HANDLER(h_TestStrictEq, BinOp::STRICT_EQUAL, Value(l == r))
NUMERIC_BINARY_HANDLER(h_TestStrictNe, BinOp::STRICT_NOT_EQUAL, Value(l != r))

// A bitwise op is a couple of instructions of real work, so the handler stays
// as lean as the arithmetic one and splits its fallback out the same way.
// ToInt32 already answers for NaN and the infinities, so being a number is the
// entire gate; a string, a BigInt or an object with valueOf takes the slow
// half, which reaches binary_slow directly.
#define BITWISE_BINARY_HANDLER(name, binop, expr)                          \
    Value name##_slow(Frame& f, uint32_t pc, Value acc) {                  \
        const BytecodeChunk& chunk = f.chunk;                              \
        Context& ctx = f.ctx;                                              \
        uint32_t& instr_pc = f.instr_pc;                                   \
        instr_pc = pc;                                                     \
        acc = binary_slow(ctx, binop, f.regs[f.code[pc + 1]], acc);        \
        pc += 2;                                                           \
        CHECK_EXC_TAIL();                                                  \
        DISPATCH();                                                        \
    }                                                                      \
    Value name(Frame& f, uint32_t pc, Value acc) {                         \
        const Value& lhs = f.regs[f.code[pc + 1]];                         \
        if (LIKELY(lhs.is_number() && acc.is_number())) {                  \
            int32_t l = js_to_int32(lhs.as_number());                      \
            int32_t r = js_to_int32(acc.as_number());                      \
            (void)l; (void)r;                                              \
            acc = (expr);                                                  \
            pc += 2;                                                       \
            DISPATCH();                                                    \
        }                                                                  \
        [[clang::musttail]] return name##_slow(f, pc, acc);                \
    }

BITWISE_BINARY_HANDLER(h_BitAnd, BinOp::BITWISE_AND, Value(static_cast<double>(l & r)))
BITWISE_BINARY_HANDLER(h_BitOr,  BinOp::BITWISE_OR,  Value(static_cast<double>(l | r)))
BITWISE_BINARY_HANDLER(h_BitXor, BinOp::BITWISE_XOR, Value(static_cast<double>(l ^ r)))
BITWISE_BINARY_HANDLER(h_Shl, BinOp::LEFT_SHIFT,
    Value(static_cast<double>(static_cast<int32_t>(static_cast<uint32_t>(l) << (r & 31)))))
BITWISE_BINARY_HANDLER(h_Sar, BinOp::RIGHT_SHIFT, Value(static_cast<double>(l >> (r & 31))))
BITWISE_BINARY_HANDLER(h_Shr, BinOp::UNSIGNED_RIGHT_SHIFT,
    Value(static_cast<double>(static_cast<uint32_t>(l) >> (r & 31))))

// ToNumeric plus the BigInt step, which is everything Inc and Dec do once the
// accumulator turns out not to be a plain number.
static Value step_numeric(Context& ctx, const Value& acc, double delta) {
    Value numeric = acc;
    if (!numeric.is_number() && !numeric.is_bigint()) {
        numeric = UnaryExpression::to_numeric(ctx, numeric);
        if (ctx.has_exception()) return Value();
    }
    if (numeric.is_bigint()) {
        return Value(new BigInt(delta > 0 ? *numeric.as_bigint() + BigInt(1)
                                          : *numeric.as_bigint() - BigInt(1)));
    }
    return Value(numeric.to_number() + delta);
}

#define UNARY_STEP_HANDLER(name, delta)                                    \
    Value name##_slow(Frame& f, uint32_t pc, Value acc) {                  \
        const BytecodeChunk& chunk = f.chunk;                              \
        Context& ctx = f.ctx;                                              \
        uint32_t& instr_pc = f.instr_pc;                                   \
        instr_pc = pc;                                                     \
        acc = step_numeric(ctx, acc, (delta));                             \
        pc += 1;                                                           \
        CHECK_EXC_TAIL();                                                  \
        DISPATCH();                                                        \
    }                                                                      \
    Value name(Frame& f, uint32_t pc, Value acc) {                         \
        if (LIKELY(acc.is_number())) {                                     \
            acc = Value(acc.as_number() + (delta));                        \
            pc += 1;                                                       \
            DISPATCH();                                                    \
        }                                                                  \
        [[clang::musttail]] return name##_slow(f, pc, acc);                \
    }

UNARY_STEP_HANDLER(h_Inc, 1.0)
UNARY_STEP_HANDLER(h_Dec, -1.0)

Value h_gen_LdaThis(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    bool& this_resolved = f.this_resolved;
    Value& this_value = f.this_value;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                // Derived-constructor this-TDZ (spec 9.1.1.3.4 GetThisBinding):
                // mirrors Identifier::evaluate's check for "this" -- must be
                // re-checked on every read, not just once, since a `super()`
                // call between two reads flips it mid-frame.
                if (ctx.this_needs_super()) {
                    ctx.throw_reference_error("Must call super constructor before accessing 'this' in derived class constructor");
                    CHECK_EXC();
                    break;
                }
                if (!this_resolved) {
                    // Same resolution as LdaLookup 'this': the binding is
                    // created by Function::call (arrows find the outer one
                    // through the chain).
                    Environment* env = ctx.find_binding_env("this");
                    if (env) {
                        this_value = env->get_binding_direct("this", &ctx);
                    } else if (ctx.has_binding("this")) {
                        this_value = ctx.get_binding("this");
                    }
                    CHECK_EXC();
                    this_resolved = true;
                }
                acc = this_value;
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_LdaTdz(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                acc = Value::vm_tdz_sentinel();
                break;
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_LdarChecked(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    Value* regs = f.regs;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint8_t reg = code[pc];
                uint16_t name_idx = read_u16(code, pc + 1);
                pc += 3;
                if (regs[reg].is_vm_tdz_sentinel()) {
                    ctx.throw_reference_error("Cannot access '" + chunk.name_at(name_idx) +
                                               "' before initialization");
                    CHECK_EXC();
                    break;
                }
                acc = regs[reg];
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_StarChecked(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    Value* regs = f.regs;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint8_t reg = code[pc];
                uint16_t name_idx = read_u16(code, pc + 1);
                pc += 3;
                if (regs[reg].is_vm_tdz_sentinel()) {
                    ctx.throw_reference_error("Cannot access '" + chunk.name_at(name_idx) +
                                               "' before initialization");
                    CHECK_EXC();
                    break;
                }
                regs[reg] = acc;
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_Div(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    Value* regs = f.regs;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                BINARY_OP(BinOp::DIVIDE, Value(l / r)); break;
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_Mod(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    Value* regs = f.regs;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                const Value& lhs = regs[code[pc]];
                pc += 1;
                if (LIKELY(lhs.is_number() && acc.is_number())) {
                    acc = Value(std::fmod(lhs.as_number(), acc.as_number()));
                } else {
                    acc = binary_slow(ctx, BinOp::MODULO, lhs, acc);
                    CHECK_EXC();
                }
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_Exp(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    Value* regs = f.regs;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                const Value& lhs = regs[code[pc]];
                pc += 1;
                acc = binary_slow(ctx, BinOp::EXPONENT, lhs, acc);
                CHECK_EXC();
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_TestInstanceOf(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    Value* regs = f.regs;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                const Value& lhs = regs[code[pc]];
                pc += 1;
                acc = binary_slow(ctx, BinOp::INSTANCEOF, lhs, acc);
                CHECK_EXC();
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_TestIn(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    Value* regs = f.regs;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                const Value& lhs = regs[code[pc]];
                pc += 1;
                acc = binary_slow(ctx, BinOp::IN, lhs, acc);
                CHECK_EXC();
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_Neg(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                if (acc.is_object() || acc.is_function()) {
                    acc = UnaryExpression::to_numeric(ctx, acc);
                    CHECK_EXC();
                }
                acc = acc.unary_minus();
                break;
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_LogicalNot(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                acc = acc.logical_not();
                break;
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_BitNot(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                if (acc.is_object() || acc.is_function()) {
                    acc = UnaryExpression::to_numeric(ctx, acc);
                    CHECK_EXC();
                }
                acc = acc.bitwise_not();
                break;
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_TypeOf(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                acc = acc.typeof_op();
                break;
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_ToNumber(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                if (acc.is_object() || acc.is_function()) {
                    acc = UnaryExpression::to_numeric(ctx, acc);
                    CHECK_EXC();
                    if (acc.is_bigint()) {
                        ctx.throw_type_error("Cannot convert a BigInt value to a number");
                        CHECK_EXC();
                        break;
                    }
                } else {
                    acc = acc.unary_plus();
                }
                break;
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_ToNumeric(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                acc = UnaryExpression::to_numeric(ctx, acc);
                CHECK_EXC();
                break;
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_ToTemplateString(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                acc = Value(TemplateLiteral::stringify_element(ctx, acc));
                CHECK_EXC();
                break;
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_ToPropertyKey(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                // A number is left alone: converting it is what the keyed
                // opcodes downstream already do for themselves, and unlike an
                // object key it has no valueOf/toString to observe, so running
                // the conversion here early buys nothing and costs every
                // compound element write its index string.
                if (!acc.is_string() && !acc.is_number()) {
                    acc = Value(acc.to_property_key());
                    CHECK_EXC();
                }
                break;
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_CheckObjectCoercible(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                if (acc.is_null() || acc.is_undefined()) {
                    ctx.throw_type_error(std::string("Cannot read properties of ") +
                        (acc.is_null() ? "null" : "undefined"));
                    CHECK_EXC();
                }
                break;
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_LdaLookup(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    BytecodeChunk::LookupCacheEntry* lookup_cache_data = f.lookup_cache_data;
    const uint8_t* code = f.code;
    Environment* entry_env = f.entry_env;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint16_t name_idx = read_u16(code, pc);
                pc += 2;
                {
                    // Captured-chain fast path: the resolved binding address is
                    // stable for this chunk's lifetime (see lookup_cache).
                    const auto& entry = lookup_cache_data[name_idx];
                    if (entry.obj_shape) {
                        Object* bo = entry.env->get_binding_object();
                        if (bo && bo->get_shape() == entry.obj_shape &&
                            entry.descriptor_epoch == Object::descriptor_epoch()) {
                            if (const Value* s = bo->get_shape_slot_unchecked(entry.obj_slot_index)) {
                                acc = *s;
                                break;
                            }
                        }
                    } else if (entry.slot) { acc = *entry.slot; break; }
                }
                // Mirrors Identifier::evaluate: TDZ first, then one scope-chain walk.
                const std::string* key = chunk.names[name_idx];
                const std::string& name = *key;
                if (ctx.is_in_tdz_interned(key)) {
                    ctx.throw_reference_error("Cannot access '" + name + "' before initialization");
                    CHECK_EXC();
                    break;
                }
                Environment* env = ctx.get_lexical_environment();
                bool found = false;
                for (; env; env = env->get_outer()) {
                    if (env->try_get_binding_interned(key, acc, &ctx)) { found = true; break; }
                    CHECK_EXC();
                }
                CHECK_EXC();
                if (found) {
                    if (env != entry_env) {
                        uint32_t obj_slot = 0;
                        bool slot_writable = false;
                        // See the identical block in the main dispatch loop.
                        if (Value* slot = env->stable_binding_slot(name, &slot_writable)) {
                            env->mark_referenced();
                            lookup_cache_data[name_idx] = {env, slot, nullptr, 0, 0, slot_writable};
                        } else if (env->cacheable_object_binding(name, obj_slot)) {
                            env->mark_referenced();
                            lookup_cache_data[name_idx] = {env, nullptr,
                                env->get_binding_object()->get_shape(),
                                Object::descriptor_epoch(), obj_slot};
                        }
                    }
                } else if (ctx.has_binding(name)) {
                    acc = ctx.get_binding(name);
                    CHECK_EXC();
                } else {
                    ctx.throw_reference_error("'" + name + "' is not defined");
                    CHECK_EXC();
                }
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_LdaLookupTypeof(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                // `typeof x` suppresses only the unresolved-binding case, not TDZ.
                const std::string& name = chunk.name_at(read_u16(code, pc));
                pc += 2;
                if (ctx.is_in_tdz(name)) {
                    ctx.throw_reference_error("Cannot access '" + name + "' before initialization");
                    CHECK_EXC();
                    break;
                }
                Environment* env = ctx.find_binding_env(name);
                CHECK_EXC();
                if (env) {
                    acc = env->get_binding_direct(name, &ctx);
                    CHECK_EXC();
                } else if (ctx.has_binding(name)) {
                    acc = ctx.get_binding(name);
                    CHECK_EXC();
                } else {
                    acc = Value();
                }
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_StaLookup(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    BytecodeChunk::LookupCacheEntry* lookup_cache_data = f.lookup_cache_data;
    const uint8_t* code = f.code;
    Environment* entry_env = f.entry_env;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint16_t sta_name_idx = read_u16(code, pc);
                pc += 2;
                {
                    const auto& entry = lookup_cache_data[sta_name_idx];
                    // The obj_shape form is LdaLookup's alone -- writing a
                    // global needs [[Set]]'s readonly/setter handling. And a
                    // const binding is cached for reading only: storing here
                    // would skip the TypeError the slow path raises.
                    if (entry.slot && !entry.obj_shape && entry.writable) {
                        // The barrier records "env gained a reference" for the
                        // remembered set -- storing a non-heap value can't.
                        if (acc.is_object() || acc.is_function() || acc.is_string() ||
                            acc.is_symbol() || acc.is_bigint()) {
                            Collector::write_barrier_env(entry.env);
                        }
                        *entry.slot = acc;
                        break;
                    }
                }
                // Mirrors AssignmentExpression's identifier PutValue. `with` and
                // direct eval bail out of the VM, so resolving the reference at
                // write time matches the tree-walker's captured-env behavior.
                const std::string* key = chunk.names[sta_name_idx];
                const std::string& name = *key;
                if (ctx.is_in_tdz_interned(key)) {
                    ctx.throw_reference_error("Cannot access '" + name + "' before initialization");
                    CHECK_EXC();
                    break;
                }
                Environment* env = ctx.find_binding_env_interned(key);
                CHECK_EXC();
                if (!env) {
                    if (ctx.is_strict_mode()) {
                        ctx.throw_reference_error("'" + name + "' is not defined");
                        CHECK_EXC();
                        break;
                    }
                    // Sloppy PutValue on an unresolvable reference: global object.
                    Object* global = ctx.get_global_object();
                    if (global) global->set_property(name, acc);
                    break;
                }
                if (env->get_type() == Environment::Type::Object && env->get_binding_object()) {
                    Object* bobj = env->get_binding_object();
                    if (!bobj->has_own_property(name) && ctx.is_strict_mode()) {
                        ctx.throw_reference_error("'" + name + "' is not defined");
                        CHECK_EXC();
                        break;
                    }
                    bool ok = bobj->set_property(name, acc);
                    if (!ok && (ctx.is_strict_mode() || ctx.is_strict_const(name))) {
                        ctx.throw_type_error("Assignment to constant variable '" + name + "'");
                    }
                } else {
                    bool ok = env->set_binding(name, acc);
                    if (!ok && (ctx.is_strict_mode() || ctx.is_strict_const(name))) {
                        ctx.throw_type_error("Assignment to constant variable '" + name + "'");
                    } else if (ok && env != entry_env) {
                        bool slot_writable = false;
                        Value* slot = env->stable_binding_slot(name, &slot_writable);
                        if (slot && slot_writable) {
                            env->mark_referenced();  // see Op::LdaLookup's note
                            lookup_cache_data[sta_name_idx] = {env, slot, nullptr, 0, 0, true};
                        }
                    }
                }
                CHECK_EXC();
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_CheckLookupResolvable(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                const std::string& name = chunk.name_at(read_u16(code, pc));
                pc += 2;
                acc = Value(ctx.find_binding_env(name) != nullptr || ctx.has_binding(name));
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

// A name the chunk has already resolved reads and writes through a cached
// binding address: one load, or one store plus a barrier. The generated
// handlers reach that in a few instructions but pay a fixed prologue first
// (five frame fields plus the instr_pc store an exception would need), which
// is most of the cost once the cache is warm. A script's top-level let/const
// is reached this way on every access, so the warm path gets a handler of its
// own and everything else tail-calls the generated one unchanged.
Value h_LdaLookupFast(Frame& f, uint32_t pc, Value acc) {
    const auto& entry = f.lookup_cache_data[read_u16(f.code, pc + 1)];
    if (LIKELY(entry.obj_shape)) {
        // A name bound on the global object, which is where a script's `var`
        // and its function declarations live -- so calling a top-level
        // function reaches its callee through here on every call.
        Object* bo = entry.env->get_binding_object();
        if (LIKELY(bo && bo->get_shape() == entry.obj_shape &&
                   entry.descriptor_epoch == Object::descriptor_epoch())) {
            if (const Value* s = bo->get_shape_slot_unchecked(entry.obj_slot_index)) {
                acc = *s;
                pc += 3;
                DISPATCH();
            }
        }
    } else if (LIKELY(entry.slot)) {
        acc = *entry.slot;
        pc += 3;
        DISPATCH();
    }
    [[clang::musttail]] return h_gen_LdaLookup(f, pc, acc);
}

Value h_StaLookupFast(Frame& f, uint32_t pc, Value acc) {
    const auto& entry = f.lookup_cache_data[read_u16(f.code, pc + 1)];
    if (LIKELY(entry.slot && !entry.obj_shape && entry.writable)) {
        if (acc.is_object() || acc.is_function() || acc.is_string() ||
            acc.is_symbol() || acc.is_bigint()) {
            Collector::write_barrier_env(entry.env);
        }
        *entry.slot = acc;
        pc += 3;
        DISPATCH();
    }
    [[clang::musttail]] return h_gen_StaLookup(f, pc, acc);
}

Value h_gen_StaLookupChecked(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    Value* regs = f.regs;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint8_t resolved_reg = code[pc];
                const std::string& name = chunk.name_at(read_u16(code, pc + 1));
                pc += 3;
                if (!regs[resolved_reg].to_boolean()) {
                    // Unresolvable BEFORE the RHS ran -- honor that verdict
                    // even if the RHS just created the binding (e.g.
                    // `x = (this.x = 1)`): PutValue resolves before GetValue
                    // of the RHS, spec 13.15.2 step 1-4.
                    if (ctx.is_strict_mode()) {
                        ctx.throw_reference_error("'" + name + "' is not defined");
                        CHECK_EXC();
                        break;
                    }
                    Object* global = ctx.get_global_object();
                    if (global) global->set_property(name, acc);
                    break;
                }
                if (ctx.is_in_tdz(name)) {
                    ctx.throw_reference_error("Cannot access '" + name + "' before initialization");
                    CHECK_EXC();
                    break;
                }
                Environment* env = ctx.find_binding_env(name);
                CHECK_EXC();
                if (env) {
                    bool ok = env->set_binding(name, acc);
                    if (!ok && (ctx.is_strict_mode() || ctx.is_strict_const(name))) {
                        ctx.throw_type_error("Assignment to constant variable '" + name + "'");
                    }
                } else if (ctx.has_binding(name)) {
                    // Object environment record (global/with) path.
                    ctx.set_binding(name, acc);
                } else {
                    // Resolvable before the RHS ran, but the RHS deleted the
                    // binding (e.g. `x = (delete global.x, 2)`) --
                    // SetMutableBinding's own HasBinding check now fails.
                    if (ctx.is_strict_mode()) {
                        ctx.throw_reference_error("'" + name + "' is not defined");
                        CHECK_EXC();
                        break;
                    }
                    Object* global = ctx.get_global_object();
                    if (global) global->set_property(name, acc);
                }
                CHECK_EXC();
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_LdaEnv(Frame& f, uint32_t pc, Value acc);

// Only the read that the declarative chain answers outright. `this` is not in
// any environment, an object environment on the way has to be asked in the
// order the full handler asks it, and a hole has to be reported -- all three
// tail-call below.
Value h_LdaEnvFast(Frame& f, uint32_t pc, Value acc) {
    static const std::string* kThis = Shape::intern("this");
    const std::string* key = f.chunk.names[read_u16(f.code, pc + 1)];
    if (LIKELY(key != kThis)) {
        if (Environment* env = f.ctx.get_lexical_environment()) {
            Value out;
            if (LIKELY(env->try_read_declarative_chain(key, out))) {
                acc = out;
                pc += 3;
                DISPATCH();
            }
        }
    }
    [[clang::musttail]] return h_gen_LdaEnv(f, pc, acc);
}

Value h_gen_LdaEnv(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                const std::string* key = chunk.names[read_u16(code, pc)];
                const std::string& name = *key;
                pc += 2;
                // `this` is not in any environment; it is answered from the
                // frame, so it keeps the general path.
                if (name != "this") {
                    // One walk for both questions. The value comes from the
                    // first environment that binds the name; whether it can be
                    // read at all comes from the first DECLARATIVE one, which
                    // may be further out -- so an object environment's answer
                    // is held and the walk goes on.
                    Value object_value;
                    bool from_object = false;
                    int r = Environment::kNotBound;
                    for (Environment* e = ctx.get_lexical_environment(); e; e = e->get_outer()) {
                        r = e->env_read_step_interned(key, acc, &ctx);
                        CHECK_EXC();
                        if (r == Environment::kObjectValue) {
                            if (!from_object) { from_object = true; object_value = acc; }
                            r = Environment::kNotBound;
                            continue;
                        }
                        if (r != Environment::kNotBound) break;
                    }
                    if (r == Environment::kTdz) {
                        ctx.throw_reference_error("Cannot access '" + name + "' before initialization");
                    } else if (from_object) {
                        acc = object_value;
                    } else if (r == Environment::kNotBound) {
                        ctx.throw_reference_error("'" + name + "' is not defined");
                    }
                    CHECK_EXC();
                    break;
                }
                if (ctx.is_in_tdz(name)) {
                    ctx.throw_reference_error("Cannot access '" + name + "' before initialization");
                    CHECK_EXC();
                    break;
                }
                // `is_local` doesn't mean the scope is still active --
                // "not found" here is the same as never-declared.
                Environment* env = ctx.find_binding_env(name);
                if (env) {
                    acc = env->get_binding_direct(name, &ctx);
                } else {
                    ctx.throw_reference_error("'" + name + "' is not defined");
                }
                CHECK_EXC();
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_StaEnv(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                const std::string* key = chunk.names[read_u16(code, pc)];
                const std::string& name = *key;
                pc += 2;
                if (ctx.is_in_tdz_interned(key)) {
                    ctx.throw_reference_error("Cannot access '" + name + "' before initialization");
                    CHECK_EXC();
                    break;
                }
                Environment* env = ctx.find_binding_env_interned(key);
                if (env) {
                    if (!env->set_binding_direct_interned(key, acc, &ctx) &&
                        (ctx.is_strict_mode() || ctx.is_strict_const(name))) {
                        ctx.throw_type_error("Assignment to constant variable '" + name + "'");
                        CHECK_EXC();
                    }
                } else {
                    ctx.throw_reference_error("'" + name + "' is not defined");
                }
                CHECK_EXC();
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_StaEnvInit(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                const std::string* key = chunk.names[read_u16(code, pc)];
                pc += 2;
                ctx.get_lexical_environment()->initialize_binding_interned(key, acc);  // current environment, no chain walk
                break;
            }

            // Guarded direct-slot variants (see BytecodeCompiler.h's
            // EnvSlotInfo and Environment::inline_slot for why the
            // predicted slot needs re-validation). On a guard miss these
            // fall through to IDENTICAL code to LdaEnv/StaEnv/StaEnvInit --
            // a miss only costs the fast path, never correctness.
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_LdaEnvSlot(Frame& f, uint32_t pc, Value acc);

// Only the read that lands in the frame environment's own slot at the index
// the compiler assigned. A TDZ hole, a name that is not there and the chain
// walk behind it all tail-call the generated handler, so the common read
// stops paying its prologue.
Value h_LdaEnvSlotFast(Frame& f, uint32_t pc, Value acc) {
    const uint8_t* code = f.code;
    const uint8_t slot = code[pc + 1];
    const std::string* key = f.chunk.names[read_u16(code, pc + 2)];
    if (Environment* env = f.ctx.get_lexical_environment()) {
        if (auto* e = env->inline_slot_interned(slot, key)) {
            if (LIKELY(e->slot.initialized)) {
                acc = e->slot.value;
                pc += 4;
                DISPATCH();
            }
        }
    }
    [[clang::musttail]] return h_gen_LdaEnvSlot(f, pc, acc);
}

Value h_gen_LdaEnvSlot(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint8_t slot = code[pc];
                pc += 1;
                const std::string* key = chunk.names[read_u16(code, pc)];
                const std::string& name = *key;
                pc += 2;
                if (auto* e = ctx.get_lexical_environment()->inline_slot_interned(slot, key)) {
                    if (!e->slot.initialized) {
                        ctx.throw_reference_error("Cannot access '" + name + "' before initialization");
                        CHECK_EXC();
                        break;
                    }
                    acc = e->slot.value;
                    break;
                }
                if (ctx.is_in_tdz_interned(key)) {
                    ctx.throw_reference_error("Cannot access '" + name + "' before initialization");
                    CHECK_EXC();
                    break;
                }
                Environment* env = ctx.find_binding_env_interned(key);
                if (env) {
                    acc = env->get_binding_direct_interned(key, &ctx);
                } else {
                    ctx.throw_reference_error("'" + name + "' is not defined");
                }
                CHECK_EXC();
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_StaEnvSlot(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint8_t slot = code[pc];
                pc += 1;
                const std::string* key = chunk.names[read_u16(code, pc)];
                const std::string& name = *key;
                pc += 2;
                if (auto* e = ctx.get_lexical_environment()->inline_slot_interned(slot, key)) {
                    // The refusal used to be spelled as a silent skip, so a
                    // const write here vanished instead of raising.
                    if (!e->slot.mutable_flag) {
                        if (ctx.is_strict_mode() || ctx.is_strict_const(name)) {
                            ctx.throw_type_error("Assignment to constant variable '" + name + "'");
                            CHECK_EXC();
                        }
                        break;
                    }
                    Collector::write_barrier_env_for(ctx.get_lexical_environment(), acc);
                    e->slot.value = acc;
                    break;
                }
                if (ctx.is_in_tdz_interned(key)) {
                    ctx.throw_reference_error("Cannot access '" + name + "' before initialization");
                    CHECK_EXC();
                    break;
                }
                Environment* env = ctx.find_binding_env_interned(key);
                if (env) {
                    if (!env->set_binding_direct_interned(key, acc, &ctx) &&
                        (ctx.is_strict_mode() || ctx.is_strict_const(name))) {
                        ctx.throw_type_error("Assignment to constant variable '" + name + "'");
                        CHECK_EXC();
                    }
                } else {
                    ctx.throw_reference_error("'" + name + "' is not defined");
                }
                CHECK_EXC();
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_StaEnvSlotInit(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint8_t slot = code[pc];
                pc += 1;
                const std::string* key = chunk.names[read_u16(code, pc)];
                pc += 2;
                if (auto* e = ctx.get_lexical_environment()->inline_slot_interned(slot, key)) {
                    // Skipping the name lookup is the point of this path; the
                    // barrier initialize_binding runs below is not optional
                    // with it. An environment already traced by an open major
                    // is never revisited on its own, so a declaration storing
                    // a fresh cell here would leave it unmarked and swept
                    // while the binding still points at it.
                    Collector::write_barrier_env_for(ctx.get_lexical_environment(), acc);
                    e->slot.value = acc;
                    e->slot.initialized = true;
                    break;
                }
                ctx.get_lexical_environment()->initialize_binding_interned(key, acc);
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_BindEnvLocals(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                Environment* env = ctx.get_lexical_environment();
                if (chunk.env) for (const auto& loc : chunk.env->env_locals) {
                    if (loc.is_lexical) {
                        env->create_uninitialized_binding(loc.name, !loc.is_const);
                        // is_strict_const() wants the const SET, not just the cleared mutable
                        // flag, and every "Assignment to constant variable" check gates on
                        // it -- without this they are all inert in sloppy mode for a binding
                        // the VM created.
                        if (loc.is_const) env->mark_const_binding(loc.name);
                    }
                    else env->create_binding(loc.name, Value(), true);
                }
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

// The chunk's loop-env names, interned once. Unlike the parameter and local
// lists, this one is rebound on every iteration rather than once per call, so
// probing the intern pool per name here was the heaviest of the three.
const std::vector<const std::string*>& loop_env_keys_for(const BytecodeChunk& chunk, uint16_t idx) {
    auto& b = const_cast<BytecodeChunk::EnvBundle&>(*chunk.env);
    if (!b.loop_env_keys_ready) {
        b.loop_env_keys.resize(b.loop_envs.size());
        for (size_t li = 0; li < b.loop_envs.size(); li++) {
            auto& keys = b.loop_env_keys[li];
            keys.reserve(b.loop_envs[li].size());
            for (const auto& v : b.loop_envs[li]) keys.push_back(Shape::intern(v.name));
        }
        b.loop_env_keys_ready = true;
    }
    return b.loop_env_keys[idx];
}

Value h_gen_EnterLoopEnv(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint16_t idx = read_u16(code, pc);
                pc += 2;
                ctx.push_block_scope();
                Environment* env = ctx.get_lexical_environment();
                const auto& vars = chunk.env->loop_envs[idx];
                const auto& keys = loop_env_keys_for(chunk, idx);
                for (size_t i = 0; i < vars.size(); i++) {
                    const auto& v = vars[i];
                    if (v.is_lexical) {
                        env->create_uninitialized_binding_interned(keys[i], !v.is_const);
                        if (v.is_const) env->mark_const_binding(v.name);
                    }
                    else env->create_binding_interned(keys[i], Value(), true);
                }
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_AdvanceLoopEnv(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint16_t idx = read_u16(code, pc);
                pc += 2;
                const auto& vars = chunk.env->loop_envs[idx];
                const auto& keys = loop_env_keys_for(chunk, idx);
                std::vector<Value> carried(vars.size());
                Environment* old_env = ctx.get_lexical_environment();
                for (size_t i = 0; i < vars.size(); i++) {
                    if (vars[i].copy_forward) carried[i] = old_env->get_binding_direct_interned(keys[i], &ctx);
                }
                ctx.pop_block_scope();
                ctx.push_block_scope();
                Environment* new_env = ctx.get_lexical_environment();
                for (size_t i = 0; i < vars.size(); i++) {
                    const auto& v = vars[i];
                    if (v.is_lexical) {
                        new_env->create_uninitialized_binding_interned(keys[i], !v.is_const);
                        if (v.is_const) new_env->mark_const_binding(v.name);
                    }
                    else new_env->create_binding_interned(keys[i], Value(), true);
                    if (v.copy_forward) new_env->initialize_binding_interned(keys[i], carried[i]);
                }
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_ExitLoopEnv(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                ctx.pop_block_scope();
                break;
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_SaveEnv(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    Environment** env_saves = f.env_saves;
    uint8_t& env_save_top = f.env_save_top;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                env_saves[env_save_top++] = ctx.get_lexical_environment();
                break;
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_RestoreEnv(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    Environment** env_saves = f.env_saves;
    uint8_t& env_save_top = f.env_save_top;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                ctx.set_lexical_environment(env_saves[--env_save_top]);
                break;
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_PopEnvSave(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    uint8_t& env_save_top = f.env_save_top;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                env_save_top--;
                break;
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_GetIterator(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    Value* regs = f.regs;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint8_t next_fn_reg = code[pc];
                pc += 1;
                Value iterator, next_fn;
                if (!ForOfStatement::get_iterator(ctx, acc, iterator, next_fn)) {
                    CHECK_EXC();
                    break;
                }
                regs[next_fn_reg] = next_fn;
                acc = iterator;
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_IteratorNextOrJump(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    Value* regs = f.regs;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint8_t iter_reg = code[pc];
                uint8_t next_fn_reg = code[pc + 1];
                int16_t off = read_i16(code, pc + 2);
                pc += 4;
                bool done = false;
                Value value;
                if (!ForOfStatement::iterator_step(ctx, regs[iter_reg], regs[next_fn_reg], done, value)) {
                    CHECK_EXC();
                    break;
                }
                if (done) {
                    pc += off;
                } else {
                    acc = value;
                }
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_IteratorClose(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    Value* regs = f.regs;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint8_t iter_reg = code[pc];
                uint8_t mode = code[pc + 1];
                pc += 2;
                if (mode == 0) {
                    ForOfStatement::iterator_close(ctx, regs[iter_reg], /*validate_result=*/true,
                                                    /*is_pending=*/false, Value());
                } else {
                    Value pending = acc;
                    ForOfStatement::iterator_close(ctx, regs[iter_reg], /*validate_result=*/false,
                                                    /*is_pending=*/true, pending);
                }
                CHECK_EXC();
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_CreateForInKeys(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    Value* regs = f.regs;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint8_t obj_out = code[pc];
                pc += 1;
                // ToObject, same as the tree-walker's head evaluation: a string
                // enumerates through its wrapper. Null and undefined pass
                // through unboxed and produce no keys, which is their answer.
                if (!acc.is_object_like() && !acc.is_null() && !acc.is_undefined()) {
                    acc = ObjectFactory::box_primitive_this_sloppy(ctx, acc);
                    CHECK_EXC();
                }
                Object* obj = as_object_like(acc);
                // The loop re-asks this object whether a key is still there, so
                // it has to be the very object enumerated here rather than the
                // head's value: a receiver this converts stays converted.
                regs[obj_out] = obj ? Value(obj) : Value();
                Object* result = ObjectFactory::create_array(0).release();
                if (obj) {
                    std::vector<std::string> keys;
                    if (!ForInStatement::collect_keys(ctx, obj, keys)) {
                        CHECK_EXC();
                        break;
                    }
                    for (size_t i = 0; i < keys.size(); i++) {
                        result->set_element(static_cast<uint32_t>(i), Value(keys[i]));
                    }
                }
                acc = Value(result);
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_JumpIfNotNullish(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                int16_t off = read_i16(code, pc);
                pc += 2;
                if (!acc.is_null() && !acc.is_undefined()) pc += off;
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_JumpIfNullish(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                int16_t off = read_i16(code, pc);
                pc += 2;
                if (acc.is_null() || acc.is_undefined()) pc += off;
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_JumpIfNotUndefined(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                int16_t off = read_i16(code, pc);
                pc += 2;
                if (!acc.is_undefined()) pc += off;
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_CreateClosure(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint16_t idx = read_u16(code, pc);
                pc += 2;
                acc = instantiate_closure(ctx, (*chunk.closures)[idx]);
                CHECK_EXC();
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_DeclareFunction(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint16_t idx = read_u16(code, pc);
                pc += 2;
                declare_function(ctx, (*chunk.closures)[idx]);
                CHECK_EXC();
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_EvalAst(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint16_t idx = read_u16(code, pc);
                pc += 2;
                acc = const_cast<ASTNode*>((*chunk.treewalk_nodes)[idx])->evaluate(ctx);
                CHECK_EXC();
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_CopyRestProperties(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    Value* regs = f.regs;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint8_t src_reg = code[pc];
                uint8_t keys_reg = code[pc + 1];
                pc += 2;
                std::vector<std::string> taken;
                if (Object* keys = as_object_like(regs[keys_reg])) {
                    uint32_t n = static_cast<uint32_t>(keys->get_property("length").to_number());
                    for (uint32_t i = 0; i < n; i++) taken.push_back(keys->get_element(i).to_string());
                }
                acc = build_rest_object(ctx, regs[src_reg], as_object_like(regs[src_reg]), taken);
                CHECK_EXC();
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_Call(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    Value* regs = f.regs;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint8_t callee_reg = code[pc];
                uint8_t args_start = code[pc + 1];
                uint8_t argc = code[pc + 2];
                uint16_t name_idx = read_u16(code, pc + 3);
                pc += 5;
                const Value& callee = regs[callee_reg];
                std::span<const Value> call_args(regs + args_start, argc);
                if (callee.is_function()) {
                    acc = callee.as_function()->call_register_args(ctx, call_args, Value());
                } else if (callee.is_object() &&
                           callee.as_object()->get_type() == Object::ObjectType::Proxy) {
                    std::vector<Value> trap_args(call_args.begin(), call_args.end());
                    acc = static_cast<Proxy*>(callee.as_object())->apply_trap(trap_args, Value());
                } else {
                    ctx.throw_type_error(chunk.name_at(name_idx) + " is not a function");
                }
                CHECK_EXC();
                Collector::safepoint();
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_CallResolved(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    std::span<const Value> args = f.args;
    Value* regs = f.regs;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                // Callee already resolved+validated by GetNamed before args
                // were compiled (spec order); this just invokes it.
                uint8_t func_reg = code[pc];
                uint8_t this_reg = code[pc + 1];
                uint8_t args_start = code[pc + 2];
                uint8_t argc = code[pc + 3];
                uint16_t name_idx = read_u16(code, pc + 4);
                pc += 6;
                const Value& callee = regs[func_reg];
                const Value& receiver = regs[this_reg];
                std::span<const Value> call_args(regs + args_start, argc);
                if (callee.is_function()) {
                    acc = callee.as_function()->call_register_args(ctx, call_args, receiver);
                } else if (callee.is_object() &&
                           callee.as_object()->get_type() == Object::ObjectType::Proxy) {
                    std::vector<Value> trap_args(call_args.begin(), call_args.end());
                    acc = static_cast<Proxy*>(callee.as_object())->apply_trap(trap_args, receiver);
                } else {
                    ctx.throw_type_error(chunk.name_at(name_idx) + " is not a function");
                }
                CHECK_EXC();
                Collector::safepoint();
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_Construct(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    Value* regs = f.regs;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint8_t callee_reg = code[pc];
                uint8_t args_start = code[pc + 1];
                uint8_t argc = code[pc + 2];
                uint16_t name_idx = read_u16(code, pc + 3);
                pc += 5;
                const Value& callee = regs[callee_reg];
                std::span<const Value> call_args(regs + args_start, argc);
                if (callee.is_function()) {
                    // A literal `new X()` targets X regardless of any ambient
                    // new.target from an enclosing constructor call.
                    Value old_new_target = ctx.get_new_target();
                    ctx.set_new_target(callee);
                    acc = callee.as_function()->construct(ctx, call_args);
                    ctx.set_new_target(old_new_target);
                } else if (callee.is_object() &&
                           callee.as_object()->get_type() == Object::ObjectType::Proxy) {
                    acc = static_cast<Proxy*>(callee.as_object())->construct_trap(call_args);
                } else {
                    ctx.throw_type_error(chunk.name_at(name_idx) + " is not a constructor");
                }
                CHECK_EXC();
                Collector::safepoint();
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_CallSpread(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    Value* regs = f.regs;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint8_t func_reg = code[pc];
                uint8_t this_reg = code[pc + 1];
                uint8_t args_reg = code[pc + 2];
                uint16_t name_idx = read_u16(code, pc + 3);
                pc += 5;
                const Value& callee = regs[func_reg];
                // The operand is a spread SOURCE, not necessarily a
                // materialized argument array: a call whose whole argument
                // list is one spread (`f(...xs)`, the common shape) hands the
                // original iterable straight through, so nothing is
                // allocated. Mixed lists still arrive as a prebuilt Array,
                // which append_spread_values bulk-copies.
                std::vector<Value> call_args;
                ValueVectorRoot call_args_root(&call_args);
                append_spread_values(ctx, regs[args_reg], call_args);
                CHECK_EXC();
                if (callee.is_function()) {
                    acc = callee.as_function()->call(ctx, call_args, regs[this_reg]);
                } else if (callee.is_object() &&
                           callee.as_object()->get_type() == Object::ObjectType::Proxy) {
                    std::vector<Value> trap_args(call_args.begin(), call_args.end());
                    acc = static_cast<Proxy*>(callee.as_object())->apply_trap(trap_args, regs[this_reg]);
                } else {
                    ctx.throw_type_error(chunk.name_at(name_idx) + " is not a function");
                }
                CHECK_EXC();
                Collector::safepoint();
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_ConstructSpread(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    Value* regs = f.regs;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint8_t callee_reg = code[pc];
                uint8_t args_reg = code[pc + 1];
                uint16_t name_idx = read_u16(code, pc + 2);
                pc += 4;
                const Value& callee = regs[callee_reg];
                std::vector<Value> call_args;   // see CallSpread: a spread source, not always an array
                ValueVectorRoot call_args_root(&call_args);
                append_spread_values(ctx, regs[args_reg], call_args);
                CHECK_EXC();
                if (callee.is_function()) {
                    Value old_new_target = ctx.get_new_target();
                    ctx.set_new_target(callee);
                    acc = callee.as_function()->construct(ctx, call_args);
                    ctx.set_new_target(old_new_target);
                } else if (callee.is_object() &&
                           callee.as_object()->get_type() == Object::ObjectType::Proxy) {
                    acc = static_cast<Proxy*>(callee.as_object())->construct_trap(call_args);
                } else {
                    ctx.throw_type_error(chunk.name_at(name_idx) + " is not a constructor");
                }
                CHECK_EXC();
                Collector::safepoint();
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_CreateRegExp(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint16_t pat_idx = read_u16(code, pc);
                uint16_t flg_idx = read_u16(code, pc + 2);
                pc += 4;
                acc = create_regexp_literal(ctx, chunk.name_at(pat_idx), chunk.name_at(flg_idx));
                CHECK_EXC();
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_LdaEngineHelper(Frame& f, uint32_t pc, Value acc) {
    Context& ctx = f.ctx;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint8_t kind = code[pc];
                pc += 1;
                Object* global = ctx.get_global_object();
                acc = global ? global->get_internal_slot(
                          EngineHelper::slot_name(static_cast<EngineHelper::Kind>(kind)))
                             : Value();
                break;
            }
    } while (0);
    DISPATCH();
}

Value h_gen_HasPrivate(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint16_t name_idx = read_u16(code, pc);
                pc += 2;
                acc = private_name_in(ctx, chunk.name_at(name_idx), acc);
                CHECK_EXC();
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_GetSuper(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint16_t name_idx = read_u16(code, pc);
                pc += 2;
                acc = super_get(ctx, chunk.name_at(name_idx));
                CHECK_EXC();
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_SetSuper(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    Value* regs = f.regs;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint8_t base_reg = code[pc];
                uint16_t name_idx = read_u16(code, pc + 1);
                pc += 3;
                super_set_on(ctx, as_object_like(regs[base_reg]), chunk.name_at(name_idx), acc);
                CHECK_EXC();
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_ResolveSuperBase(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    Value* regs = f.regs;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint8_t dst = code[pc];
                pc += 1;
                // The this-TDZ check belongs here rather than in the keyed opcodes:
                // GetThisBinding precedes the key expression, whose side effects must
                // not run first (13.3.7.1 step 2).
                if (ctx.this_needs_super()) {
                    ctx.throw_reference_error("Must call super constructor before accessing 'this' in derived class constructor");
                    CHECK_EXC();
                    break;
                }
                Object* base = resolve_super_base(ctx);
                CHECK_EXC();
                regs[dst] = base ? Value(base) : Value();
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_GetSuperKeyed(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    Value* regs = f.regs;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint8_t base_reg = code[pc];
                pc += 1;
                std::string key = acc.to_property_key();
                CHECK_EXC();
                acc = super_get_on(ctx, as_object_like(regs[base_reg]), key);
                CHECK_EXC();
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_SetSuperKeyed(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    Value* regs = f.regs;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint8_t base_reg = code[pc];
                uint8_t key_reg = code[pc + 1];
                pc += 2;
                std::string key = regs[key_reg].to_property_key();
                CHECK_EXC();
                super_set_on(ctx, as_object_like(regs[base_reg]), key, acc);
                CHECK_EXC();
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_SuperCall(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    Value* regs = f.regs;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint8_t args_start = code[pc];
                uint8_t argc = code[pc + 1];
                pc += 2;
                std::vector<Value> call_args(regs + args_start, regs + args_start + argc);
                // The arguments already ran, so sampling here is what the
                // tree-walker gets by OR-ing its before/after samples.
                acc = perform_super_call(ctx, call_args, ctx.was_super_called());
                CHECK_EXC();
                Collector::safepoint();
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_ObjectSpreadInto(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    Value* regs = f.regs;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint8_t obj_reg = code[pc];
                pc += 1;
                if (Object* target = as_object_like(regs[obj_reg])) {
                    object_spread_into(ctx, target, acc);
                }
                CHECK_EXC();
                }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_SpreadInto(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    Value* regs = f.regs;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint8_t arr_reg = code[pc];
                uint8_t idx_reg = code[pc + 1];
                pc += 2;
                std::vector<Value> expanded;
                ValueVectorRoot expanded_root(&expanded);
                append_spread_values(ctx, acc, expanded);
                CHECK_EXC();
                if (Object* target = as_object_like(regs[arr_reg])) {
                    uint32_t idx = static_cast<uint32_t>(regs[idx_reg].to_number());
                    for (const Value& v : expanded) target->set_element(idx++, v);
                    regs[idx_reg] = Value(static_cast<double>(idx));
                }
                CHECK_EXC();
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_GetNamed(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    Function* owner = f.owner;
    Value* regs = f.regs;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint8_t obj_reg = code[pc];
                uint16_t name_idx = read_u16(code, pc + 1);
                uint16_t fb_idx = read_u16(code, pc + 3);
                pc += 5;
                acc = get_named(ctx, regs[obj_reg], chunk.name_at(name_idx), &chunk.feedback[fb_idx], owner, f.feedback_rooted);
                CHECK_EXC();
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

// A monomorphic own-property read is a shape compare and a slot load, and it
// cannot throw. get_named reaches that in a handful of instructions but the
// generated handler pays its prologue first, including the instr_pc store only
// a throw would ever need. The first feedback entry is the whole fast path
// here: a miss, a polymorphic site or anything that is not a plain object
// tail-calls the generated handler, which rescans from scratch.
Value h_GetNamedRest(Frame& f, uint32_t pc, Value acc);

// Only the read that answers from the receiver's own shape slot, which is
// seven in ten of them. Everything else -- an Array's length, an inherited
// property, a primitive receiver, a miss -- tail-calls the handler below, so
// the common read stops paying that handler's prologue: it saves five
// callee-saved registers and a frame, for paths it does not take.
Value h_GetNamedFast(Frame& f, uint32_t pc, Value acc) {
    const uint8_t* code = f.code;
    const Value& receiver = f.regs[code[pc + 1]];
    if (LIKELY(receiver.is_object())) {
        Object* obj = receiver.as_object();
        if (LIKELY(obj->get_type() == Object::ObjectType::Ordinary)) {
            const FeedbackSlot& fb = f.chunk.feedback[read_u16(code, pc + 4)];
            const FeedbackSlot::Entry& e = fb.entries[0];
            // A non-null shape here means the site is neither empty nor
            // megamorphic: going mega clears it (see learn_feedback).
            if (LIKELY(e.shape && !e.is_accessor &&
                       e.shape == obj->get_shape() &&
                       !obj->has_any_descriptor_override())) {
                if (const Value* slot = obj->get_shape_slot_unchecked(e.slot_index)) {
                    acc = *slot;
                    pc += 6;
                    DISPATCH();
                }
            }
        }
    }
    [[clang::musttail]] return h_GetNamedRest(f, pc, acc);
}

Value h_GetNamedRest(Frame& f, uint32_t pc, Value acc) {
    const uint8_t* code = f.code;
    const Value& receiver = f.regs[code[pc + 1]];
    // An own property that lives in the receiver's descriptor map -- every
    // namespace builtin, where the shape-slot entry above can never hit
    // because a non-enumerable property has no slot to be in. The receiver's
    // own identity is the key and the descriptor epoch retires the entry, so
    // the whole test is two compares against a site that has learned one.
    // It sits ahead of the is_object() gate because a constructor namespace
    // is a function object, and those never reach that gate at all.
    {
        const FeedbackSlot& fb = f.chunk.feedback[read_u16(code, pc + 4)];
        if (fb.own_desc_receiver != nullptr &&
            fb.own_desc_epoch == Object::descriptor_epoch() &&
            as_object_like(receiver) == fb.own_desc_receiver) {
            acc = fb.own_desc_value;
            pc += 6;
            DISPATCH();
        }
    }
    if (LIKELY(receiver.is_object())) {
        Object* obj = receiver.as_object();
        if (obj->get_type() == Object::ObjectType::Array) {
            // A dense Array's length is its element count, and `length` can
            // never become an accessor on one, so the count answers the read
            // exactly. A length moved into a descriptor fails the dense check
            // and takes the general path.
            const FeedbackSlot& afb = f.chunk.feedback[read_u16(code, pc + 4)];
            if (LIKELY(afb.array_length && obj->has_only_dense_elements())) {
                acc = Value(static_cast<double>(obj->element_count()));
                pc += 6;
                DISPATCH();
            }
        } else if (LIKELY(obj->get_type() == Object::ObjectType::Ordinary)) {
            const FeedbackSlot& fb = f.chunk.feedback[read_u16(code, pc + 4)];
            // Every learned entry, not just the first. The handler above tries
            // that one and hands the rest here, so a site that has seen more
            // than one shape -- three objects taking turns is enough -- reached
            // the generated handler on every read but the one whose shape
            // happened to be learned first, and rescanned from scratch to find
            // an entry that was sitting right here.
            //
            // The stamp and the receiver's own map answer the same question
            // from different sides: the stamp says no object had an override
            // when the entry was learned, the map says this object has none
            // now. The stamp is global, so a single defineProperty anywhere
            // retires every entry and nothing revives them -- which is what
            // sent nearly every read here down the general path. The map is
            // per-receiver and re-establishes itself on every read, and it is
            // the one that can be trusted from a single object.
            Shape* own_shape = obj->get_shape();
            for (uint8_t k = 0; k < fb.count; k++) {
                const FeedbackSlot::Entry& cand = fb.entries[k];
                if (cand.shape != own_shape || cand.is_accessor) continue;
                if (obj->has_any_descriptor_override()) break;
                if (const Value* slot = obj->get_shape_slot_unchecked(cand.slot_index)) {
                    acc = *slot;
                    pc += 6;
                    DISPATCH();
                }
                break;
            }
            // Inherited-property fast path -- the same entry scan get_named
            // does, hoisted here. Reading a method off a prototype is the
            // single commonest read this handler could not serve, and each
            // one paid the generated handler's prologue and a rescan from
            // scratch. The own-descriptor guard is the whole-object form
            // rather than the keyed one: strictly stronger, and O(1).
            if (f.feedback_rooted && !fb.proto_mega && fb.proto_count > 0 &&
                !obj->has_any_descriptor_override()) {
                Shape* rs = obj->get_shape();
                Object* p0 = obj->get_prototype();
                uint64_t pep = Object::proto_epoch();
                for (uint8_t k = 0; k < fb.proto_count; k++) {
                    const FeedbackSlot::ProtoEntry& pe = fb.proto_entries[k];
                    if (pe.receiver_shape == rs && pe.prototype == p0 && pe.proto_epoch == pep) {
                        // An absence is a value like any other here: undefined.
                        // A getter entry is not served from this handler -- the
                        // call it needs can throw, which is what the generated
                        // handler's epilogue is for.
                        if (pe.absent) {
                            acc = Value();
                            pc += 6;
                            DISPATCH();
                        }
                        if (pe.is_getter) break;
                        if (pe.from_descriptor) {
                            if (pe.desc_epoch == Object::descriptor_epoch()) {
                                acc = pe.cached_value;
                                pc += 6;
                                DISPATCH();
                            }
                        } else if (const Value* hs = pe.holder->get_shape_slot_unchecked(pe.slot_index)) {
                            acc = *hs;
                            pc += 6;
                            DISPATCH();
                        }
                        break;
                    }
                }
            }
        }
    } else if (!receiver.is_function()) {
        // Primitive receiver. get_primitive_named already caches the resolved
        // value per site (prim_*), but only after get_named's entry checks, its
        // own `length`/index early-outs and the kind dispatch have all run --
        // so a `s.charCodeAt` that the cache can answer outright still pays the
        // generated handler's prologue and two calls to get there. This is the
        // identical predicate, asked where the read starts.
        //
        // The two string early-outs get_primitive_named runs first (`length`
        // and a canonical index) are not skipped, they are unreachable: a
        // GetNamed site's name is a compile-time constant, and either of those
        // returns before the cache is ever written, so a site whose name is one
        // of them can only carry a prim entry learned from a DIFFERENT kind --
        // whose prototype then fails the identity check below.
        const FeedbackSlot& fb = f.chunk.feedback[read_u16(code, pc + 4)];
        if (LIKELY(fb.prim_valid && !fb.prim_is_getter &&
                   fb.prim_desc_epoch == Object::descriptor_epoch())) {
            using PK = Context::PrimitiveKind;
            PK kind = receiver.is_string()  ? PK::String
                    : receiver.is_number()  ? PK::Number
                    : receiver.is_boolean() ? PK::Boolean
                    : receiver.is_bigint()  ? PK::BigInt
                    : receiver.is_symbol()  ? PK::Symbol
                                            : PK::Count;
            if (LIKELY(kind != PK::Count && Context::primitive_prototype(kind) == fb.prim_proto)) {
                acc = fb.prim_value;
                pc += 6;
                DISPATCH();
            }
        }
    }
    [[clang::musttail]] return h_gen_GetNamed(f, pc, acc);
}

Value h_gen_SetNamed(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    Function* owner = f.owner;
    Value* regs = f.regs;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint8_t obj_reg = code[pc];
                uint16_t name_idx = read_u16(code, pc + 1);
                uint16_t fb_idx = read_u16(code, pc + 3);
                pc += 5;
                set_named(ctx, regs[obj_reg], chunk.name_at(name_idx), acc, &chunk.feedback[fb_idx], owner);
                CHECK_EXC();
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

// The write counterpart of h_GetNamedFast, with the same reasoning: a
// monomorphic own-property store is a shape compare and a slot write, and it
// cannot throw. The barrier still runs, since the store is what the remembered
// set needs to hear about.
Value h_SetNamedRest(Frame& f, uint32_t pc, Value acc);

Value h_SetNamedFast(Frame& f, uint32_t pc, Value acc) {
    const uint8_t* code = f.code;
    const Value& receiver = f.regs[code[pc + 1]];
    if (LIKELY(receiver.is_object())) {
        Object* obj = receiver.as_object();
        if (LIKELY(obj->get_type() == Object::ObjectType::Ordinary)) {
            const FeedbackSlot& fb = f.chunk.feedback[read_u16(code, pc + 4)];
            const FeedbackSlot::Entry& e = fb.entries[0];
            // Same pair of facts as the read side: the stamp is global and
            // never revived, the receiver's own map answers for itself.
            // A non-null shape here means the site is neither empty nor
            // megamorphic: going mega clears it (see learn_feedback).
            if (LIKELY(e.shape && !e.is_accessor &&
                       e.shape == obj->get_shape() &&
                       !obj->has_any_descriptor_override())) {
                if (Value* slot = obj->get_shape_slot_unchecked(e.slot_index)) {
                    write_barrier_for(obj, acc);
                    *slot = acc;
                    pc += 6;
                    DISPATCH();
                }
            }
        }
    }
    [[clang::musttail]] return h_SetNamedRest(f, pc, acc);
}

// The entries the handler above did not try. Split for the reason the read
// side is split: a monomorphic write is the overwhelming case and must not
// walk a loop to find the one entry it always hits, while a site that has
// seen more than one shape should not reach the general handler and rescan
// from scratch for an entry that is sitting right here.
Value h_SetNamedRest(Frame& f, uint32_t pc, Value acc) {
    const uint8_t* code = f.code;
    const Value& receiver = f.regs[code[pc + 1]];
    if (LIKELY(receiver.is_object())) {
        Object* obj = receiver.as_object();
        if (LIKELY(obj->get_type() == Object::ObjectType::Ordinary)) {
            const FeedbackSlot& fb = f.chunk.feedback[read_u16(code, pc + 4)];
            Shape* own_shape = obj->get_shape();
            for (uint8_t k = 1; k < fb.count; k++) {
                const FeedbackSlot::Entry& e = fb.entries[k];
                if (e.shape != own_shape || e.is_accessor) continue;
                if (obj->has_any_descriptor_override()) break;
                if (Value* slot = obj->get_shape_slot_unchecked(e.slot_index)) {
                    write_barrier_for(obj, acc);
                    *slot = acc;
                    pc += 6;
                    DISPATCH();
                }
                break;
            }
        }
    }
    [[clang::musttail]] return h_gen_SetNamed(f, pc, acc);
}

Value h_gen_GetKeyed(Frame& f, uint32_t pc, Value acc);

// Indexing a dense array is a bounds check and a load. Everything else, the
// null receiver included, goes to the generated handler, which does the whole
// spec-ordered sequence from the start.
Value h_GetKeyedFast(Frame& f, uint32_t pc, Value acc) {
    uint32_t index;
    Object* dense;
    TypedArrayBase* typed;
    if (LIKELY(array_index_key(acc, index))) {
        const Value& recv = f.regs[f.code[pc + 1]];
        if (LIKELY(dense_element_slot(recv, index, dense))) {
            acc = dense->get_element_unchecked(index);
            pc += 4;
            DISPATCH();
        }
        if (typed_element_slot(recv, index, typed)) {
            acc = typed->get_element(index);
            pc += 4;
            DISPATCH();
        }
    }
    [[clang::musttail]] return h_gen_GetKeyed(f, pc, acc);
}

Value h_gen_GetPrivate(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    Value* regs = f.regs;
    PrivateFeedback* private_feedback_data = f.private_feedback_data;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint8_t obj_reg = code[pc];
                uint16_t name_idx = read_u16(code, pc + 1);
                uint16_t fb_idx = read_u16(code, pc + 3);
                pc += 5;
                acc = get_private(ctx, regs[obj_reg], chunk.name_at(name_idx), &private_feedback_data[fb_idx]);
                CHECK_EXC();
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_SetPrivate(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    Value* regs = f.regs;
    PrivateFeedback* private_feedback_data = f.private_feedback_data;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint8_t obj_reg = code[pc];
                uint16_t name_idx = read_u16(code, pc + 1);
                uint16_t fb_idx = read_u16(code, pc + 3);
                pc += 5;
                set_private(ctx, regs[obj_reg], chunk.name_at(name_idx), acc, &private_feedback_data[fb_idx]);
                CHECK_EXC();
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_GetKeyed(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    Value* regs = f.regs;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint8_t obj_reg = code[pc];
                uint16_t fb_idx = read_u16(code, pc + 1);
                pc += 3;
                const Value& recv = regs[obj_reg];
                // Null/undefined check must run before ToPropertyKey on the key (spec order).
                if (recv.is_null() || recv.is_undefined()) {
                    ctx.throw_type_error("Cannot read property of null or undefined");
                    CHECK_EXC();
                    break;
                }
                uint32_t index;
                Object* dense;
                TypedArrayBase* typed;
                if (array_index_key(acc, index)) {
                    if (dense_element_slot(recv, index, dense)) {
                        acc = dense->get_element_unchecked(index);
                        break;
                    }
                    if (typed_element_slot(recv, index, typed)) {
                        acc = typed->get_element(index);
                        break;
                    }
                }
                std::string key = acc.to_property_key();
                CHECK_EXC();
                acc = get_keyed(ctx, recv, key, &chunk.ic_feedback->keyed_feedback[fb_idx]);
                CHECK_EXC();
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_SetKeyed(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    Value* regs = f.regs;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint8_t obj_reg = code[pc];
                uint8_t key_reg = code[pc + 1];
                uint16_t fb_idx = read_u16(code, pc + 2);
                pc += 4;
                const Value& recv = regs[obj_reg];
                if (recv.is_null() || recv.is_undefined()) {
                    ctx.throw_type_error(std::string("Cannot set properties of ") +
                        (recv.is_null() ? "null" : "undefined"));
                    CHECK_EXC();
                    break;
                }
                uint32_t index;
                Object* dense;
                TypedArrayBase* typed;
                if (array_index_key(regs[key_reg], index)) {
                    if (dense_element_store_slot(recv, index, dense) &&
                        dense->store_dense_element(index, acc)) {
                        break;
                    }
                    if (typed_element_slot(recv, index, typed)) {
                        typed->set_element(index, acc);
                        break;
                    }
                }
                std::string key = regs[key_reg].to_property_key();
                CHECK_EXC();
                set_keyed(ctx, recv, key, acc, &chunk.ic_feedback->keyed_feedback[fb_idx]);
                CHECK_EXC();
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_DeleteNamed(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    Value* regs = f.regs;
    const uint8_t* code = f.code;
    const Op op = Op::DeleteNamed;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint8_t obj_reg = code[pc];
                std::string property_name;
                if (op == Op::DeleteNamed) {
                    property_name = chunk.name_at(read_u16(code, pc + 1));
                    pc += 3;
                } else {
                    pc += 1;
                }
                const Value& recv = regs[obj_reg];
                Object* obj = recv.is_object() ? recv.as_object()
                            : recv.is_function() ? static_cast<Object*>(recv.as_function())
                            : nullptr;
                if (!obj) {
                    // null/undefined: ToObject throws; other primitives wrap into a
                    // temporary whose delete trivially succeeds.
                    if (recv.is_null() || recv.is_undefined()) {
                        ctx.throw_type_error("Cannot convert undefined or null to object");
                        CHECK_EXC();
                        break;
                    }
                    if (op == Op::DeleteKeyed) {
                        (void)acc.to_property_key();  // ToPropertyKey may still throw
                        CHECK_EXC();
                    }
                    acc = Value(true);
                    break;
                }
                if (op == Op::DeleteKeyed) {
                    property_name = acc.to_property_key();
                    CHECK_EXC();
                }
                bool deleted;
                if (obj->get_type() == Object::ObjectType::Proxy) {
                    deleted = static_cast<Proxy*>(obj)->delete_trap(Value(property_name));
                } else {
                    deleted = obj->delete_property(property_name);
                }
                CHECK_EXC();
                if (!deleted && ctx.is_strict_mode()) {
                    ctx.throw_type_error("Cannot delete property '" + property_name + "'");
                    CHECK_EXC();
                    break;
                }
                acc = Value(deleted);
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_DeleteKeyed(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    Value* regs = f.regs;
    const uint8_t* code = f.code;
    const Op op = Op::DeleteKeyed;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint8_t obj_reg = code[pc];
                std::string property_name;
                if (op == Op::DeleteNamed) {
                    property_name = chunk.name_at(read_u16(code, pc + 1));
                    pc += 3;
                } else {
                    pc += 1;
                }
                const Value& recv = regs[obj_reg];
                Object* obj = recv.is_object() ? recv.as_object()
                            : recv.is_function() ? static_cast<Object*>(recv.as_function())
                            : nullptr;
                if (!obj) {
                    // null/undefined: ToObject throws; other primitives wrap into a
                    // temporary whose delete trivially succeeds.
                    if (recv.is_null() || recv.is_undefined()) {
                        ctx.throw_type_error("Cannot convert undefined or null to object");
                        CHECK_EXC();
                        break;
                    }
                    if (op == Op::DeleteKeyed) {
                        (void)acc.to_property_key();  // ToPropertyKey may still throw
                        CHECK_EXC();
                    }
                    acc = Value(true);
                    break;
                }
                if (op == Op::DeleteKeyed) {
                    property_name = acc.to_property_key();
                    CHECK_EXC();
                }
                bool deleted;
                if (obj->get_type() == Object::ObjectType::Proxy) {
                    deleted = static_cast<Proxy*>(obj)->delete_trap(Value(property_name));
                } else {
                    deleted = obj->delete_property(property_name);
                }
                CHECK_EXC();
                if (!deleted && ctx.is_strict_mode()) {
                    ctx.throw_type_error("Cannot delete property '" + property_name + "'");
                    CHECK_EXC();
                    break;
                }
                acc = Value(deleted);
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_DefineOwn(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    Value* regs = f.regs;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint8_t obj_reg = code[pc];
                uint16_t name_idx = read_u16(code, pc + 1);
                uint16_t fb_idx = read_u16(code, pc + 3);
                pc += 5;
                Object* obj = as_object_like(regs[obj_reg]);
                define_own_cached(obj, chunk.name_at(name_idx), acc, &chunk.feedback[fb_idx]);
                CHECK_EXC();
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_DefineElement(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    Value* regs = f.regs;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint8_t obj_reg = code[pc];
                uint8_t key_reg = code[pc + 1];
                pc += 2;
                Object* obj = as_object_like(regs[obj_reg]);
                if (obj) obj->set_element(static_cast<uint32_t>(regs[key_reg].to_number()), acc);
                CHECK_EXC();
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_ToPropertyKeyStrict(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                if (!acc.is_string()) {
                    acc = Value(acc.to_property_key_strict(ctx));
                    CHECK_EXC();
                }
                break;
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_DefineOwnKeyed(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    Value* regs = f.regs;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint8_t obj_reg = code[pc];
                uint8_t key_reg = code[pc + 1];
                pc += 2;
                Object* obj = as_object_like(regs[obj_reg]);
                if (obj) {
                    std::string key = regs[key_reg].to_property_key();
                    CHECK_EXC();
                    if (key == "__proto__") {
                        // Computed __proto__ is a plain data property, never
                        // [[Prototype]] (Annex B.3.1 only special-cases the
                        // non-computed literal form) -- set_property() would
                        // otherwise find Object.prototype's own __proto__
                        // ACCESSOR via its inherited-setter walk and wrongly
                        // invoke it instead of creating an own property.
                        obj->set_property_descriptor(key, PropertyDescriptor(acc, PropertyAttributes::Default));
                    } else {
                        obj->set_property(key, acc);
                    }
                }
                CHECK_EXC();
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_FinalizeStaticProperty(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    Value* regs = f.regs;
    const uint8_t* code = f.code;
    const Op op = Op::FinalizeStaticProperty;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint8_t obj_reg = code[pc];
                uint16_t key_name_idx = read_u16(code, pc + 1);
                uint16_t display_name_idx = read_u16(code, pc + 3);
                uint8_t raw_kind = code[pc + 5];
                // Bit 0x4: the compiler proved this method's body never
                // references `super`, so the [[HomeObject]] write below
                // (needed only for super resolution, see member.cpp) was
                // skipped entirely -- see BytecodeCompiler.cpp's
                // method_references_super. Getter/Setter never set this bit.
                uint8_t kind = raw_kind & 0x3;
                bool super_free = (raw_kind & 0x4) != 0;
                uint16_t fb_idx = read_u16(code, pc + 6);
                pc += 8;
                Object* obj = as_object_like(regs[obj_reg]);
                if (acc.is_function()) {
                    Function* fn = acc.as_function();
                    // Only rename if currently unnamed -- method/getter/setter
                    // shorthand syntax can't produce a pre-named function, but
                    // mirror literals.cpp's own guard exactly rather than
                    // assume that.
                    if (fn->get_name().empty() || fn->get_name() == "<arrow>") {
                        fn->set_name(chunk.name_at(display_name_idx));
                    }
                    const std::string& key = chunk.name_at(key_name_idx);
                    if (kind == 0) {
                        // Method: spec 14.3.9 -- non-generator methods are not
                        // constructors and have no .prototype.
                        if (!super_free && obj) fn->set_home_object(obj);
                        if (fn->is_constructor()) {
                            fn->set_is_constructor(false);
                            fn->set_function_prototype(nullptr);
                        }
                        if (obj) define_own_cached(obj, key, acc, &chunk.feedback[fb_idx]);
                    } else {
                        // Getter (1) / Setter (2): spec 14.4.13/14.4.14 --
                        // GetterMethod/SetterMethod never had a .prototype to
                        // begin with (create_prototype=false at creation, same
                        // as a shorthand Method), so is_constructor() is
                        // already false here; skip the strip entirely instead
                        // of paying set_function_prototype's real (if no-op)
                        // descriptor-erase + Shape::find_slot + a linear scan
                        // over property_insertion_order_ on every getter/
                        // setter. define_accessor_cached handles the fetch-
                        // existing-descriptor-and-merge case internally, so a
                        // getter+setter pair sharing a key still installs
                        // correctly regardless of what else runs between them.
                        if (fn->is_constructor()) fn->set_function_prototype(nullptr);
                        if (obj) define_accessor_cached(obj, key, fn, kind == 1, &chunk.feedback[fb_idx]);
                    }
                }
                CHECK_EXC();
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_FinalizeComputedProperty(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    Value* regs = f.regs;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint8_t obj_reg = code[pc];
                uint8_t key_reg = code[pc + 1];
                uint8_t raw_key_reg = code[pc + 2];
                uint8_t raw_kind = code[pc + 3];
                // See FinalizeStaticProperty's identical bit-0x4 comment.
                uint8_t kind = raw_kind & 0x3;
                bool super_free = (raw_kind & 0x4) != 0;
                pc += 4;
                Object* obj = as_object_like(regs[obj_reg]);
                if (kind != 0 && acc.is_function()) {
                    // ValueWithName (1) or Method (2): NamedEvaluation, computed
                    // at runtime since the key isn't known until now -- mirrors
                    // literals.cpp's is_symbol()-aware "[desc]" formatting.
                    Function* fn = acc.as_function();
                    // Only rename if currently unnamed (e.g. `{[k]: function named(){}}`
                    // keeps "named", matching literals.cpp's own guard).
                    if (fn->get_name().empty() || fn->get_name() == "<arrow>") {
                        const Value& raw_key = regs[raw_key_reg];
                        std::string func_name;
                        if (raw_key.is_symbol()) {
                            std::string desc = raw_key.as_symbol()->get_description();
                            func_name = desc.empty() ? "" : "[" + desc + "]";
                        } else {
                            func_name = regs[key_reg].to_property_key();
                            CHECK_EXC();
                        }
                        fn->set_name(func_name);
                    }
                }
                if (kind == 2 && acc.is_function()) {
                    // Method finalize (spec 14.3.9), same as FinalizeStaticProperty.
                    Function* fn = acc.as_function();
                    if (!super_free && obj) fn->set_home_object(obj);
                    if (fn->is_constructor()) {
                        fn->set_is_constructor(false);
                        fn->set_function_prototype(nullptr);
                    }
                }
                if (obj) {
                    std::string key = regs[key_reg].to_property_key();
                    CHECK_EXC();
                    if (key == "__proto__") {
                        // Same fix as DefineOwnKeyed: computed __proto__ is a
                        // plain data property, never [[Prototype]].
                        obj->set_property_descriptor(key, PropertyDescriptor(acc, PropertyAttributes::Default));
                    } else {
                        obj->create_own_data_property(key, acc);
                    }
                }
                CHECK_EXC();
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_SetFunctionNameIfUnnamed(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint16_t name_idx = read_u16(code, pc);
                pc += 2;
                if (acc.is_function()) {
                    Function* fn = acc.as_function();
                    if (fn->get_name().empty() || fn->get_name() == "<arrow>") {
                        fn->set_name(chunk.name_at(name_idx));
                    }
                }
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_CreateObject(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                pc += 2;
                const uint16_t slot_hint = read_u16(code, pc - 2);
                // The literal says how many properties are coming, so the cell
                // is asked for room to hold them and the object never reaches
                // for a butterfly block of its own.
                Object* obj;
                if (slot_hint <= 4) {
                    obj = ObjectFactory::create_object_with_slots(4).release();
                } else {
                    obj = ObjectFactory::create_object().release();
                    obj->reserve_property_slots(slot_hint);
                }
                acc = Value(obj);
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_CreateArray(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint16_t n = read_u16(code, pc);
                pc += 2;
                auto arr = ObjectFactory::create_array(0);
                if (n) arr->set_length(n);  // trailing holes count toward length
                acc = Value(arr.release());
                break;
            }
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_CreateRestArray(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    std::span<const Value> args = f.args;
    const uint8_t* code = f.code;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                {
                uint8_t start_index = code[pc];
                pc += 1;
                auto rest_array = ObjectFactory::create_array(0);
                for (size_t j = start_index; j < args.size(); j++) {
                    rest_array->push(args[j]);
                }
                acc = Value(rest_array.release());
                break;
            }

            // Backward jumps are loop back-edges -- the VM's equivalent of the
            // tree-walker's once-per-statement Collector::safepoint() hook.
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_Throw(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                ctx.throw_exception(acc, /*raw=*/true);
                CHECK_EXC();
                break;
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

Value h_gen_ReraiseGeneratorReturn(Frame& f, uint32_t pc, Value acc) {
    const BytecodeChunk& chunk = f.chunk;
    Context& ctx = f.ctx;
    uint32_t& instr_pc = f.instr_pc;
    instr_pc = pc;
    pc += 1;
    do {
                throw GeneratorReturnException(acc);
    } while (0);
    CHECK_EXC_TAIL();
    DISPATCH();
}

#undef CHECK_EXC_TAIL
#undef BINARY_OP
#undef CHECK_EXC
#undef DISPATCH

// The table is 256 wide and the opcode enum is not, so a byte that is not an
// opcode at all needs somewhere to land.
Value h_invalid(Frame& f, uint32_t pc, Value acc) {
    (void)pc;
    (void)acc;
    f.ctx.throw_exception(Value(std::string("VM: invalid opcode")));
    return Value();
}

constexpr std::array<Handler, 256> make_handler_table() {
    std::array<Handler, 256> t{};
    for (auto& e : t) e = &h_invalid;
    t[static_cast<uint8_t>(Op::Ldar)]          = &h_Ldar;
    t[static_cast<uint8_t>(Op::Star)]          = &h_Star;
    t[static_cast<uint8_t>(Op::Mov)]           = &h_Mov;
    t[static_cast<uint8_t>(Op::LdarStar)]      = &h_LdarStar;
    t[static_cast<uint8_t>(Op::LdaSmiStar)]    = &h_LdaSmiStar;
    t[static_cast<uint8_t>(Op::LdaZeroStar)]   = &h_LdaZeroStar;
    t[static_cast<uint8_t>(Op::LdaConstStar)]  = &h_LdaConstStar;
    t[static_cast<uint8_t>(Op::LdaZero)]       = &h_LdaZero;
    t[static_cast<uint8_t>(Op::LdaUndefined)]  = &h_LdaUndefined;
    t[static_cast<uint8_t>(Op::LdaNull)]       = &h_LdaNull;
    t[static_cast<uint8_t>(Op::LdaTrue)]       = &h_LdaTrue;
    t[static_cast<uint8_t>(Op::LdaFalse)]      = &h_LdaFalse;
    t[static_cast<uint8_t>(Op::LdaSmi)]        = &h_LdaSmi;
    t[static_cast<uint8_t>(Op::LdaConst)]      = &h_LdaConst;
    t[static_cast<uint8_t>(Op::Return)]        = &h_Return;
    t[static_cast<uint8_t>(Op::Jump)]          = &h_Jump;
    t[static_cast<uint8_t>(Op::JumpIfFalse)]   = &h_JumpIfFalse;
    t[static_cast<uint8_t>(Op::JumpIfTrue)]    = &h_JumpIfTrue;
    t[static_cast<uint8_t>(Op::Add)]           = &h_Add;
    t[static_cast<uint8_t>(Op::Sub)]           = &h_Sub;
    t[static_cast<uint8_t>(Op::Mul)]           = &h_Mul;
    t[static_cast<uint8_t>(Op::TestLt)]        = &h_TestLt;
    t[static_cast<uint8_t>(Op::TestGt)]        = &h_TestGt;
    t[static_cast<uint8_t>(Op::TestLe)]        = &h_TestLe;
    t[static_cast<uint8_t>(Op::TestGe)]        = &h_TestGe;
    t[static_cast<uint8_t>(Op::TestEq)]        = &h_TestEq;
    t[static_cast<uint8_t>(Op::TestNe)]        = &h_TestNe;
    t[static_cast<uint8_t>(Op::TestStrictEq)]  = &h_TestStrictEq;
    t[static_cast<uint8_t>(Op::TestStrictNe)]  = &h_TestStrictNe;
    t[static_cast<uint8_t>(Op::Inc)]           = &h_Inc;
    t[static_cast<uint8_t>(Op::Dec)]           = &h_Dec;
    t[static_cast<uint8_t>(Op::LdaThis)] = &h_gen_LdaThis;
    t[static_cast<uint8_t>(Op::LdaTdz)] = &h_gen_LdaTdz;
    t[static_cast<uint8_t>(Op::LdarChecked)] = &h_gen_LdarChecked;
    t[static_cast<uint8_t>(Op::StarChecked)] = &h_gen_StarChecked;
    t[static_cast<uint8_t>(Op::Div)] = &h_gen_Div;
    t[static_cast<uint8_t>(Op::Mod)] = &h_gen_Mod;
    t[static_cast<uint8_t>(Op::Exp)] = &h_gen_Exp;
    t[static_cast<uint8_t>(Op::BitAnd)] = &h_BitAnd;
    t[static_cast<uint8_t>(Op::BitOr)] = &h_BitOr;
    t[static_cast<uint8_t>(Op::BitXor)] = &h_BitXor;
    t[static_cast<uint8_t>(Op::Shl)] = &h_Shl;
    t[static_cast<uint8_t>(Op::Sar)] = &h_Sar;
    t[static_cast<uint8_t>(Op::Shr)] = &h_Shr;
    t[static_cast<uint8_t>(Op::TestInstanceOf)] = &h_gen_TestInstanceOf;
    t[static_cast<uint8_t>(Op::TestIn)] = &h_gen_TestIn;
    t[static_cast<uint8_t>(Op::Neg)] = &h_gen_Neg;
    t[static_cast<uint8_t>(Op::LogicalNot)] = &h_gen_LogicalNot;
    t[static_cast<uint8_t>(Op::BitNot)] = &h_gen_BitNot;
    t[static_cast<uint8_t>(Op::TypeOf)] = &h_gen_TypeOf;
    t[static_cast<uint8_t>(Op::ToNumber)] = &h_gen_ToNumber;
    t[static_cast<uint8_t>(Op::ToNumeric)] = &h_gen_ToNumeric;
    t[static_cast<uint8_t>(Op::ToTemplateString)] = &h_gen_ToTemplateString;
    t[static_cast<uint8_t>(Op::ToPropertyKey)] = &h_gen_ToPropertyKey;
    t[static_cast<uint8_t>(Op::CheckObjectCoercible)] = &h_gen_CheckObjectCoercible;
    t[static_cast<uint8_t>(Op::LdaLookup)] = &h_LdaLookupFast;
    t[static_cast<uint8_t>(Op::LdaLookupTypeof)] = &h_gen_LdaLookupTypeof;
    t[static_cast<uint8_t>(Op::StaLookup)] = &h_StaLookupFast;
    t[static_cast<uint8_t>(Op::CheckLookupResolvable)] = &h_gen_CheckLookupResolvable;
    t[static_cast<uint8_t>(Op::StaLookupChecked)] = &h_gen_StaLookupChecked;
    t[static_cast<uint8_t>(Op::LdaEnv)] = &h_LdaEnvFast;
    t[static_cast<uint8_t>(Op::StaEnv)] = &h_gen_StaEnv;
    t[static_cast<uint8_t>(Op::StaEnvInit)] = &h_gen_StaEnvInit;
    t[static_cast<uint8_t>(Op::LdaEnvSlot)] = &h_LdaEnvSlotFast;
    t[static_cast<uint8_t>(Op::StaEnvSlot)] = &h_gen_StaEnvSlot;
    t[static_cast<uint8_t>(Op::StaEnvSlotInit)] = &h_gen_StaEnvSlotInit;
    t[static_cast<uint8_t>(Op::BindEnvLocals)] = &h_gen_BindEnvLocals;
    t[static_cast<uint8_t>(Op::EnterLoopEnv)] = &h_gen_EnterLoopEnv;
    t[static_cast<uint8_t>(Op::AdvanceLoopEnv)] = &h_gen_AdvanceLoopEnv;
    t[static_cast<uint8_t>(Op::ExitLoopEnv)] = &h_gen_ExitLoopEnv;
    t[static_cast<uint8_t>(Op::SaveEnv)] = &h_gen_SaveEnv;
    t[static_cast<uint8_t>(Op::RestoreEnv)] = &h_gen_RestoreEnv;
    t[static_cast<uint8_t>(Op::PopEnvSave)] = &h_gen_PopEnvSave;
    t[static_cast<uint8_t>(Op::GetIterator)] = &h_gen_GetIterator;
    t[static_cast<uint8_t>(Op::IteratorNextOrJump)] = &h_gen_IteratorNextOrJump;
    t[static_cast<uint8_t>(Op::IteratorClose)] = &h_gen_IteratorClose;
    t[static_cast<uint8_t>(Op::CreateForInKeys)] = &h_gen_CreateForInKeys;
    t[static_cast<uint8_t>(Op::JumpIfNotNullish)] = &h_gen_JumpIfNotNullish;
    t[static_cast<uint8_t>(Op::JumpIfNullish)] = &h_gen_JumpIfNullish;
    t[static_cast<uint8_t>(Op::JumpIfNotUndefined)] = &h_gen_JumpIfNotUndefined;
    t[static_cast<uint8_t>(Op::CreateClosure)] = &h_gen_CreateClosure;
    t[static_cast<uint8_t>(Op::DeclareFunction)] = &h_gen_DeclareFunction;
    t[static_cast<uint8_t>(Op::EvalAst)] = &h_gen_EvalAst;
    t[static_cast<uint8_t>(Op::CopyRestProperties)] = &h_gen_CopyRestProperties;
    t[static_cast<uint8_t>(Op::Call)] = &h_gen_Call;
    t[static_cast<uint8_t>(Op::CallResolved)] = &h_gen_CallResolved;
    t[static_cast<uint8_t>(Op::Construct)] = &h_gen_Construct;
    t[static_cast<uint8_t>(Op::CallSpread)] = &h_gen_CallSpread;
    t[static_cast<uint8_t>(Op::ConstructSpread)] = &h_gen_ConstructSpread;
    t[static_cast<uint8_t>(Op::CreateRegExp)] = &h_gen_CreateRegExp;
    t[static_cast<uint8_t>(Op::HasPrivate)] = &h_gen_HasPrivate;
    t[static_cast<uint8_t>(Op::LdaEngineHelper)] = &h_gen_LdaEngineHelper;
    t[static_cast<uint8_t>(Op::GetSuper)] = &h_gen_GetSuper;
    t[static_cast<uint8_t>(Op::SetSuper)] = &h_gen_SetSuper;
    t[static_cast<uint8_t>(Op::ResolveSuperBase)] = &h_gen_ResolveSuperBase;
    t[static_cast<uint8_t>(Op::GetSuperKeyed)] = &h_gen_GetSuperKeyed;
    t[static_cast<uint8_t>(Op::SetSuperKeyed)] = &h_gen_SetSuperKeyed;
    t[static_cast<uint8_t>(Op::SuperCall)] = &h_gen_SuperCall;
    t[static_cast<uint8_t>(Op::SpreadInto)] = &h_gen_SpreadInto;
    t[static_cast<uint8_t>(Op::ObjectSpreadInto)] = &h_gen_ObjectSpreadInto;
    t[static_cast<uint8_t>(Op::GetNamed)] = &h_GetNamedFast;
    t[static_cast<uint8_t>(Op::SetNamed)] = &h_SetNamedFast;
    t[static_cast<uint8_t>(Op::GetPrivate)] = &h_gen_GetPrivate;
    t[static_cast<uint8_t>(Op::SetPrivate)] = &h_gen_SetPrivate;
    t[static_cast<uint8_t>(Op::GetKeyed)] = &h_GetKeyedFast;
    t[static_cast<uint8_t>(Op::SetKeyed)] = &h_gen_SetKeyed;
    t[static_cast<uint8_t>(Op::DeleteNamed)] = &h_gen_DeleteNamed;
    t[static_cast<uint8_t>(Op::DeleteKeyed)] = &h_gen_DeleteKeyed;
    t[static_cast<uint8_t>(Op::DefineOwn)] = &h_gen_DefineOwn;
    t[static_cast<uint8_t>(Op::DefineElement)] = &h_gen_DefineElement;
    t[static_cast<uint8_t>(Op::ToPropertyKeyStrict)] = &h_gen_ToPropertyKeyStrict;
    t[static_cast<uint8_t>(Op::DefineOwnKeyed)] = &h_gen_DefineOwnKeyed;
    t[static_cast<uint8_t>(Op::FinalizeStaticProperty)] = &h_gen_FinalizeStaticProperty;
    t[static_cast<uint8_t>(Op::FinalizeComputedProperty)] = &h_gen_FinalizeComputedProperty;
    t[static_cast<uint8_t>(Op::SetFunctionNameIfUnnamed)] = &h_gen_SetFunctionNameIfUnnamed;
    t[static_cast<uint8_t>(Op::CreateObject)] = &h_gen_CreateObject;
    t[static_cast<uint8_t>(Op::CreateArray)] = &h_gen_CreateArray;
    t[static_cast<uint8_t>(Op::CreateRestArray)] = &h_gen_CreateRestArray;
    t[static_cast<uint8_t>(Op::Throw)] = &h_gen_Throw;
    t[static_cast<uint8_t>(Op::ReraiseGeneratorReturn)] = &h_gen_ReraiseGeneratorReturn;
    return t;
}
const std::array<Handler, 256> kHandlers = make_handler_table();

// Entry point: run() hands the frame over, the table takes it from there.
Value run_dispatch(Frame& f) {
    return kHandlers[f.code[f.pc]](f, f.pc, f.acc);
}

Value run(const BytecodeChunk& chunk, Context& ctx, std::span<const Value> args,
          const Value* this_val, Function* owner) {
    // Only the registers the chunk actually uses: a fixed 256 put the whole
    // bank on the C++ stack and zeroed it on every call, when the compiler
    // already knows the real count and it is small for most functions.
    // Zero-initialized either way, so leftover stack garbage in an unused slot
    // can't look like a live heap pointer to the conservative GC scan.
    constexpr uint16_t kInlineRegs = 32;
    Value inline_regs[kInlineRegs] = {};
    Value* regs = inline_regs;
    std::vector<Value> spill_regs;
    if (chunk.register_count > kInlineRegs) {
        // Off the C++ stack, so the conservative scan cannot see it: rooted
        // explicitly for as long as this frame runs.
        spill_regs.resize(chunk.register_count);
        regs = spill_regs.data();
    }
    struct SpillRoot {
        const std::vector<Value>* v;
        ~SpillRoot() { if (v) Collector::pop_value_vector(v); }
    } spill_root{nullptr};
    if (!spill_regs.empty()) {
        Collector::push_value_vector(&spill_regs);
        spill_root.v = &spill_regs;
    }
    const uint8_t param_count = chunk.parameter_count;
    for (uint8_t i = 0; i < param_count && i < args.size(); i++) {
        regs[i] = args[i];
    }

    // Side-stack for Op::SaveEnv/RestoreEnv/PopEnvSave. Not a GC root:
    // Environment objects are already un-GC-managed, leaked with the Context.
    Environment* env_saves[64];
    uint8_t env_save_top = 0;

    if (chunk.env_mode && chunk.env) {
        Environment* env = ctx.get_lexical_environment();
        // Intern the chunk's binding names once instead of once per call; the
        // pointers are stable for the thread's lifetime.
        env->reserve_slots(chunk.env->env_slot_total);
        if (!chunk.env->env_keys_ready) {
            auto& b = const_cast<BytecodeChunk::EnvBundle&>(*chunk.env);
            b.env_param_keys.reserve(b.env_params.size());
            for (const auto& p : b.env_params) b.env_param_keys.push_back(Shape::intern(p));
            b.env_local_keys.reserve(b.env_locals.size());
            for (const auto& l : b.env_locals) b.env_local_keys.push_back(Shape::intern(l.name));
            b.env_keys_ready = true;
        }
        if (chunk.env_params_tdz) {
            // Spec FDI ordering: params start uninitialized (their raw values
            // sit in registers), the entry bytecode initializes each one left
            // to right, and Op::BindEnvLocals creates the body's bindings
            // only after the whole parameter list resolved.
            for (const auto* k : chunk.env->env_param_keys) {
                env->create_uninitialized_binding_interned(k, true);
            }
        } else {
            for (size_t i = 0; i < chunk.env->env_param_keys.size(); i++) {
                Value v = i < args.size() ? args[i] : Value();
                env->create_binding_interned(chunk.env->env_param_keys[i], v, true);
            }
            for (size_t li = 0; li < chunk.env->env_locals.size(); li++) {
                const auto& loc = chunk.env->env_locals[li];
                const std::string* lk = chunk.env->env_local_keys[li];
                if (loc.is_lexical) {
                        env->create_uninitialized_binding_interned(lk, !loc.is_const);
                        // is_strict_const() wants the const SET, not just the cleared mutable
                        // flag, and every "Assignment to constant variable" check gates on
                        // it -- without this they are all inert in sloppy mode for a binding
                        // the VM created.
                        if (loc.is_const) env->mark_const_binding(loc.name);
                    }
                else env->create_binding_interned(lk, Value(), true);
            }
        }
    }

    // The frame's own environment: per-call, so lookup_cache must never
    // point into it (outer captured envs are the cacheable ones). A script
    // frame's env is the persistent script env -- fully cacheable.
    Environment* entry_env = chunk.script_mode ? nullptr : ctx.get_lexical_environment();

    // A chunk may be shared across several Function instances created from the
    // same declaration site (see FunctionExecutable), each with its own
    // captured environment chain -- chunk.lookup_cache can't
    // be trusted in that case (it would bake in whichever instance resolved a
    // name first and serve that stale slot to every other instance forever).
    // Route through the calling Function's own per-instance cache instead;
    // owner is null only for the ownerless top-level script chunk, which is
    // inherently single-instance, so chunk.lookup_cache stays fine there.
    // Raw pointer, not vector<LookupCacheEntry>*: chunk.lookup_cache and
    // owner->instance_lookup_cache() use different allocators (the latter is
    // pooled -- see its declaration), so they're different vector types.
    // .data() returns the same LookupCacheEntry* either way. Safe to capture
    // once here and reuse for the whole call: chunk.lookup_cache is only
    // ever assigned at compile time (never resized during execution), and
    // instance_lookup_cache_ is resized at most once ever per instance
    // (chunk.names.size() is fixed for a given owner), always in this setup
    // code before that call's own bytecode -- including recursive self-calls
    // -- ever dispatches, so no reentrant call can invalidate this pointer.
    // Reaching for the instance cache is what MATERIALIZES a Function's
    // instance data, so a chunk with no LdaLookup/StaLookup must not ask: that
    // block is far larger than the cache it would hold, it is one per closure
    // rather than one per declaration site, and nothing in this frame will
    // read a single entry of it.
    BytecodeChunk::LookupCacheEntry* lookup_cache_data = chunk.lookup_cache.data();
    if (owner && chunk.uses_lookup_cache) {
        auto& instance_cache = owner->instance_lookup_cache();
        if (instance_cache.size() < chunk.names.size()) instance_cache.resize(chunk.names.size());
        lookup_cache_data = instance_cache.data();
    }

    // Same routing as lookup_cache_data above, for the same reason: a shared
    // chunk's GetPrivate/SetPrivate sites cache a resolved qualified key that
    // encodes the CALLING instance's own declaring brand (see PrivateFeedback's
    // doc comment), so every instance sharing the chunk needs its own copy.
    size_t chunk_private_feedback_size = chunk.ic_feedback ? chunk.ic_feedback->private_feedback.size() : 0;
    PrivateFeedback* private_feedback_data = chunk.ic_feedback ? chunk.ic_feedback->private_feedback.data() : nullptr;
    if (owner && chunk_private_feedback_size > 0) {
        auto& instance_pf = owner->instance_private_feedback();
        if (instance_pf.size() < chunk_private_feedback_size) instance_pf.resize(chunk_private_feedback_size);
        private_feedback_data = instance_pf.data();
    }

    // Op::LdaThis cache: `this`'s VALUE is immutable for the whole frame
    // (even in a derived constructor -- super() sets it once), so resolve
    // the binding at most once. Whether a read is ALLOWED yet is a separate,
    // per-read check (this-TDZ, see the opcode below).
    bool this_resolved = this_val != nullptr;
    Value this_value = this_val ? *this_val : Value();

    const uint8_t* code = chunk.code.data();
    const Value* constants = chunk.constants.data();

    // The try sits outside the dispatch loop AND outside its function: a
    // handler live at every throwing call keeps the compiler from holding the
    // loop's state in registers, and a landing pad in the same function does
    // that even with the try hoisted out of the loop itself. Recovery
    // re-enters run_dispatch with frame.pc moved instead of continuing.
    // compile_script is reached from run_script alone, and that is where the
    // chunk gets its root -- so script_mode is exactly "this chunk's caches
    // are traced".
    const bool feedback_rooted = owner != nullptr || chunk.script_mode;
    Frame frame{chunk, ctx, args, owner, feedback_rooted, regs, env_saves, lookup_cache_data,
                private_feedback_data, code, constants, entry_env,
                this_value, Value(), 0, 0, 0, this_resolved};

    for (;;) {
      try {
        return run_dispatch(frame);
      } catch (const YieldException&) {
            throw;
      } catch (const GeneratorReturnException& gen_ret) {
            // .return() resumed a suspended yield/await mid-try: skip any
            // catch clause (spec) and run the covering try/catch's finally,
            // same handler-table lookup as CHECK_EXC but keyed off
            // genreturn_pc -- Op::ReraiseGeneratorReturn re-throws once
            // finally is done, so an enclosing try's own handler picks it
            // up in turn.
            int32_t genreturn_pc = -1;
            uint32_t best_width = UINT32_MAX;
            if (chunk.handlers) for (const auto& h : *chunk.handlers) {
                if (h.genreturn_pc < 0) continue;
                if (frame.instr_pc >= h.start_pc && frame.instr_pc < h.end_pc) {
                    uint32_t width = h.end_pc - h.start_pc;
                    if (width < best_width) { best_width = width; genreturn_pc = h.genreturn_pc; }
                }
            }
            if (genreturn_pc < 0) throw;
            frame.acc = gen_ret.return_value;
            frame.pc = static_cast<uint32_t>(genreturn_pc);
            continue;
      } catch (const std::exception& e) {
            // A native call (e.g. Proxy invariant violation) threw a raw C++
            // exception; CHECK_EXC below routes it like a normal JS throw.
            if (!ctx.has_exception()) ctx.throw_exception(Value(std::string(e.what())));
      } catch (...) {
            if (!ctx.has_exception()) ctx.throw_exception(Value(std::string("Error: Unknown error")));
      }

      // The same handler search CHECK_EXC does, for an exception that arrived
      // as a C++ throw rather than through the context.
      if (ctx.has_exception()) {
          int32_t handler_pc = -1;
          uint32_t best_width = UINT32_MAX;
          if (chunk.handlers) for (const auto& h : *chunk.handlers) {
              if (frame.instr_pc >= h.start_pc && frame.instr_pc < h.end_pc) {
                  uint32_t width = h.end_pc - h.start_pc;
                  if (width < best_width) { best_width = width; handler_pc = static_cast<int32_t>(h.handler_pc); }
              }
          }
          if (handler_pc < 0) return Value();
          frame.acc = ctx.get_exception();
          ctx.clear_exception();
          frame.pc = static_cast<uint32_t>(handler_pc);
          continue;
      }
      // Only reachable if a catch above left no exception pending, which none
      // of them do; resume at the instruction that was executing.
      frame.pc = frame.instr_pc;
    }
}

Value run_script(const std::vector<std::unique_ptr<ASTNode>>& statements,
                 Context& ctx, bool& used_vm) {
    used_vm = false;
    if (!enabled()) return Value();
    for (Environment* e = ctx.get_lexical_environment(); e; e = e->get_outer()) {
        if (e->is_with_environment()) return Value();
    }
    auto chunk = BytecodeCompiler::compile_script(statements);
    if (!chunk) return Value();
    used_vm = true;
    static const bool disasm = [] {
        const char* env = std::getenv("QUANTA_VM_DISASM");
        return env && env[0] == '1';
    }();
    if (disasm) {
        std::fprintf(stderr, "%s", disassemble_chunk(*chunk, "<script>").c_str());
    }
    ValueArrayRoot const_root(&chunk->constants);
    // The caches this chunk learns hold real cells and nothing else keeps them
    // alive: no function owns this chunk. Rooted for the run, they work here
    // the same as they do inside a function.
    ChunkFeedbackRoot feedback_root(chunk.get());
    Value global_this = ctx.get_global_object()
        ? Value(ctx.get_global_object()) : Value();
    return run(*chunk, ctx, {}, &global_this);
}

std::unique_ptr<BytecodeChunk> compile_suspendable(const ASTNode* body) {
    if (!enabled() || !body) return nullptr;
    static const std::vector<std::unique_ptr<Parameter>> no_params;
    auto chunk = BytecodeCompiler::compile(body, no_params, /*suspendable=*/true);
    if (!chunk) return nullptr;
    static const bool disasm = [] {
        const char* env = std::getenv("QUANTA_VM_DISASM");
        return env && env[0] == '1';
    }();
    if (disasm) {
        std::fprintf(stderr, "%s", disassemble_chunk(*chunk, "<suspendable>").c_str());
    }
    return chunk;
}

Value run_suspendable_chunk(const BytecodeChunk& chunk, Context& ctx, Function* owner) {
    return run(chunk, ctx, {}, nullptr, owner);
}

}
}
