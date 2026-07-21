/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

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
                     uint64_t no_override_epoch = Object::descriptor_epoch());

// Mirrors MemberExpression::evaluate's primitive-receiver branch.
Value get_primitive_named(Context& ctx, const Value& prim, const std::string& name,
                           FeedbackSlot* fb) {
    if (prim.is_string()) {
        if (name == "length") {
            return Value(static_cast<double>(utf16_length(prim.to_string())));
        }
        size_t idx;
        if (key_is_canonical_index(name, idx)) {
            int32_t unit = utf16_code_unit_at(prim.to_string(), idx);
            if (unit < 0) return Value();  // out of range: no such own property
            return Value(encode_utf16_unit(static_cast<uint32_t>(unit)));
        }
    }
    const char* ctor_name = prim.is_string() ? "String"
        : prim.is_number() ? "Number"
        : prim.is_bigint() ? "BigInt"
        : prim.is_boolean() ? "Boolean"
        : prim.is_symbol() ? "Symbol"
        : nullptr;
    if (!ctor_name) return Value();
    Value ctor = ctx.get_binding(ctor_name);
    if (ctx.has_exception() || !ctor.is_function()) return Value();
    Value proto = ctor.as_function()->get_property("prototype");
    if (ctx.has_exception() || !proto.is_object()) return Value();
    Object* proto_obj = proto.as_object();

    // Mono/poly cache keyed on proto_obj's OWN shape (every string shares the
    // same String.prototype, etc.) -- not the receiver's shape, which is why
    // this is a separate check from GetNamed's own receiver-shape cache even
    // though it shares the same fb/kMaxEntries budget (proto_obj's Shape* is
    // always distinct from any real receiver's shape, so entries from each
    // never collide/alias, they just compete for the same 4 slots).
    bool cacheable = fb && !fb->mega && proto_obj->get_type() == Object::ObjectType::Ordinary &&
                      !proto_obj->has_descriptor_override(name);
    if (cacheable) {
        Shape* shape = proto_obj->get_shape();
        for (uint8_t i = 0; i < fb->count; i++) {
            if (fb->entries[i].shape == shape) {
                const Value* slot = proto_obj->get_shape_slot_unchecked(fb->entries[i].slot_index);
                if (slot) return *slot;
                break;
            }
        }
    }
    PropertyDescriptor desc = proto_obj->get_property_descriptor(name);
    if (desc.is_accessor_descriptor()) {
        if (!desc.has_getter()) return Value();
        Function* getter = dynamic_cast<Function*>(desc.get_getter());
        return getter ? getter->call(ctx, {}, prim) : Value();
    }
    if (cacheable && desc.has_value()) {
        Shape* s = proto_obj->get_shape();
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
                        Function* setter = dynamic_cast<Function*>(desc.get_setter());
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
void learn_feedback(FeedbackSlot* fb, Shape* shape, uint32_t slot_index, uint64_t no_override_epoch) {
    for (uint8_t i = 0; i < fb->count; i++) {
        if (fb->entries[i].shape == shape) {
            // Refresh, not no-op: a caller re-deriving this same shape has
            // just re-confirmed "no override" too (same precondition every
            // call site here shares), so the entry's epoch stamp needs to
            // move forward or it stays permanently stale after the first
            // unrelated descriptor_epoch bump anywhere in the program.
            fb->entries[i].no_override_epoch = no_override_epoch;
            return;
        }
    }
    if (fb->count < FeedbackSlot::kMaxEntries) {
        fb->entries[fb->count++] = {shape, slot_index, no_override_epoch};
    } else {
        fb->mega = true;
    }
}

// Like learn_feedback, but a duplicate from_shape REFRESHES the entry rather
// than no-opping: to_shape/slot_index are deterministic for (from_shape, key),
// but proto_epoch goes stale and a re-hit needs the current value to trust it.
void learn_transition(FeedbackSlot* fb, Shape* from_shape, Shape* to_shape,
                       uint32_t slot_index, uint64_t epoch) {
    for (uint8_t i = 0; i < fb->transition_count; i++) {
        if (fb->transitions[i].from_shape == from_shape) {
            fb->transitions[i] = {from_shape, to_shape, slot_index, epoch};
            return;
        }
    }
    if (fb->transition_count < FeedbackSlot::kMaxEntries) {
        fb->transitions[fb->transition_count++] = {from_shape, to_shape, slot_index, epoch};
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
                  Object* holder, uint32_t slot_index, uint64_t epoch, Function* owner) {
    for (uint8_t i = 0; i < fb->proto_count; i++) {
        auto& pe = fb->proto_entries[i];
        if (pe.receiver_shape == receiver_shape && pe.prototype == prototype) {
            Collector::write_barrier(owner);
            pe = {receiver_shape, prototype, epoch, holder, slot_index};
            return;
        }
    }
    if (fb->proto_count < FeedbackSlot::kMaxEntries) {
        Collector::write_barrier(owner);
        fb->proto_entries[fb->proto_count++] = {receiver_shape, prototype, epoch, holder, slot_index};
    } else {
        fb->proto_mega = true;
    }
}

// A shape match alone doesn't prove "plain data slot, no override" -- both
// the hit check and the miss-path refill re-verify has_descriptor_override.
// has_descriptor_override is a fact about (obj, name), independent of which
// entry matches, so it's checked once per call (in `cacheable`) rather than
// once per scanned entry.
Value get_named(Context& ctx, const Value& receiver, const std::string& name,
                 FeedbackSlot* fb, Function* owner) {
    if (receiver.is_null() || receiver.is_undefined()) {
        ctx.throw_type_error("Cannot read property of null or undefined");
        return Value();
    }
    Object* obj = as_object_like(receiver);
    if (!obj) return get_primitive_named(ctx, receiver, name, fb);

    // Array.length is always live-computed from elements_ (never reliably
    // mirrored in a shape slot -- push/index-growth never syncs one, only
    // an explicit `arr.length = N` assignment does), so it can never be
    // cached as a shape-slot value. Skip the whole descriptor dance below
    // and go straight to it, same as Object::get_property's own Array
    // branch -- but a defineProperty override (e.g. non-writable length)
    // still must win, so that rare case still falls through.
    if (obj->get_type() == Object::ObjectType::Array && name == "length" &&
        !obj->has_descriptor_override(name)) {
        return Value(static_cast<double>(obj->get_length()));
    }

    Shape* obj_shape = obj->get_shape();
    bool ordinary = obj->get_type() == Object::ObjectType::Ordinary;
    uint64_t cur_epoch = Object::descriptor_epoch();

    // Fast path: a previously-learned entry for this exact shape already
    // confirmed "no descriptors_ override for this key" -- if the global
    // descriptor epoch hasn't moved since, trust that without rescanning
    // descriptors_ at all (skip find_descriptor_override entirely). Safe
    // even though descriptors_ is per-object, not per-shape: ANY object
    // anywhere gaining a new override bumps the epoch, invalidating this
    // for every shape, not just the one that changed -- see
    // Object::descriptor_epoch_'s doc comment.
    if (fb && !fb->mega && ordinary && obj_shape) {
        for (uint8_t i = 0; i < fb->count; i++) {
            if (fb->entries[i].shape == obj_shape) {
                if (fb->entries[i].no_override_epoch == cur_epoch) {
                    const Value* slot = obj->get_shape_slot_unchecked(fb->entries[i].slot_index);
                    if (slot) return *slot;
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
    bool cacheable = fb && !fb->mega && ordinary && !override_desc;
    if (cacheable) {
        Shape* shape = obj_shape;
        for (uint8_t i = 0; i < fb->count; i++) {
            if (fb->entries[i].shape == shape) {
                const Value* slot = obj->get_shape_slot_unchecked(fb->entries[i].slot_index);
                if (slot) return *slot;
                break;
            }
        }
    }
    // An own accessor must run before get_property()'s type-specific
    // shortcuts, which don't know about one installed via defineProperty.
    if (obj->get_type() != Object::ObjectType::Proxy) {
        PropertyDescriptor desc = override_desc ? *override_desc : obj->get_property_descriptor(name);
        if (desc.is_accessor_descriptor()) {
            if (!desc.has_getter()) return Value();
            Function* getter_fn = dynamic_cast<Function*>(desc.get_getter());
            return getter_fn ? getter_fn->call(ctx, {}, receiver) : Value();
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
                if (idx >= 0) learn_feedback(fb, s, static_cast<uint32_t>(idx), cur_epoch);
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
            if (owner && fb && !fb->proto_mega && obj->get_type() == Object::ObjectType::Ordinary &&
                !obj->has_descriptor_override(name)) {
                Shape* shape = obj->get_shape();
                Object* proto0 = obj->get_prototype();
                uint64_t epoch = Object::proto_epoch();
                for (uint8_t i = 0; i < fb->proto_count; i++) {
                    const auto& pe = fb->proto_entries[i];
                    if (pe.receiver_shape == shape && pe.prototype == proto0 && pe.proto_epoch == epoch) {
                        const Value* slot = pe.holder->get_shape_slot_unchecked(pe.slot_index);
                        if (slot) return *slot;
                        break;
                    }
                }
            }
            for (Object* proto = obj->get_prototype(); proto; proto = proto->get_prototype()) {
                PropertyDescriptor proto_desc = proto->get_property_descriptor(name);
                if (proto_desc.is_accessor_descriptor()) {
                    if (!proto_desc.has_getter()) return Value();
                    Function* getter_fn = dynamic_cast<Function*>(proto_desc.get_getter());
                    return getter_fn ? getter_fn->call(ctx, {}, receiver) : Value();
                }
                if (proto_desc.has_value()) {
                    // Learn: proto is the holder. Only cacheable as a plain
                    // shape slot (Ordinary, no descriptor override) -- same
                    // trust rule as the receiver-side cache.
                    if (owner && fb && !fb->proto_mega && proto->get_type() == Object::ObjectType::Ordinary &&
                        !proto->has_descriptor_override(name)) {
                        Shape* hs = proto->get_shape();
                        int32_t hidx = hs ? hs->find_slot(name) : -1;
                        if (hidx >= 0) {
                            learn_proto(fb, obj->get_shape(), obj->get_prototype(), proto,
                                        static_cast<uint32_t>(hidx), Object::proto_epoch(), owner);
                        }
                    }
                    break;
                }
            }
        }
    }
    Value result = obj->get_property(name);
    if (ctx.has_exception()) return Value();
    if (cacheable) {
        Shape* s = obj->get_shape();
        int32_t idx = s ? s->find_slot(name) : -1;
        if (idx >= 0) learn_feedback(fb, s, static_cast<uint32_t>(idx), cur_epoch);
    }
    return result;
}

void set_named(Context& ctx, const Value& receiver, const std::string& name,
               const Value& value, FeedbackSlot* fb) {
    if (receiver.is_null() || receiver.is_undefined()) {
        ctx.throw_type_error(std::string("Cannot set properties of ") +
            (receiver.is_null() ? "null" : "undefined"));
        return;
    }
    Object* obj = as_object_like(receiver);
    if (!obj) { set_primitive_named(ctx, receiver, name, value); return; }

    Collector::write_barrier(obj);
    bool ordinary_recv = obj->get_type() == Object::ObjectType::Ordinary;
    // Same epoch-trusting fast path as get_named's own -- skips
    // has_descriptor_override entirely on a hit. See Object::
    // descriptor_epoch_'s doc comment for why a global epoch is safe here
    // even though descriptors_ is per-object.
    if (fb && !fb->mega && ordinary_recv) {
        Shape* shape = obj->get_shape();
        for (uint8_t i = 0; i < fb->count; i++) {
            if (fb->entries[i].shape == shape) {
                if (fb->entries[i].no_override_epoch == Object::descriptor_epoch()) {
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
        uint64_t epoch = Object::proto_epoch();
        for (uint8_t i = 0; i < fb->transition_count; i++) {
            const auto& te = fb->transitions[i];
            if (te.from_shape == shape && te.proto_epoch == epoch) {
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
            learn_transition(fb, shape_before, s, static_cast<uint32_t>(idx), Object::proto_epoch());
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
    Collector::write_barrier(obj);
    if (fb && !fb->transition_mega && obj->get_type() == Object::ObjectType::Ordinary &&
        !obj->has_descriptor_override(key) && obj->is_extensible()) {
        Shape* shape = obj->get_shape();
        uint64_t epoch = Object::proto_epoch();
        for (uint8_t i = 0; i < fb->transition_count; i++) {
            const auto& te = fb->transitions[i];
            if (te.from_shape == shape && te.proto_epoch == epoch) {
                obj->add_shape_property_cached(key, value, te.to_shape);
                return;
            }
        }
    }
    Shape* shape_before = obj->get_shape();
    bool was_new = fb && obj->get_type() == Object::ObjectType::Ordinary &&
                    !obj->has_descriptor_override(key) &&
                    shape_before && shape_before->find_slot(key) < 0;
    obj->create_own_data_property(key, value);
    if (was_new && fb && !fb->transition_mega &&
        obj->get_type() == Object::ObjectType::Ordinary && !obj->has_descriptor_override(key)) {
        Shape* s = obj->get_shape();
        int32_t idx = s ? s->find_slot(key) : -1;
        if (idx >= 0) {
            learn_transition(fb, shape_before, s, static_cast<uint32_t>(idx), Object::proto_epoch());
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
        if (fb && !fb->transition_mega) {
            Shape* shape = obj->get_shape();
            uint64_t epoch = Object::proto_epoch();
            for (uint8_t i = 0; i < fb->transition_count; i++) {
                const auto& te = fb->transitions[i];
                if (te.from_shape == shape && te.proto_epoch == epoch) {
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
                learn_transition(fb, shape_before, to_shape, 0, Object::proto_epoch());
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
    Value result = get_named(ctx, receiver, key, nullptr, nullptr);
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
                if (slot) { Collector::write_barrier(obj); *slot = value; return; }
                break;
            }
        }
    }
    set_named(ctx, receiver, key, value, nullptr);
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
            Function* getter_fn = dynamic_cast<Function*>(own_d.get_getter());
            return getter_fn ? getter_fn->call(ctx, {}, receiver) : Value();
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
            Function* getter_fn = dynamic_cast<Function*>(d.get_getter());
            return getter_fn ? getter_fn->call(ctx, {}, receiver) : Value();
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
            Function* getter_fn = dynamic_cast<Function*>(d.get_getter());
            return getter_fn ? getter_fn->call(ctx, {}, receiver) : Value();
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
    Collector::write_barrier(obj);
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
            Function* setter_fn = dynamic_cast<Function*>(own_pd.get_setter());
            if (setter_fn) setter_fn->call(ctx, {value}, receiver);
            return;
        }
        if (own_pd.has_value() && own_pd.get_value().is_function()) {
            Function* mfn = own_pd.get_value().as_function();
            if (mfn && mfn->has_property("__private_class_brand__")) {
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
        Function* setter_fn = dynamic_cast<Function*>(pd.get_setter());
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

Value run(const BytecodeChunk& chunk, Context& ctx, const std::vector<Value>& args,
          const Value* this_val, Function* owner) {
    // Zero-initialized so leftover stack garbage in an unused slot can't look
    // like a live heap pointer to the conservative GC scan.
    Value regs[256] = {};
    const uint8_t param_count = chunk.parameter_count;
    for (uint8_t i = 0; i < param_count && i < args.size(); i++) {
        regs[i] = args[i];
    }

    // Side-stack for Op::SaveEnv/RestoreEnv/PopEnvSave. Not a GC root:
    // Environment objects are already un-GC-managed, leaked with the Context.
    Environment* env_saves[64];
    uint8_t env_save_top = 0;

    if (chunk.env_mode) {
        Environment* env = ctx.get_lexical_environment();
        if (chunk.env_params_tdz) {
            // Spec FDI ordering: params start uninitialized (their raw values
            // sit in registers), the entry bytecode initializes each one left
            // to right, and Op::BindEnvLocals creates the body's bindings
            // only after the whole parameter list resolved.
            for (const auto& p : chunk.env_params) {
                env->create_uninitialized_binding(p, true);
            }
        } else {
            for (size_t i = 0; i < chunk.env_params.size(); i++) {
                Value v = i < args.size() ? args[i] : Value();
                env->create_binding(chunk.env_params[i], v, true);
            }
            for (const auto& loc : chunk.env_locals) {
                if (loc.is_lexical) env->create_uninitialized_binding(loc.name, !loc.is_const);
                else env->create_binding(loc.name, Value(), true);
            }
        }
    }

    // The frame's own environment: per-call, so lookup_cache must never
    // point into it (outer captured envs are the cacheable ones). A script
    // frame's env is the persistent script env -- fully cacheable.
    Environment* entry_env = chunk.script_mode ? nullptr : ctx.get_lexical_environment();

    // A chunk may be shared across several Function instances created from the
    // same declaration site (see nested_chunk_cache_/attach_precompiled_chunk),
    // each with its own captured environment chain -- chunk.lookup_cache can't
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
    BytecodeChunk::LookupCacheEntry* lookup_cache_data = chunk.lookup_cache.data();
    if (owner) {
        auto& instance_cache = owner->instance_lookup_cache();
        if (instance_cache.size() < chunk.names.size()) instance_cache.resize(chunk.names.size());
        lookup_cache_data = instance_cache.data();
    }

    // Op::LdaThis cache: `this`'s VALUE is immutable for the whole frame
    // (even in a derived constructor -- super() sets it once), so resolve
    // the binding at most once. Whether a read is ALLOWED yet is a separate,
    // per-read check (this-TDZ, see the opcode below).
    bool this_resolved = this_val != nullptr;
    Value this_value = this_val ? *this_val : Value();

    const uint8_t* code = chunk.code.data();
    const Value* constants = chunk.constants.data();
    uint32_t pc = 0;
    uint32_t instr_pc = 0;  // pc of the instruction currently executing, for handler lookup
    Value acc;

    // On exception, find the innermost handler covering instr_pc; `continue`
    // re-enters the for(;;) below with pc already moved to the handler.
// NOT a do-while(0) macro: `continue` inside do{}while(0) binds to the
// do-while, not the dispatch loop -- the rest of the case would keep
// running with pc already pointed at the handler, clobbering the
// exception value in acc (found via ~{valueOf(){throw}} in a try).
#define CHECK_EXC()                                                        \
    if (ctx.has_exception()) {                                            \
        int32_t handler_pc = -1;                                           \
        uint32_t best_width = UINT32_MAX;                                  \
        for (const auto& h : chunk.handlers) {                            \
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
        if (LIKELY(lhs.is_number() && acc.is_number())) {                  \
            double l = lhs.as_number();                                    \
            double r = acc.as_number();                                    \
            (void)l; (void)r;                                              \
            acc = (expr);                                                  \
        } else {                                                           \
            acc = binary_slow(ctx, binop, lhs, acc);                       \
            CHECK_EXC();                                                   \
        }                                                                  \
    } while (0)

    for (;;) {
        instr_pc = pc;
        Op op = static_cast<Op>(code[pc++]);
        try {
        switch (op) {
            case Op::LdaConst:
                acc = constants[read_u16(code, pc)];
                pc += 2;
                break;
            case Op::LdaZero:
                acc = Value(0.0);
                break;
            case Op::LdaSmi:
                acc = Value(static_cast<double>(static_cast<int8_t>(code[pc])));
                pc += 1;
                break;
            case Op::LdaUndefined:
                acc = Value();
                break;
            case Op::LdaNull:
                acc = Value::null();
                break;
            case Op::LdaTrue:
                acc = Value(true);
                break;
            case Op::LdaFalse:
                acc = Value(false);
                break;

            case Op::LdaThis: {
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
            case Op::Ldar:
                acc = regs[code[pc]];
                pc += 1;
                break;
            case Op::Star:
                regs[code[pc]] = acc;
                pc += 1;
                break;
            case Op::Mov:
                regs[code[pc + 1]] = regs[code[pc]];
                pc += 2;
                break;

            case Op::LdaTdz:
                acc = Value::vm_tdz_sentinel();
                break;
            case Op::LdarChecked: {
                uint8_t reg = code[pc];
                uint16_t name_idx = read_u16(code, pc + 1);
                pc += 3;
                if (regs[reg].is_vm_tdz_sentinel()) {
                    ctx.throw_reference_error("Cannot access '" + chunk.names[name_idx] +
                                               "' before initialization");
                    CHECK_EXC();
                    break;
                }
                acc = regs[reg];
                break;
            }
            case Op::StarChecked: {
                uint8_t reg = code[pc];
                uint16_t name_idx = read_u16(code, pc + 1);
                pc += 3;
                if (regs[reg].is_vm_tdz_sentinel()) {
                    ctx.throw_reference_error("Cannot access '" + chunk.names[name_idx] +
                                               "' before initialization");
                    CHECK_EXC();
                    break;
                }
                regs[reg] = acc;
                break;
            }

            case Op::Add:    BINARY_OP(BinOp::ADD, Value(l + r)); break;
            case Op::Sub:    BINARY_OP(BinOp::SUBTRACT, Value(l - r)); break;
            case Op::Mul:    BINARY_OP(BinOp::MULTIPLY, Value(l * r)); break;
            case Op::Div: {
                // The number fast path in apply_operator has dedicated
                // divide-by-zero handling; keep divide on the shared path.
                const Value& lhs = regs[code[pc]];
                pc += 1;
                acc = binary_slow(ctx, BinOp::DIVIDE, lhs, acc);
                CHECK_EXC();
                break;
            }
            case Op::Mod: {
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
            case Op::Exp: {
                const Value& lhs = regs[code[pc]];
                pc += 1;
                acc = binary_slow(ctx, BinOp::EXPONENT, lhs, acc);
                CHECK_EXC();
                break;
            }
            case Op::BitAnd: {
                const Value& lhs = regs[code[pc]];
                pc += 1;
                acc = binary_slow(ctx, BinOp::BITWISE_AND, lhs, acc);
                CHECK_EXC();
                break;
            }
            case Op::BitOr: {
                const Value& lhs = regs[code[pc]];
                pc += 1;
                acc = binary_slow(ctx, BinOp::BITWISE_OR, lhs, acc);
                CHECK_EXC();
                break;
            }
            case Op::BitXor: {
                const Value& lhs = regs[code[pc]];
                pc += 1;
                acc = binary_slow(ctx, BinOp::BITWISE_XOR, lhs, acc);
                CHECK_EXC();
                break;
            }
            case Op::Shl: {
                const Value& lhs = regs[code[pc]];
                pc += 1;
                acc = binary_slow(ctx, BinOp::LEFT_SHIFT, lhs, acc);
                CHECK_EXC();
                break;
            }
            case Op::Shr: {
                const Value& lhs = regs[code[pc]];
                pc += 1;
                acc = binary_slow(ctx, BinOp::UNSIGNED_RIGHT_SHIFT, lhs, acc);
                CHECK_EXC();
                break;
            }
            case Op::Sar: {
                const Value& lhs = regs[code[pc]];
                pc += 1;
                acc = binary_slow(ctx, BinOp::RIGHT_SHIFT, lhs, acc);
                CHECK_EXC();
                break;
            }

            case Op::TestEq:       BINARY_OP(BinOp::EQUAL, Value(l == r)); break;
            case Op::TestNe:       BINARY_OP(BinOp::NOT_EQUAL, Value(l != r)); break;
            case Op::TestStrictEq: BINARY_OP(BinOp::STRICT_EQUAL, Value(l == r)); break;
            case Op::TestStrictNe: BINARY_OP(BinOp::STRICT_NOT_EQUAL, Value(l != r)); break;
            case Op::TestLt:       BINARY_OP(BinOp::LESS_THAN, Value(l < r)); break;
            case Op::TestGt:       BINARY_OP(BinOp::GREATER_THAN, Value(l > r)); break;
            case Op::TestLe:       BINARY_OP(BinOp::LESS_EQUAL, Value(l <= r)); break;
            case Op::TestGe:       BINARY_OP(BinOp::GREATER_EQUAL, Value(l >= r)); break;

            case Op::TestInstanceOf: {
                const Value& lhs = regs[code[pc]];
                pc += 1;
                acc = binary_slow(ctx, BinOp::INSTANCEOF, lhs, acc);
                CHECK_EXC();
                break;
            }
            case Op::TestIn: {
                const Value& lhs = regs[code[pc]];
                pc += 1;
                acc = binary_slow(ctx, BinOp::IN, lhs, acc);
                CHECK_EXC();
                break;
            }

            case Op::Neg:
                if (acc.is_object() || acc.is_function()) {
                    acc = UnaryExpression::to_numeric(ctx, acc);
                    CHECK_EXC();
                }
                acc = acc.unary_minus();
                break;
            case Op::LogicalNot:
                acc = acc.logical_not();
                break;
            case Op::BitNot:
                if (acc.is_object() || acc.is_function()) {
                    acc = UnaryExpression::to_numeric(ctx, acc);
                    CHECK_EXC();
                }
                acc = acc.bitwise_not();
                break;
            case Op::TypeOf:
                acc = acc.typeof_op();
                break;
            case Op::ToNumber:
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
            case Op::ToNumeric:
                acc = UnaryExpression::to_numeric(ctx, acc);
                CHECK_EXC();
                break;
            case Op::ToTemplateString:
                acc = Value(TemplateLiteral::stringify_element(ctx, acc));
                CHECK_EXC();
                break;
            case Op::ToPropertyKey:
                if (!acc.is_string()) {
                    acc = Value(acc.to_property_key());
                    CHECK_EXC();
                }
                break;
            case Op::CheckObjectCoercible:
                if (acc.is_null() || acc.is_undefined()) {
                    ctx.throw_type_error(std::string("Cannot read properties of ") +
                        (acc.is_null() ? "null" : "undefined"));
                    CHECK_EXC();
                }
                break;
            case Op::Inc:
            case Op::Dec: {
                Value numeric = acc;
                if (!numeric.is_number() && !numeric.is_bigint()) {
                    numeric = UnaryExpression::to_numeric(ctx, numeric);
                    CHECK_EXC();
                }
                double delta = (op == Op::Inc) ? 1.0 : -1.0;
                if (numeric.is_bigint()) {
                    acc = Value(new BigInt(op == Op::Inc
                        ? *numeric.as_bigint() + BigInt(1)
                        : *numeric.as_bigint() - BigInt(1)));
                } else {
                    acc = Value(numeric.to_number() + delta);
                }
                break;
            }

            case Op::LdaLookup: {
                uint16_t name_idx = read_u16(code, pc);
                pc += 2;
                {
                    // Captured-chain fast path: the resolved binding address is
                    // stable for this chunk's lifetime (see lookup_cache).
                    const auto& entry = lookup_cache_data[name_idx];
                    if (entry.slot) { acc = *entry.slot; break; }
                }
                // Mirrors Identifier::evaluate: TDZ first, then one scope-chain walk.
                const std::string& name = chunk.names[name_idx];
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
                    if (env != entry_env) {
                        if (Value* slot = env->stable_binding_slot(name)) {
                            lookup_cache_data[name_idx] = {env, slot};
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

            case Op::LdaLookupTypeof: {
                // `typeof x` suppresses only the unresolved-binding case, not TDZ.
                const std::string& name = chunk.names[read_u16(code, pc)];
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

            case Op::StaLookup: {
                uint16_t sta_name_idx = read_u16(code, pc);
                pc += 2;
                {
                    const auto& entry = lookup_cache_data[sta_name_idx];
                    if (entry.slot) {
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
                const std::string& name = chunk.names[sta_name_idx];
                if (ctx.is_in_tdz(name)) {
                    ctx.throw_reference_error("Cannot access '" + name + "' before initialization");
                    CHECK_EXC();
                    break;
                }
                Environment* env = ctx.find_binding_env(name);
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
                        if (Value* slot = env->stable_binding_slot(name)) {
                            lookup_cache_data[sta_name_idx] = {env, slot};
                        }
                    }
                }
                CHECK_EXC();
                break;
            }

            case Op::CheckLookupResolvable: {
                const std::string& name = chunk.names[read_u16(code, pc)];
                pc += 2;
                acc = Value(ctx.find_binding_env(name) != nullptr || ctx.has_binding(name));
                break;
            }

            case Op::StaLookupChecked: {
                uint8_t resolved_reg = code[pc];
                const std::string& name = chunk.names[read_u16(code, pc + 1)];
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

            case Op::LdaEnv: {
                const std::string& name = chunk.names[read_u16(code, pc)];
                pc += 2;
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
            case Op::StaEnv: {
                const std::string& name = chunk.names[read_u16(code, pc)];
                pc += 2;
                if (ctx.is_in_tdz(name)) {
                    ctx.throw_reference_error("Cannot access '" + name + "' before initialization");
                    CHECK_EXC();
                    break;
                }
                Environment* env = ctx.find_binding_env(name);
                if (env) {
                    env->set_binding_direct(name, acc, &ctx);
                } else {
                    ctx.throw_reference_error("'" + name + "' is not defined");
                }
                CHECK_EXC();
                break;
            }
            case Op::StaEnvInit: {
                const std::string& name = chunk.names[read_u16(code, pc)];
                pc += 2;
                ctx.get_lexical_environment()->initialize_binding(name, acc);  // current environment, no chain walk
                break;
            }

            // Guarded direct-slot variants (see BytecodeCompiler.h's
            // EnvSlotInfo and Environment::inline_slot for why the
            // predicted slot needs re-validation). On a guard miss these
            // fall through to IDENTICAL code to LdaEnv/StaEnv/StaEnvInit --
            // a miss only costs the fast path, never correctness.
            case Op::LdaEnvSlot: {
                uint8_t slot = code[pc];
                pc += 1;
                const std::string& name = chunk.names[read_u16(code, pc)];
                pc += 2;
                if (auto* e = ctx.get_lexical_environment()->inline_slot(slot, name)) {
                    if (!e->slot.initialized) {
                        ctx.throw_reference_error("Cannot access '" + name + "' before initialization");
                        CHECK_EXC();
                        break;
                    }
                    acc = e->slot.value;
                    break;
                }
                if (ctx.is_in_tdz(name)) {
                    ctx.throw_reference_error("Cannot access '" + name + "' before initialization");
                    CHECK_EXC();
                    break;
                }
                Environment* env = ctx.find_binding_env(name);
                if (env) {
                    acc = env->get_binding_direct(name, &ctx);
                } else {
                    ctx.throw_reference_error("'" + name + "' is not defined");
                }
                CHECK_EXC();
                break;
            }
            case Op::StaEnvSlot: {
                uint8_t slot = code[pc];
                pc += 1;
                const std::string& name = chunk.names[read_u16(code, pc)];
                pc += 2;
                if (auto* e = ctx.get_lexical_environment()->inline_slot(slot, name)) {
                    if (e->slot.mutable_flag) e->slot.value = acc;
                    break;
                }
                if (ctx.is_in_tdz(name)) {
                    ctx.throw_reference_error("Cannot access '" + name + "' before initialization");
                    CHECK_EXC();
                    break;
                }
                Environment* env = ctx.find_binding_env(name);
                if (env) {
                    env->set_binding_direct(name, acc, &ctx);
                } else {
                    ctx.throw_reference_error("'" + name + "' is not defined");
                }
                CHECK_EXC();
                break;
            }
            case Op::StaEnvSlotInit: {
                uint8_t slot = code[pc];
                pc += 1;
                const std::string& name = chunk.names[read_u16(code, pc)];
                pc += 2;
                if (auto* e = ctx.get_lexical_environment()->inline_slot(slot, name)) {
                    e->slot.value = acc;
                    e->slot.initialized = true;
                    break;
                }
                ctx.get_lexical_environment()->initialize_binding(name, acc);
                break;
            }

            case Op::BindEnvLocals: {
                Environment* env = ctx.get_lexical_environment();
                for (const auto& loc : chunk.env_locals) {
                    if (loc.is_lexical) env->create_uninitialized_binding(loc.name, !loc.is_const);
                    else env->create_binding(loc.name, Value(), true);
                }
                break;
            }

            case Op::EnterLoopEnv: {
                uint16_t idx = read_u16(code, pc);
                pc += 2;
                ctx.push_block_scope();
                Environment* env = ctx.get_lexical_environment();
                for (const auto& v : chunk.loop_envs[idx]) {
                    if (v.is_lexical) env->create_uninitialized_binding(v.name, !v.is_const);
                    else env->create_binding(v.name, Value(), true);
                }
                break;
            }
            case Op::AdvanceLoopEnv: {
                uint16_t idx = read_u16(code, pc);
                pc += 2;
                const auto& vars = chunk.loop_envs[idx];
                std::vector<Value> carried(vars.size());
                Environment* old_env = ctx.get_lexical_environment();
                for (size_t i = 0; i < vars.size(); i++) {
                    if (vars[i].copy_forward) carried[i] = old_env->get_binding_direct(vars[i].name, &ctx);
                }
                ctx.pop_block_scope();
                ctx.push_block_scope();
                Environment* new_env = ctx.get_lexical_environment();
                for (size_t i = 0; i < vars.size(); i++) {
                    const auto& v = vars[i];
                    if (v.is_lexical) new_env->create_uninitialized_binding(v.name, !v.is_const);
                    else new_env->create_binding(v.name, Value(), true);
                    if (v.copy_forward) new_env->initialize_binding(v.name, carried[i]);
                }
                break;
            }
            case Op::ExitLoopEnv:
                ctx.pop_block_scope();
                break;

            case Op::SaveEnv:
                env_saves[env_save_top++] = ctx.get_lexical_environment();
                break;
            case Op::RestoreEnv:
                ctx.set_lexical_environment(env_saves[--env_save_top]);
                break;
            case Op::PopEnvSave:
                env_save_top--;
                break;

            case Op::GetIterator: {
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
            case Op::IteratorNextOrJump: {
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
            case Op::IteratorClose: {
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

            case Op::CreateForInKeys: {
                Object* obj = as_object_like(acc);
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

            case Op::JumpIfNotNullish: {
                int16_t off = read_i16(code, pc);
                pc += 2;
                if (!acc.is_null() && !acc.is_undefined()) pc += off;
                break;
            }
            case Op::JumpIfNullish: {
                int16_t off = read_i16(code, pc);
                pc += 2;
                if (acc.is_null() || acc.is_undefined()) pc += off;
                break;
            }
            case Op::JumpIfNotUndefined: {
                int16_t off = read_i16(code, pc);
                pc += 2;
                if (!acc.is_undefined()) pc += off;
                break;
            }

            case Op::CreateClosure: {
                uint16_t idx = read_u16(code, pc);
                pc += 2;
                acc = const_cast<ASTNode*>(chunk.closures[idx])->evaluate(ctx);
                CHECK_EXC();
                break;
            }

            case Op::DestructureBind: {
                uint16_t idx = read_u16(code, pc);
                pc += 2;
                const auto& site = chunk.destructuring_patterns[idx];
                auto* pattern = const_cast<ASTNode*>(site.pattern);
                static_cast<DestructuringAssignment*>(pattern)->evaluate_with_value(
                    ctx, acc, site.as_lexical, site.is_const);
                CHECK_EXC();
                break;
            }

            case Op::Call: {
                uint8_t callee_reg = code[pc];
                uint8_t args_start = code[pc + 1];
                uint8_t argc = code[pc + 2];
                uint16_t name_idx = read_u16(code, pc + 3);
                pc += 5;
                const Value& callee = regs[callee_reg];
                std::vector<Value> call_args(regs + args_start, regs + args_start + argc);
                if (callee.is_function()) {
                    acc = callee.as_function()->call(ctx, call_args, Value());
                } else if (callee.is_object() &&
                           callee.as_object()->get_type() == Object::ObjectType::Proxy) {
                    acc = static_cast<Proxy*>(callee.as_object())->apply_trap(call_args, Value());
                } else {
                    ctx.throw_type_error(chunk.names[name_idx] + " is not a function");
                }
                CHECK_EXC();
                Collector::safepoint();
                break;
            }

            case Op::CallResolved: {
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
                std::vector<Value> call_args(regs + args_start, regs + args_start + argc);
                if (callee.is_function()) {
                    acc = callee.as_function()->call(ctx, call_args, receiver);
                } else if (callee.is_object() &&
                           callee.as_object()->get_type() == Object::ObjectType::Proxy) {
                    acc = static_cast<Proxy*>(callee.as_object())->apply_trap(call_args, receiver);
                } else {
                    ctx.throw_type_error(chunk.names[name_idx] + " is not a function");
                }
                CHECK_EXC();
                Collector::safepoint();
                break;
            }

            case Op::Construct: {
                uint8_t callee_reg = code[pc];
                uint8_t args_start = code[pc + 1];
                uint8_t argc = code[pc + 2];
                uint16_t name_idx = read_u16(code, pc + 3);
                pc += 5;
                const Value& callee = regs[callee_reg];
                std::vector<Value> call_args(regs + args_start, regs + args_start + argc);
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
                    ctx.throw_type_error(chunk.names[name_idx] + " is not a constructor");
                }
                CHECK_EXC();
                Collector::safepoint();
                break;
            }

            case Op::GetNamed: {
                uint8_t obj_reg = code[pc];
                uint16_t name_idx = read_u16(code, pc + 1);
                uint16_t fb_idx = read_u16(code, pc + 3);
                pc += 5;
                acc = get_named(ctx, regs[obj_reg], chunk.names[name_idx], &chunk.feedback[fb_idx], owner);
                CHECK_EXC();
                break;
            }
            case Op::SetNamed: {
                uint8_t obj_reg = code[pc];
                uint16_t name_idx = read_u16(code, pc + 1);
                uint16_t fb_idx = read_u16(code, pc + 3);
                pc += 5;
                set_named(ctx, regs[obj_reg], chunk.names[name_idx], acc, &chunk.feedback[fb_idx]);
                CHECK_EXC();
                break;
            }
            case Op::GetPrivate: {
                uint8_t obj_reg = code[pc];
                uint16_t name_idx = read_u16(code, pc + 1);
                uint16_t fb_idx = read_u16(code, pc + 3);
                pc += 5;
                acc = get_private(ctx, regs[obj_reg], chunk.names[name_idx], &chunk.private_feedback[fb_idx]);
                CHECK_EXC();
                break;
            }
            case Op::SetPrivate: {
                uint8_t obj_reg = code[pc];
                uint16_t name_idx = read_u16(code, pc + 1);
                uint16_t fb_idx = read_u16(code, pc + 3);
                pc += 5;
                set_private(ctx, regs[obj_reg], chunk.names[name_idx], acc, &chunk.private_feedback[fb_idx]);
                CHECK_EXC();
                break;
            }
            case Op::GetKeyed: {
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
                std::string key = acc.to_property_key();
                CHECK_EXC();
                acc = get_keyed(ctx, recv, key, &chunk.keyed_feedback[fb_idx]);
                CHECK_EXC();
                break;
            }
            case Op::SetKeyed: {
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
                std::string key = regs[key_reg].to_property_key();
                CHECK_EXC();
                set_keyed(ctx, recv, key, acc, &chunk.keyed_feedback[fb_idx]);
                CHECK_EXC();
                break;
            }

            case Op::DeleteNamed:
            case Op::DeleteKeyed: {
                uint8_t obj_reg = code[pc];
                std::string property_name;
                if (op == Op::DeleteNamed) {
                    property_name = chunk.names[read_u16(code, pc + 1)];
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

            case Op::DefineOwn: {
                uint8_t obj_reg = code[pc];
                uint16_t name_idx = read_u16(code, pc + 1);
                uint16_t fb_idx = read_u16(code, pc + 3);
                pc += 5;
                Object* obj = as_object_like(regs[obj_reg]);
                define_own_cached(obj, chunk.names[name_idx], acc, &chunk.feedback[fb_idx]);
                CHECK_EXC();
                break;
            }
            case Op::DefineElement: {
                uint8_t obj_reg = code[pc];
                uint8_t key_reg = code[pc + 1];
                pc += 2;
                Object* obj = as_object_like(regs[obj_reg]);
                if (obj) obj->set_element(static_cast<uint32_t>(regs[key_reg].to_number()), acc);
                CHECK_EXC();
                break;
            }
            case Op::ToPropertyKeyStrict:
                if (!acc.is_string()) {
                    acc = Value(acc.to_property_key_strict(ctx));
                    CHECK_EXC();
                }
                break;
            case Op::DefineOwnKeyed: {
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
            case Op::FinalizeStaticProperty: {
                uint8_t obj_reg = code[pc];
                uint16_t key_name_idx = read_u16(code, pc + 1);
                uint16_t display_name_idx = read_u16(code, pc + 3);
                uint8_t raw_kind = code[pc + 5];
                // Bit 0x4: the compiler proved this method's body never
                // references `super`, so the __super_constructor__ write
                // below (needed only for super resolution, see member.cpp)
                // was skipped entirely -- see BytecodeCompiler.cpp's
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
                        fn->set_name(chunk.names[display_name_idx]);
                    }
                    const std::string& key = chunk.names[key_name_idx];
                    if (kind == 0) {
                        // Method: spec 14.3.9 -- non-generator methods are not
                        // constructors and have no .prototype.
                        if (!super_free) fn->set_property("__super_constructor__", Value(true));
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
            case Op::FinalizeComputedProperty: {
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
                    if (!super_free) fn->set_property("__super_constructor__", Value(true));
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
            case Op::SetFunctionNameIfUnnamed: {
                uint16_t name_idx = read_u16(code, pc);
                pc += 2;
                if (acc.is_function()) {
                    Function* fn = acc.as_function();
                    if (fn->get_name().empty() || fn->get_name() == "<arrow>") {
                        fn->set_name(chunk.names[name_idx]);
                    }
                }
                break;
            }
            case Op::CreateObject: {
                pc += 2;  // hint currently informational only (see BytecodeCompiler)
                Object* obj = ObjectFactory::create_object().release();
                obj->reserve_property_slots(read_u16(code, pc - 2));
                acc = Value(obj);
                break;
            }
            case Op::CreateArray: {
                uint16_t n = read_u16(code, pc);
                pc += 2;
                auto arr = ObjectFactory::create_array(0);
                if (n) arr->set_length(n);  // trailing holes count toward length
                acc = Value(arr.release());
                break;
            }
            case Op::CreateRestArray: {
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
            case Op::Jump: {
                int16_t off = read_i16(code, pc);
                pc += 2 + off;
                if (off < 0) Collector::safepoint();
                break;
            }
            case Op::JumpIfTrue: {
                int16_t off = read_i16(code, pc);
                pc += 2;
                if (acc.to_boolean()) {
                    pc += off;
                    if (off < 0) Collector::safepoint();
                }
                break;
            }
            case Op::JumpIfFalse: {
                int16_t off = read_i16(code, pc);
                pc += 2;
                if (!acc.to_boolean()) {
                    pc += off;
                    if (off < 0) Collector::safepoint();
                }
                break;
            }

            case Op::Return:
                return acc;

            case Op::Throw:
                ctx.throw_exception(acc, /*raw=*/true);
                CHECK_EXC();
                break;

            case Op::ReraiseGeneratorReturn:
                throw GeneratorReturnException(acc);

            default:
                ctx.throw_exception(Value(std::string("VM: invalid opcode")));
                return Value();
        }
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
            for (const auto& h : chunk.handlers) {
                if (h.genreturn_pc < 0) continue;
                if (instr_pc >= h.start_pc && instr_pc < h.end_pc) {
                    uint32_t width = h.end_pc - h.start_pc;
                    if (width < best_width) { best_width = width; genreturn_pc = h.genreturn_pc; }
                }
            }
            if (genreturn_pc < 0) throw;
            acc = gen_ret.return_value;
            pc = static_cast<uint32_t>(genreturn_pc);
            continue;
        } catch (const std::exception& e) {
            // A native call (e.g. Proxy invariant violation) threw a raw C++
            // exception; CHECK_EXC below routes it like a normal JS throw.
            if (!ctx.has_exception()) ctx.throw_exception(Value(std::string(e.what())));
        } catch (...) {
            if (!ctx.has_exception()) ctx.throw_exception(Value(std::string("Error: Unknown error")));
        }
        CHECK_EXC();
    }

#undef BINARY_OP
#undef CHECK_EXC
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
    ValueVectorRoot const_root(&chunk->constants);
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
