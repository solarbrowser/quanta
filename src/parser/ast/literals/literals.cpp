/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/parser/AST.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/engine/Engine.h"
#include "quanta/core/gc/Collector.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/ProxyReflect.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/core/runtime/String.h"
#include <sstream>
#include <cmath>
#include <climits>

namespace Quanta {

// Defined in call.cpp: the single definition of what a spread expands to,
// shared with the tree-walker's argument marshalling and the VM's Op::SpreadInto.
void append_spread_values(Context&, const Value&, std::vector<Value>&);

static std::string literal_to_property_key(Context& ctx, const Value& val) {
    return val.to_property_key_strict(ctx);
}

// create_own_data_property's shortcut for an array-index-shaped key routes
// through set_element, whose storage answers "is index i present" by asking
// whether the stored value is undefined -- sound for a real hole, wrong for a
// property whose actual value IS undefined (`{...src}` where src has an index
// key holding undefined loses it from the spread target's own key list
// entirely). The spread target here is never an Array, so nothing needs that
// storage's other behavior; a defineProperty with W|E|C keeps the key on the
// shape/descriptor path that stores presence and value separately.
static void spread_create_own_data_property(Object* target, const std::string& key, const Value& value) {
    uint32_t index;
    if (target->is_array_index(key, &index)) {
        PropertyDescriptor desc(value, PropertyAttributes::Default);
        target->set_property_descriptor(key, desc);
        return;
    }
    target->create_own_data_property(key, value);
}

// The fast loop below spreads the same fixed sequence of keys -- straight
// off the source's own shape, in its own order -- into a target on the
// same starting shape every time an object literal's `{...src, ...}` runs
// in a loop over the same src: same (source shape, target's own starting
// shape) pair in, same resulting sequence of target shapes out, every
// time. One memoized slot answers it (not a small cache -- collapsing back
// to the same pair after a rarer one is the common case too), the same
// shortcut RegExp::exec's match result construction uses for a fixed
// sequence of properties instead of a variable one. Array-index-shaped
// keys route through set_property_descriptor above, not a shape
// transition at all, so the memo simply never forms when the source has
// one -- rare for an object literal's own spread, and the plain call below
// is still correct for it, just not memoized.
static thread_local Shape* spread_src_shape = nullptr;
static thread_local Shape* spread_target_from = nullptr;
static thread_local Shape* spread_target_after[Shape::kMaxSlots];
static thread_local uint32_t spread_memo_n = 0;

static void spread_fast_copy(Object* target, const std::string* const* names, const Value* vals,
                             uint32_t n, Shape* src_shape) {
    Shape* target_from = target->get_shape();
    if (src_shape == spread_src_shape && target_from == spread_target_from && spread_memo_n == n) {
        for (uint32_t i = 0; i < n; i++) {
            target->add_shape_property_cached(*names[i], vals[i], spread_target_after[i]);
        }
        return;
    }
    // Learning afresh invalidates whatever memo stood (a different key
    // sequence needs its own record) until this walk proves itself clean.
    spread_src_shape = nullptr;
    spread_target_from = target_from;
    spread_memo_n = 0;
    bool any_index = false;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx;
        if (target->is_array_index(*names[i], &idx)) any_index = true;
        spread_create_own_data_property(target, *names[i], vals[i]);
        if (!any_index && spread_memo_n < Shape::kMaxSlots) {
            spread_target_after[spread_memo_n++] = target->get_shape();
        }
    }
    if (!any_index && spread_memo_n == n) spread_src_shape = src_shape;
}

// One definition of what an object spread copies, shared by the tree-walker's
// ObjectLiteral::evaluate and the VM's Op::ObjectSpreadInto so the two cannot
// drift -- the same reason append_spread_values exists for the array and
// argument forms. Returns false with an exception pending on the context.
// CopyDataProperties makes each property outright (CreateDataProperty), which
// an assignment does not: [[Set]] walks the prototype chain, so a setter above
// the target -- Object.prototype's, most easily -- ran instead of the property
// being created, and the spread came back missing it. Object.assign is the one
// that really is specified with Set, and stays as it is.
bool object_spread_into(Context& ctx, Object* target, const Value& spread_value) {
        if (ctx.has_exception()) return false;

        // ES2018: null/undefined in object spread are silently ignored
        if (spread_value.is_null() || spread_value.is_undefined()) return true;
        if (!spread_value.is_object() && !spread_value.is_function() &&
                !spread_value.is_string() && !spread_value.is_number() && !spread_value.is_boolean()) {
            ctx.throw_type_error("Spread syntax can only be applied to objects");
            return false;
        }

        // Box primitives for spread
        Object* spread_obj = nullptr;
        if (spread_value.is_object()) {
            spread_obj = spread_value.as_object();
        } else if (spread_value.is_function()) {
            spread_obj = spread_value.as_function();
        } else if (spread_value.is_string()) {
            // A boxed string carries one own enumerable property per code
            // unit; a number or a boolean genuinely carries none, which is
            // why the branch below is right for them and was not for this.
            String* src = spread_value.as_string();
            const size_t units = src->utf16_length();
            for (size_t u = 0; u < units; u++) {
                const int32_t unit = src->code_unit_at(u);
                if (unit < 0) break;
                target->create_own_data_property(
                    std::to_string(u), Value(encode_utf16_unit(static_cast<uint32_t>(unit))));
                if (ctx.has_exception()) return false;
            }
            return true;
        } else {
            return true; // number and boolean have no enumerable own properties
        }
        if (!spread_obj) {
            ctx.throw_exception(Value(std::string("Error: Could not convert value to object")));
            return false;
        }

        try {
            if (spread_obj->get_type() == Object::ObjectType::Proxy) {
                // get_enumerable_keys()/get_property() don't know about Proxy traps, so go through ownKeys/getOwnPropertyDescriptor/get directly per spec.
                Proxy* proxy = static_cast<Proxy*>(spread_obj);
                for (const auto& prop_name : proxy->own_keys_trap()) {
                    // own_keys_trap() returns symbol keys as their "@@sym:" string encoding; decode back to the real Symbol so traps receive the original key, not its string form.
                    Symbol* sym = Symbol::find_by_property_key(prop_name);
                    Value key_value = sym ? Value(sym) : Value(prop_name);
                    PropertyDescriptor desc = proxy->get_own_property_descriptor_trap(key_value);
                    if (!desc.is_data_descriptor() && !desc.is_accessor_descriptor()) continue;
                    if (!desc.is_enumerable()) continue;
                    Value prop_value = proxy->get_trap(key_value);
                    spread_create_own_data_property(target, prop_name, prop_value);
                }
            } else {
                // Names and values both come off the source's shape, so the
                // key list is never materialized and no name is looked up
                // twice. An accessor gives the whole spread back to the
                // general path: its getter is user code that can move the
                // source to another shape or to dictionary mode while the
                // walk is still standing on this one.
                const std::string* fast_names[Shape::kMaxSlots];
                Value fast_vals[Shape::kMaxSlots];
                uint32_t fast_n = 0;
                bool bail = false;
                bool walked = spread_obj->for_each_own_enumerable_fast(
                    [&](const std::string& k, uint32_t slot, bool is_accessor) {
                        if (bail) return;
                        const Value* v = is_accessor ? nullptr
                                                     : spread_obj->get_shape_slot_unchecked(slot);
                        if (!v || fast_n >= Shape::kMaxSlots) { bail = true; return; }
                        fast_names[fast_n] = &k;
                        fast_vals[fast_n++] = *v;
                    }, /*include_symbols=*/true);
                if (walked && !bail) {
                    spread_fast_copy(target, fast_names, fast_vals, fast_n, spread_obj->get_shape());
                } else {
                    auto property_names = spread_obj->get_enumerable_keys();
                    for (const auto& prop_name : property_names) {
                        Value prop_value = spread_obj->get_property(prop_name);
                        spread_create_own_data_property(target, prop_name, prop_value);
                    }
                }
            }
        } catch (const std::exception& e) {
            ctx.throw_exception(Value("Error processing spread properties: " + std::string(e.what())));
            return false;
        }
    return true;
}



std::string ObjectLiteral::to_string() const {
    std::ostringstream oss;
    oss << "{";

    for (size_t i = 0; i < properties_.size(); ++i) {
        if (i > 0) oss << ", ";

        if (properties_[i]->key == nullptr && properties_[i]->value &&
            properties_[i]->value->get_type() == ASTNode::Type::SPREAD_ELEMENT) {
            oss << properties_[i]->value->to_string();
        } else {
            if (properties_[i]->computed) {
                oss << "[" << properties_[i]->key->to_string() << "]";
            } else {
                oss << properties_[i]->key->to_string();
            }

            oss << ": " << properties_[i]->value->to_string();
        }
    }

    oss << "}";
    return oss.str();
}

std::unique_ptr<ASTNode> ObjectLiteral::clone() const {
    std::vector<std::unique_ptr<Property>> cloned_properties;

    for (const auto& prop : properties_) {
        auto cloned_prop = std::make_unique<Property>(
            prop->key ? prop->key->clone() : nullptr,
            prop->value ? prop->value->clone() : nullptr,
            prop->computed,
            prop->type
        );
        cloned_prop->shorthand = prop->shorthand;
        cloned_properties.push_back(std::move(cloned_prop));
    }

    auto copy = std::make_unique<ObjectLiteral>(std::move(cloned_properties), start_, end_);
    copy->set_trailing_comma_after_rest(trailing_comma_after_rest_);
    return copy;
}




std::string ArrayLiteral::to_string() const {
    std::ostringstream oss;
    oss << "[";

    for (size_t i = 0; i < elements_.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << elements_[i]->to_string();
    }

    oss << "]";
    return oss.str();
}

std::unique_ptr<ASTNode> ArrayLiteral::clone() const {
    std::vector<std::unique_ptr<ASTNode>> cloned_elements;

    for (const auto& element : elements_) {
        cloned_elements.push_back(element->clone());
    }

    auto copy = std::make_unique<ArrayLiteral>(std::move(cloned_elements), start_, end_);
    copy->set_trailing_comma_after_spread(trailing_comma_after_spread_);
    return copy;
}

} // namespace Quanta
