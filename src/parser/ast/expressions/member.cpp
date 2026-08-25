/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/parser/AST.h"
#include <span>
#include "quanta/core/engine/Context.h"
#include "quanta/core/engine/Engine.h"
#include "quanta/core/engine/CallStack.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/RegExp.h"
#include "quanta/core/runtime/Async.h"
#include "quanta/core/runtime/BigInt.h"
#include "quanta/core/runtime/Iterator.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/core/runtime/ProxyReflect.h"
#include "quanta/core/runtime/String.h"
#include "quanta/core/runtime/Math.h"
#include "../ast_internal.h"
#include <sstream>
#include <set>
#include <cmath>
#include <climits>
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <unordered_map>
#include <map>
#include <cstdlib>
#include <cstdio>

#ifdef __GNUC__
    #define LIKELY(x) __builtin_expect(!!(x), 1)
    #define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
    #define LIKELY(x) (x)
    #define UNLIKELY(x) (x)
#endif

namespace Quanta {

// ToPropertyKey with ctx: for objects calls JS toString (hint: "string"), then valueOf.
static std::string to_js_property_key(Context& ctx, const Value& val) {
    return val.to_property_key_strict(ctx);
}

static bool do_brand_check(Object* obj, Object* expected) {
    if (obj->is_function() && static_cast<Function*>(obj)->is_class_constructor()) {
        return obj == expected;
    }
    Object* expected_proto;
    if (expected->is_function() && static_cast<Function*>(expected)->is_class_constructor()) {
        Value pv = static_cast<Function*>(expected)->get_property("prototype");
        expected_proto = pv.is_object() ? pv.as_object() : nullptr;
    } else {
        expected_proto = expected;
    }
    if (!expected_proto) return false;
    Object* proto = obj;
    while (proto) {
        if (proto == expected_proto) return true;
        proto = proto->get_prototype();
    }
    return false;
}

bool private_brand_check(Context& ctx, Object* obj, const std::string& prop_name, bool require_exists) {
    (void)ctx;
    CallStack& cs = CallStack::instance();
    for (size_t i = cs.depth(); i > 0; --i) {
        Function* fn = cs.at(i - 1).function_ptr;
        if (!fn) continue;

        if (Object* brands = fn->private_brands()) {
            Value name_brand = brands->get_property(prop_name);
            if (name_brand.is_object() || name_brand.is_function()) {
                Object* expected = name_brand.is_function()
                    ? static_cast<Object*>(name_brand.as_function())
                    : name_brand.as_object();

                // Methods/getters/setters: PrivateBrandCheck. Derived classes get a
                // per-instance marker slot after super() (also correct under a
                // constructor return-override, where `this` has no prototype link
                // to the declaring class); base classes fall back to the
                // prototype-chain check.
                Value pm_names_val = fn->get_internal_slot("__private_method_names__");
                if (pm_names_val.is_object() &&
                    pm_names_val.as_object()->get_property(prop_name).to_boolean()) {
                    const std::string& pm_slot = fn->pm_brand_slot();
                    if (!pm_slot.empty()) {
                        return obj->has_private_slot(pm_slot);
                    }
                    return do_brand_check(obj, expected);
                }

                // Fields (and members without pm metadata): presence check via the
                // qualified key, which already encodes the declaring brand. Fields
                // sit on the receiver itself; base-class instance methods/accessors
                // sit on the declaring prototype, so instance brands also walk the
                // chain. Static brands (a constructor) never walk -- a subclass
                // constructor inheriting through it must fail the check.
                std::string qualified = prop_name + "@" + std::to_string(reinterpret_cast<uintptr_t>(expected));
                if (obj->has_private_slot(qualified)) return true;
                bool static_brand = expected->is_function() &&
                                    static_cast<Function*>(expected)->is_class_constructor();
                if (!static_brand) {
                    Object* p = obj->get_prototype();
                    while (p) { if (p->has_private_slot(qualified)) return true; p = p->get_prototype(); }
                }
                return false;
            }
        }

        Value brand_val = fn->get_internal_slot("__private_class_brand__");
        if (brand_val.is_object() || brand_val.is_function()) {
            Object* expected = brand_val.is_function()
                ? static_cast<Object*>(brand_val.as_function())
                : brand_val.as_object();
            if (!do_brand_check(obj, expected)) return false;
            if (!require_exists) return true;
            std::string qualified = resolve_private_storage_key(prop_name);
            bool found = obj->has_private_slot(qualified) || obj->has_private_slot(prop_name);
            if (!found) {
                Object* p = obj->get_prototype();
                while (p && !found) { if (p->has_private_slot(qualified) || p->has_private_slot(prop_name)) found = true; p = p->get_prototype(); }
            }
            return found;
        }
    }
    // No frame declares this name -- e.g. resumed after an await/yield. Fall back to the object's own qualified slot, if any (see resolve_private_storage_key).
    std::string qualified = resolve_private_storage_key(prop_name, obj);
    bool found = obj->has_private_slot(qualified) || obj->has_private_slot(prop_name);
    if (!found) {
        Object* p = obj->get_prototype();
        while (p && !found) { if (p->has_private_slot(qualified) || p->has_private_slot(prop_name)) found = true; p = p->get_prototype(); }
    }
    return found;
}

std::vector<Value> process_arguments_with_spread(const std::vector<std::unique_ptr<ASTNode>>& arguments, Context& ctx);

// Defined in ProxyReflect.cpp (OrdinarySet).
bool ordinary_set_with_receiver(Object* O, const std::string& key, const Value& value, Object* receiver, Context& ctx);

// GetSuperBase: the object `super.x` looks up on. Shared by the read path
// below, the write path in assignment.cpp and the compiled Op::GetSuper
// family, so all three agree on how __super__/__super_is_static__/
// __home_object__ are interpreted.
Object* resolve_super_base(Context& ctx) {
    // Spec: GetSuperBase() is [[HomeObject]].[[GetPrototypeOf]](). Reading the
    // prototype live also keeps a post-definition Object.setPrototypeOf on the
    // home object visible, which a cached parent constructor would miss.
    Value home = ctx.get_binding("__home_object__");
    if (!home.is_undefined() && !home.is_null()) {
        Object* home_obj = home.is_function() ? static_cast<Object*>(home.as_function())
                                              : home.as_object();
        return home_obj ? home_obj->get_prototype() : nullptr;
    }
    // No home object recorded (async methods, Proxy-wrapped calls): fall back to
    // the parent constructor that the call frame bound.
    Value super_ctor = ctx.get_binding("__super__");
    if (super_ctor.is_function()) {
        // Static method: super.x resolves on the parent constructor itself;
        // an instance method goes through its prototype.
        if (ctx.has_binding("__super_is_static__")) return super_ctor.as_function();
        Value proto_val = super_ctor.as_function()->get_property("prototype");
        return proto_val.is_object() ? proto_val.as_object() : nullptr;
    }
    Value this_val = ctx.get_binding("this");
    if (this_val.is_object_like()) {
        Object* this_obj = this_val.is_function()
            ? static_cast<Object*>(this_val.as_function()) : this_val.as_object();
        return this_obj ? this_obj->get_prototype() : nullptr;
    }
    return nullptr;
}

// super [[Get]] (ES2024 13.3.7.3) on an already-resolved base. Computed keys take
// this form because MakeSuperPropertyReference runs GetSuperBase before GetValue
// applies ToPropertyKey, and a key's toString() can mutate the prototype chain.
Value super_get_on(Context& ctx, Object* base, const std::string& prop_name) {
    if (!base) {
        // RequireObjectCoercible: null prototype base throws TypeError (spec 13.3.7.3 step 5)
        ctx.throw_type_error("Cannot read properties of null (reading super property)");
        return Value();
    }
    // For getter properties, invoke with current 'this' as receiver (spec super[[Get]])
    PropertyDescriptor desc = base->get_property_descriptor(prop_name);
    if (desc.is_accessor_descriptor() && desc.has_getter()) {
        Function* getter = as_function(desc.get_getter());
        if (getter) return getter->call(ctx, {}, ctx.get_binding("this"));
    }
    return base->get_property(prop_name);
}

// super.<name>: no key expression, so the base can be resolved here.
// Shared with Op::GetSuper.
Value super_get(Context& ctx, const std::string& prop_name) {
    if (ctx.this_needs_super()) {
        ctx.throw_reference_error("Must call super constructor before accessing 'this' in derived class constructor");
        return Value();
    }
    return super_get_on(ctx, resolve_super_base(ctx), prop_name);
}

// super [[Set]] (ES2024 13.3.7.4) on an already-resolved base: the lookup walks the
// super base's chain, but a setter found there runs with `this` as receiver and a
// plain write lands on `this`. That is exactly OrdinarySet(superBase, key, value, this).
void super_set_on(Context& ctx, Object* base, const std::string& prop_name, const Value& value) {
    if (!base) {
        ctx.throw_type_error("Cannot set properties of null (super property)");
        return;
    }
    Value this_val = ctx.get_binding("this");
    Object* receiver = this_val.is_function() ? static_cast<Object*>(this_val.as_function())
                     : this_val.is_object()   ? this_val.as_object()
                                              : nullptr;
    if (!receiver) {
        ctx.throw_type_error("Cannot set properties of a non-object super receiver");
        return;
    }
    bool ok = ordinary_set_with_receiver(base, prop_name, value, receiver, ctx);
    // PutValue only turns a failed [[Set]] into a TypeError in strict code; an
    // object-literal method holding super can be sloppy, where it stays silent.
    if (!ok && !ctx.has_exception() && ctx.is_strict_mode()) {
        ctx.throw_type_error("Cannot assign to read only property '" + prop_name + "' of super");
    }
}

// super.<name> = value. Shared with Op::SetSuper.
void super_set(Context& ctx, const std::string& prop_name, const Value& value) {
    if (ctx.this_needs_super()) {
        ctx.throw_reference_error("Must call super constructor before accessing 'this' in derived class constructor");
        return;
    }
    super_set_on(ctx, resolve_super_base(ctx), prop_name, value);
}


// A literal `.#name` read: the brand check, then the storage the name
// actually uses -- a field's qualified slot, or the declaring prototype's
// entry for a method or accessor. Returns true when the read is finished
// and `out` holds its value; false means only `prop_name` was resolved and
// the ordinary property read should carry on with it.
bool private_member_get(Context& ctx, Object* obj, const Value& object_value,
                        std::string& prop_name, Value& out) {
    if (!private_brand_check(ctx, obj, prop_name)) {
        ctx.throw_type_error("Cannot read private member " + prop_name + " from an object whose class did not declare it");
        out = Value(); return true;
    }
    // Fields live under a qualified key; accessors/methods live on the declaring
    // class's own prototype (not necessarily the closest "#name" in obj's chain).
    std::string qualified = resolve_private_storage_key(prop_name, obj);
    if (obj->has_private_slot(qualified)) {
        // Private slot access is fully raw: it never fires Proxy traps
        // or exotic overrides (e.g. a deferred namespace's evaluating
        // [[Get]]) -- spec: private state bypasses [[Get]] entirely.
        PropertyDescriptor own_d;
        if (obj->get_private_slot_descriptor(qualified, own_d) && own_d.is_accessor_descriptor()) {
            if (!own_d.has_getter()) {
                ctx.throw_type_error("'" + prop_name + "' accessor has no getter");
                out = Value(); return true;
            }
            Function* getter_fn = as_function(own_d.get_getter());
            if (getter_fn) { out = getter_fn->call(ctx, {}, object_value); return true; }
            out = Value(); return true;
        }
        { out = obj->get_private_slot_value(qualified); return true; }
    } else {
        Object* owner = resolve_private_accessor_owner(prop_name);
        if (owner) {
            // Methods/accessors are stored under the qualified key on
            // the declaring prototype/constructor (bare fallback for
            // paths resumed without a declaring frame).
            PropertyDescriptor d = owner->get_property_descriptor(qualified);
            if (!d.is_accessor_descriptor() && !d.has_value()) {
                d = owner->get_property_descriptor(prop_name);
            } else {
                prop_name = qualified;
            }
            if (d.is_accessor_descriptor()) {
                if (!d.has_getter()) {
                    ctx.throw_type_error("'" + prop_name + "' accessor has no getter");
                    out = Value(); return true;
                }
                Function* getter_fn = as_function(d.get_getter());
                if (getter_fn) { out = getter_fn->call(ctx, {}, object_value); return true; }
                out = Value(); return true;
            }
            // get_property(), not d.get_value(): a data field's value can live in
            // overflow storage while descriptors_ still holds a stale pre-write value.
            if (d.has_value()) { out = owner->get_property(prop_name); return true; }
        }
        // Fallback: no frame declared this name (e.g. resumed after await/yield).
        Object* lookup = obj;
        while (lookup) {
            PropertyDescriptor d = lookup->get_property_descriptor(qualified);
            if (!d.is_accessor_descriptor() && !d.has_value()) {
                d = lookup->get_property_descriptor(prop_name);
            } else {
                prop_name = qualified;
            }
            if (d.is_accessor_descriptor()) {
                if (!d.has_getter()) {
                    ctx.throw_type_error("'" + prop_name + "' accessor has no getter");
                    out = Value(); return true;
                }
                break;
            }
            if (d.has_value()) break;
            lookup = lookup->get_prototype();
        }
    }
    return false;
}



std::string MemberExpression::to_string() const {
    if (computed_) {
        return object_->to_string() + "[" + property_->to_string() + "]";
    } else {
        return object_->to_string() + "." + property_->to_string();
    }
}

std::unique_ptr<ASTNode> MemberExpression::clone() const {
    return std::make_unique<MemberExpression>(
        object_->clone(), property_->clone(), computed_, start_, end_
    );
}




std::string NewExpression::to_string() const {
    std::string result = "new " + constructor_->to_string() + "(";
    for (size_t i = 0; i < arguments_.size(); ++i) {
        if (i > 0) result += ", ";
        result += arguments_[i]->to_string();
    }
    result += ")";
    return result;
}

std::unique_ptr<ASTNode> NewExpression::clone() const {
    std::vector<std::unique_ptr<ASTNode>> cloned_args;
    for (const auto& arg : arguments_) {
        cloned_args.push_back(arg->clone());
    }
    return std::make_unique<NewExpression>(
        constructor_->clone(), std::move(cloned_args), start_, end_
    );
}



std::string MetaProperty::to_string() const {
    return meta_ + "." + property_;
}

std::unique_ptr<ASTNode> MetaProperty::clone() const {
    return std::make_unique<MetaProperty>(meta_, property_, start_, end_);
}






} // namespace Quanta
