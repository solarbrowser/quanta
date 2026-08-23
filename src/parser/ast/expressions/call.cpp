/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/parser/AST.h"
#include "quanta/core/gc/Collector.h"
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

// The single definition of what a spread expands to: call arguments, array
// literals and the VM's Op::SpreadInto all come through here, so no two can
// disagree. A plain Array is bulk-copied while the array-iterator protector
// holds; otherwise the full iterator protocol runs.
void append_spread_values(Context& ctx, const Value& spread_value, std::vector<Value>& arg_values) {
    {
            if (spread_value.is_object() || spread_value.is_function()) {
                Object* spread_obj = spread_value.is_function()
                    ? static_cast<Object*>(spread_value.as_function())
                    : spread_value.as_object();
                Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
                // A plain Array skips the protocol only while the protector
                // holds; once anything redefines array iteration (a replaced
                // Array.prototype[@@iterator], an own @@iterator on an
                // instance, or a patched %ArrayIteratorPrototype%.next) every
                // array falls back to the spec path, permanently.
                if (iter_sym && spread_obj->is_array() &&
                    Object::array_iterator_protector_intact()) {
                    uint32_t spread_length = spread_obj->get_length();
                    for (uint32_t j = 0; j < spread_length; ++j) {
                        arg_values.push_back(spread_obj->get_element(j));
                    }
                    return;
                }
                // Everything else is the protocol, and every way it can fail is
                // a TypeError. There used to be an array-like fallback here for
                // an object with no @@iterator, which quietly spread a plain
                // {length: n} and a {} instead of refusing them.
                Value iter_method = iter_sym
                    ? spread_obj->get_property(iter_sym->to_property_key())
                    : Value();
                if (ctx.has_exception()) return;
                if (iter_method.is_undefined() || iter_method.is_null()) {
                    ctx.throw_type_error("Spread syntax requires an iterable");
                    return;
                }
                if (!iter_method.is_function()) {
                    ctx.throw_type_error("Symbol.iterator is not callable");
                    return;
                }
                Value iter_obj = iter_method.as_function()->call(ctx, {}, spread_value);
                if (ctx.has_exception()) return;
                if (!iter_obj.is_object()) {
                    ctx.throw_type_error("Symbol.iterator must return an Object");
                    return;
                }
                Value next_fn = iter_obj.as_object()->get_property("next");
                if (ctx.has_exception()) return;
                if (!next_fn.is_function()) {
                    ctx.throw_type_error("Iterator has no callable next method");
                    return;
                }
                for (;;) {
                    Collector::safepoint();
                    Value res = next_fn.as_function()->call(ctx, {}, iter_obj);
                    if (ctx.has_exception()) return;
                    if (!res.is_object()) {
                        ctx.throw_type_error("Iterator result is not an object");
                        return;
                    }
                    if (res.as_object()->get_property("done").to_boolean()) break;
                    arg_values.push_back(res.as_object()->get_property("value"));
                }
            } else if (spread_value.is_string()) {
                const std::string& str = spread_value.as_string()->str();
                size_t i = 0;
                while (i < str.size()) {
                    unsigned char c = str[i];
                    size_t char_len = 1;
                    if (c >= 0xF0) char_len = 4;
                    else if (c >= 0xE0) char_len = 3;
                    else if (c >= 0xC0) char_len = 2;
                    std::string ch = str.substr(i, char_len);
                    arg_values.push_back(Value(ch));
                    i += char_len;
                }
            } else if (!spread_value.is_null() && !spread_value.is_undefined()) {
                // Non-iterable, non-null: TypeError
                ctx.throw_type_error("Spread syntax requires an iterable");
                return;
            } else {
                // null/undefined in call spread: TypeError (unlike object spread)
                ctx.throw_type_error("Spread syntax requires an iterable, got " +
                    std::string(spread_value.is_null() ? "null" : "undefined"));
                return;
            }
    }
}

std::vector<Value> process_arguments_with_spread(const std::vector<std::unique_ptr<ASTNode>>& arguments, Context& ctx) {
    std::vector<Value> arg_values;
    // Already-evaluated args sit in this vector's malloc'd storage while later
    // args (which may trigger GC) evaluate; keep it reachable meanwhile.
    ValueVectorRoot arg_root(&arg_values);

    for (const auto& arg : arguments) {
        if (arg->get_type() == ASTNode::Type::SPREAD_ELEMENT) {
            Value spread_value = static_cast<SpreadElement*>(arg.get())->get_argument()->evaluate(ctx);
            if (ctx.has_exception()) return arg_values;
            append_spread_values(ctx, spread_value, arg_values);
            if (ctx.has_exception()) return arg_values;
        } else {
            Value arg_value = arg->evaluate(ctx);
            if (ctx.has_exception()) return arg_values;
            arg_values.push_back(arg_value);
        }
    }

    return arg_values;
}

// SuperCall (13.3.7.1) plus the ConstructorEvaluation bookkeeping around it.
// Shared by the tree-walker below and Op::SuperCall. The caller evaluates the
// arguments and samples `super_already_called` beforehand, because BindThisValue's
// "called twice" check must see the state from before they ran (`super(super())`).
Value perform_super_call(Context& ctx, std::span<const Value> arg_values,
                         bool super_already_called) {
    // Note: NO already-called check here -- spec order for a second super()
    // is args evaluated, parent [[Construct]] runs again, and only then
    // BindThisValue throws (checked below, after the parent returns).

    // Check for class extends null -- super() always throws TypeError
    Value super_is_null = ctx.get_binding("__super_is_null__");
    if (super_is_null.to_boolean()) {
        ctx.throw_type_error("Super constructor is not a constructor");
        return Value();
    }

    Value parent_constructor = ctx.get_binding("__super__");


    if ((parent_constructor.is_undefined() && parent_constructor.is_function()) ||
        (parent_constructor.is_function() && parent_constructor.as_function() == nullptr)) {
        return Value();
    }

    if (parent_constructor.is_function()) {
        try {
            Function* parent_func = parent_constructor.as_function();
            if (!parent_func) {
                return Value();
            }

            // IsConstructor check: calling super() with a non-constructor throws TypeError
            if (!parent_func->is_constructor()) {
                ctx.throw_type_error("Super constructor is not a constructor");
                return Value();
            }

            Object* this_obj = ctx.get_this_binding();

            bool was_in_ctor = ctx.is_in_constructor_call();
            Value old_new_target = ctx.get_new_target();
            ctx.set_in_constructor_call(true);
            if (old_new_target.is_undefined()) {
                ctx.set_new_target(Value(static_cast<Object*>(parent_func)));
            }

            ctx.set_pending_construct_call(true);
            Value result;
            // call() and construct() still take a vector; a super call is not
            // a hot path, so the arguments are materialized here.
            const std::vector<Value> parent_args(arg_values.begin(), arg_values.end());
            // A default-ctor parent's own implicit super(...args) only runs via construct().
            if (!parent_func->is_native() && parent_func->is_default_ctor()) {
                result = parent_func->construct(ctx, parent_args);
            } else if (this_obj) {
                Value this_value(this_obj);
                result = parent_func->call(ctx, parent_args, this_value);
            } else {
                result = parent_func->call(ctx, parent_args);
            }
            ctx.clear_return_value();
            if (ctx.has_exception()) return Value();

            ctx.set_in_constructor_call(was_in_ctor);
            ctx.set_new_target(old_new_target);

            // BindThisValue on an already-initialized binding: a second
            // super() throws here, AFTER the parent ran -- `this` keeps
            // its first value and field initializers don't re-run.
            if (super_already_called) {
                ctx.throw_reference_error("Super constructor called twice");
                return Value();
            }

            ctx.set_super_called(true);
            ctx.set_this_needs_super(false);

            // If parent constructor explicitly returned an object, use that as new this.
            // Resolved BEFORE adding the private-method brand slot below: the slot must
            // land on whichever object actually ends up being `this` going forward, not
            // the pre-override allocation (which return-override may discard entirely).
            Object* final_this_obj = this_obj;
            bool returned_override = false;
            if ((result.is_object() || result.is_function()) && this_obj) {
                Object* new_this = result.as_object();
                if (new_this && new_this != this_obj) {
                    ctx.set_this_binding(new_this);
                    ctx.set_binding("this", result);
                    final_this_obj = new_this;
                    // Lets Function::construct tell a super-swapped `this`
                    // (needs the subclass prototype stomped for built-in
                    // supers) apart from an explicit `return obj` (returned
                    // untouched). Context-side identity only: a property
                    // marker would be observable through Proxy traps or a
                    // deferred module namespace's [[Get]].
                    ctx.set_last_super_override(new_this);
                }
                returned_override = true;
            }

            // InitializeInstanceElements: add per-instance private method brand slot.
            // This must happen after super() returns so that accessing private methods
            // before super() correctly throws TypeError (brand slot not yet present).
            {
                CallStack& pm_cs = CallStack::instance();
                if (!pm_cs.is_empty() && pm_cs.top().function_ptr) {
                    const std::string& pm_slot = pm_cs.top().function_ptr->pm_brand_slot();
                    if (!pm_slot.empty()) {
                        Object* pm_this = final_this_obj ? final_this_obj : ctx.get_this_binding();
                        if (pm_this) pm_this->add_private_field(pm_slot);
                        if (ctx.has_exception()) return Value();
                    }
                }
            }

            if (returned_override) {
                return result;
            }

            // Return the this value
            if (final_this_obj) {
                return Value(final_this_obj);
            }
            return Value();
        } catch (...) {
            return Value();
        }
    } else {
        return Value();
    }
}

// GetTemplateObject: one frozen object per template site, reused by every
// call through it, which is the whole reason a tag can compare identities.
// Shared with the compiler, which asks for it once and parks it in the
// chunk's constants rather than keeping the node to ask again.
Value get_template_object(TemplateLiteral* tmpl) {
    Value cached_strings = tmpl->cached_template_object();
    if (!cached_strings.is_object()) {
        const auto& elements = tmpl->get_elements();
        std::vector<std::string> cooked_parts;
        std::vector<std::string> raw_parts;
        for (const auto& el : elements) {
            if (el.type == TemplateLiteral::Element::Type::TEXT) {
                cooked_parts.push_back(el.text);
                raw_parts.push_back(el.raw_text);
            }
        }

        auto strings_obj = ObjectFactory::create_array(static_cast<int>(cooked_parts.size()));
        Object* strings_array = strings_obj.get();
        for (size_t i = 0; i < cooked_parts.size(); i++) {
            if (cooked_parts[i] == "\x01") {
                strings_array->set_property(std::to_string(i), Value());
            } else {
                strings_array->set_property(std::to_string(i), Value(cooked_parts[i]));
            }
        }
        strings_array->set_property("length", Value(static_cast<double>(cooked_parts.size())));

        auto raw_obj = ObjectFactory::create_array(static_cast<int>(raw_parts.size()));
        Object* raw_array = raw_obj.get();
        for (size_t i = 0; i < raw_parts.size(); i++) {
            raw_array->set_property(std::to_string(i), Value(raw_parts[i]));
        }
        raw_array->set_property("length", Value(static_cast<double>(raw_parts.size())));

        // The template object and its raw array are frozen (GetTemplateObject
        // step 11): a tag receives an object it cannot alter, which is what
        // lets one be shared across calls at all.
        raw_array->freeze();
        strings_array->set_property("raw", Value(raw_obj.release()));
        strings_array->freeze();

        cached_strings = Value(strings_obj.release());
        tmpl->cache_template_object(cached_strings);
    }
    return cached_strings;
}

// The argument list a tag receives: the site's frozen template object, then
// each substitution in source order.




std::string CallExpression::to_string() const {
    std::ostringstream oss;
    oss << callee_->to_string() << "(";
    for (size_t i = 0; i < arguments_.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << arguments_[i]->to_string();
    }
    oss << ")";
    return oss.str();
}

std::unique_ptr<ASTNode> CallExpression::clone() const {
    std::vector<std::unique_ptr<ASTNode>> cloned_args;
    for (const auto& arg : arguments_) {
        cloned_args.push_back(arg->clone());
    }
    auto cloned = std::make_unique<CallExpression>(callee_->clone(), std::move(cloned_args), start_, end_, is_optional_);
    cloned->set_tagged_template(is_tagged_template_);
    return cloned;
}



} // namespace Quanta
