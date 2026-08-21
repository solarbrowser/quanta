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
bool CallExpression::build_tagged_template_arguments(Context& ctx, std::vector<Value>& out) {
    TemplateLiteral* tmpl = static_cast<TemplateLiteral*>(arguments_[0].get());
    out.push_back(get_template_object(tmpl));
    for (const auto& el : tmpl->get_elements()) {
        if (el.type != TemplateLiteral::Element::Type::EXPRESSION) continue;
        Value expr_val = el.expression->evaluate(ctx);
        if (ctx.has_exception()) return false;
        out.push_back(expr_val);
    }
    return true;
}

Value CallExpression::evaluate(Context& ctx) {
    if (callee_->get_type() == ASTNode::Type::OPTIONAL_CHAINING_EXPRESSION) {
        OptionalChainingExpression* opt = static_cast<OptionalChainingExpression*>(callee_.get());
        // See g_optional_chain_shortcircuit's doc comment (ast_internal.h).
        // This reimplements the base evaluation inline (rather than calling
        // opt->evaluate()) since `base` is also needed as `this` below.
        if (!is_chain_link_type(opt->get_object()->get_type())) g_optional_chain_shortcircuit = false;
        Value base = opt->get_object()->evaluate(ctx);
        if (ctx.has_exception()) return Value();
        if (base.is_null() || base.is_undefined()) {
            g_optional_chain_shortcircuit = true;
            return Value();
        }
        g_optional_chain_shortcircuit = false;
        // base is non-null: get property and call with base as this
        std::string prop_name;
        if (opt->is_computed()) {
            Value prop_val = opt->get_property()->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            prop_name = prop_val.to_string();
        } else if (opt->get_property()->get_type() == ASTNode::Type::IDENTIFIER) {
            prop_name = static_cast<Identifier*>(opt->get_property())->get_name();
        }
        Value method_val;
        Object* base_obj = base.is_object() ? base.as_object()
                         : base.is_function() ? static_cast<Object*>(base.as_function())
                         : nullptr;
        if (base_obj) {
            // A private method lives in a slot on the declaring prototype, not
            // as an ordinary property, so the plain read finds nothing.
            Value pv;
            if (!prop_name.empty() && prop_name[0] == '#' &&
                private_member_get(ctx, base_obj, base, prop_name, pv)) {
                if (ctx.has_exception()) return Value();
                method_val = pv;
            } else {
                if (ctx.has_exception()) return Value();
                method_val = base_obj->get_property(prop_name);
            }
        }
        // `o?.g?.()`: the call's own `?.` still gets to short-circuit after the
        // base survived the chain's first one.
        if (is_optional_ && (method_val.is_null() || method_val.is_undefined())) {
            g_optional_chain_shortcircuit = true;
            return Value();
        }
        if (!method_val.is_function()) {
            ctx.throw_type_error(prop_name + " is not a function");
            return Value();
        }
        std::vector<Value> arg_values = process_arguments_with_spread(arguments_, ctx);
        if (ctx.has_exception()) return Value();
        return method_val.as_function()->call(ctx, arg_values, base);
    }

    if (is_optional_) {
        // See g_optional_chain_shortcircuit's doc comment (ast_internal.h).
        // `a?.()`'s own null-check is always a fresh short-circuit decision.
        if (!is_chain_link_type(callee_->get_type())) g_optional_chain_shortcircuit = false;
        Value callee_val = callee_->evaluate(ctx);
        if (ctx.has_exception()) return Value();
        if (callee_val.is_null() || callee_val.is_undefined()) {
            g_optional_chain_shortcircuit = true;
            return Value();
        }
        g_optional_chain_shortcircuit = false;
        if (callee_->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
            return handle_member_expression_call(ctx);
        }
        if (!callee_val.is_function()) {
            ctx.throw_type_error("is not a function");
            return Value();
        }
        std::vector<Value> arg_values = process_arguments_with_spread(arguments_, ctx);
        if (ctx.has_exception()) return Value();
        return callee_val.as_function()->call(ctx, arg_values);
    }

    // Tagged template calls with MemberExpression callees (e.g. String.raw`...`) must be
    // intercepted here before handle_member_expression_call takes over, since that path
    // uses process_arguments_with_spread which doesn't know about template literals.
    if (callee_->get_type() == ASTNode::Type::MEMBER_EXPRESSION && is_tagged_template_ &&
        arguments_.size() == 1 && arguments_[0]->get_type() == ASTNode::Type::TEMPLATE_LITERAL) {
        // A tag reached through a member expression is still a method call, so
        // the base is its `this`. Only the argument list differs, and the member
        // path takes that ready-made rather than walking one it cannot read.
        std::vector<Value> arg_values;
        if (!build_tagged_template_arguments(ctx, arg_values)) return Value();
        return handle_member_expression_call(ctx, &arg_values);
    } else if (callee_->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
        return handle_member_expression_call(ctx);
    }
    
    if (callee_->get_type() == ASTNode::Type::IDENTIFIER) {
        Identifier* identifier = static_cast<Identifier*>(callee_.get());
        if (identifier->get_name() == "super") {
            bool super_already_called = ctx.was_super_called();
            std::vector<Value> arg_values = process_arguments_with_spread(arguments_, ctx);
            if (ctx.has_exception()) return Value();
            // `super(super())`: the inner super() just ran as an argument.
            super_already_called = super_already_called || ctx.was_super_called();
            return perform_super_call(ctx, arg_values, super_already_called);
        }
    }

    Value callee_value = callee_->evaluate(ctx);
    if (ctx.has_exception()) return Value();

    if (callee_value.is_undefined() && callee_value.is_function()) {
        throw std::runtime_error("Invalid Value state: NaN-boxing corruption detected");
    }
    
    if (callee_value.is_function()) {
        // Tagged template literal handling
        if (is_tagged_template_ && arguments_.size() == 1 &&
            arguments_[0]->get_type() == ASTNode::Type::TEMPLATE_LITERAL) {

            std::vector<Value> arg_values;
            if (!build_tagged_template_arguments(ctx, arg_values)) return Value();

            Function* function = callee_value.as_function();
            Value this_value = Value();
            return function->call(ctx, arg_values, this_value);
        }

        std::vector<Value> arg_values = process_arguments_with_spread(arguments_, ctx);
        if (ctx.has_exception()) return Value();

        Function* function = callee_value.as_function();

        // In ES5, 'this' should be undefined for non-method calls
        // The function itself will convert to global object if not in strict mode
        Value this_value = Value();  // undefined

        // Direct eval detection: eval(code) called via plain Identifier lookup
        bool is_direct_eval = (callee_->get_type() == ASTNode::Type::IDENTIFIER &&
                               static_cast<Identifier*>(callee_.get())->get_name() == "eval");
        bool saved_direct_eval = ctx.is_direct_eval_call();
        if (is_direct_eval) ctx.set_direct_eval_call(true);
        Value result = function->call(ctx, arg_values, this_value);
        ctx.set_direct_eval_call(saved_direct_eval);
        return result;
    }

    if (callee_value.is_object() &&
        callee_value.as_object()->get_type() == Object::ObjectType::Proxy) {
        Proxy* proxy = static_cast<Proxy*>(callee_value.as_object());
        std::vector<Value> arg_values = process_arguments_with_spread(arguments_, ctx);
        if (ctx.has_exception()) return Value();
        return proxy->apply_trap(arg_values, Value());
    }

    if (callee_->get_type() == ASTNode::Type::IDENTIFIER) {
        Identifier* func_id = static_cast<Identifier*>(callee_.get());
        std::string func_name = func_id->get_name();

        if (false && func_name == "super") {
            Value super_constructor = ctx.get_binding("__super__");
            
            if (super_constructor.is_function()) {
                std::vector<Value> arg_values = process_arguments_with_spread(arguments_, ctx);
                if (ctx.has_exception()) return Value();
                
                Value this_value = ctx.get_binding("this");
                
                Function* parent_constructor = super_constructor.as_function();
                Value result = parent_constructor->call(ctx, arg_values, this_value);
                return result;
            } else {
                ctx.throw_exception(Value(std::string("super() called but no parent constructor found")));
                return Value();
            }
        }
        
        Value function_value = ctx.get_binding(func_name);

        if (function_value.is_function()) {
            std::vector<Value> arg_values = process_arguments_with_spread(arguments_, ctx);
            if (ctx.has_exception()) return Value();
            Function* func = function_value.as_function();
            return func->call(ctx, arg_values);
        } else if (function_value.is_object() &&
                   function_value.as_object()->get_type() == Object::ObjectType::Proxy) {
            Proxy* proxy = static_cast<Proxy*>(function_value.as_object());
            std::vector<Value> arg_values = process_arguments_with_spread(arguments_, ctx);
            if (ctx.has_exception()) return Value();
            return proxy->apply_trap(arg_values, Value());
        } else {
            ctx.throw_type_error(func_name + " is not a function");
            return Value();
        }
    }

    if (callee_->get_type() == ASTNode::Type::CALL_EXPRESSION) {
        Value callee_result = callee_->evaluate(ctx);
        if (ctx.has_exception()) return Value();
        
        
        if (callee_result.is_function()) {
            Function* func = callee_result.as_function();
            
            static thread_local int super_call_depth = 0;
            const int MAX_SUPER_DEPTH = 32;
            
            if (ctx.has_binding("__super__") && super_call_depth < MAX_SUPER_DEPTH) {
                Value super_constructor = ctx.get_binding("__super__");
                if (super_constructor.is_function() && super_constructor.as_function() == func) {

                    std::vector<Value> arg_values = process_arguments_with_spread(arguments_, ctx);
                    if (ctx.has_exception()) return Value();

                    Value this_value = ctx.get_binding("this");

                    super_call_depth++;
                    bool was_in_ctor = ctx.is_in_constructor_call();
                    Value old_new_target = ctx.get_new_target();
                    ctx.set_in_constructor_call(true);
                    if (old_new_target.is_undefined()) {
                        ctx.set_new_target(Value(static_cast<Object*>(func)));
                    }
                    ctx.set_pending_construct_call(true);
                    try {
                        Value result = func->call(ctx, arg_values, this_value);
                        super_call_depth--;
                        ctx.set_in_constructor_call(was_in_ctor);
                        ctx.set_new_target(old_new_target);
                        return result;
                    } catch (...) {
                        super_call_depth--;
                        ctx.set_in_constructor_call(was_in_ctor);
                        ctx.set_new_target(old_new_target);
                        throw;
                    }
                }
            }
            
            std::vector<Value> arg_values = process_arguments_with_spread(arguments_, ctx);
            if (ctx.has_exception()) return Value();
            
            return func->call(ctx, arg_values);
        }
    }
    
    ctx.throw_type_error(callee_->to_string() + " is not a function");
    return Value();
}

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

Value CallExpression::handle_array_method_call(Object* array, const std::string& method_name, Context& ctx) {
    if (method_name == "push") {
        for (const auto& arg : arguments_) {
            Value arg_value = arg->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            array->push(arg_value);
        }
        return Value(static_cast<double>(array->get_length()));
        
    } else if (method_name == "pop") {
        if (array->get_length() > 0) {
            return array->pop();
        } else {
            return Value();
        }
        
    } else if (method_name == "shift") {
        if (array->get_length() > 0) {
            return array->shift();
        } else {
            return Value();
        }
        
    } else if (method_name == "unshift") {
        for (const auto& arg : arguments_) {
            Value arg_value = arg->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            array->unshift(arg_value);
        }
        return Value(static_cast<double>(array->get_length()));
        
    } else if (method_name == "join") {
        std::string separator = ",";
        if (arguments_.size() > 0) {
            Value sep_value = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            separator = sep_value.to_string();
        }
        
        std::ostringstream result;
        uint32_t length = array->get_length();
        for (uint32_t i = 0; i < length; ++i) {
            if (i > 0) result << separator;
            Value element = array->get_element(i);
            if (!element.is_undefined() && !element.is_null()) {
                result << element.to_string();
            }
        }
        return Value(result.str());
        
    } else if (method_name == "indexOf") {
        if (arguments_.size() > 0) {
            Value search_value = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            uint32_t length = array->get_length();
            for (uint32_t i = 0; i < length; ++i) {
                Value element = array->get_element(i);
                if (element.strict_equals(search_value)) {
                    return Value(static_cast<double>(i));
                }
            }
        }
        return Value(-1.0);
        
    } else if (method_name == "map") {
        if (arguments_.size() > 0) {
            Value callback = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            if (callback.is_function()) {
                Function* callback_fn = callback.as_function();
                auto result_array = ObjectFactory::create_array(0);
                
                uint32_t length = array->get_length();
                for (uint32_t i = 0; i < length; ++i) {
                    Value element = array->get_element(i);
                    std::vector<Value> args = {element, Value(static_cast<double>(i)), Value(array)};
                    
                    Value mapped_value = callback_fn->call(ctx, args);
                    if (ctx.has_exception()) return Value();
                    
                    result_array->set_element(i, mapped_value);
                }
                return Value(result_array.release());
            } else {
                ctx.throw_exception(Value(std::string("Callback is not a function")));
                return Value();
            }
        } else {
            ctx.throw_exception(Value(std::string("Array.map requires a callback function")));
            return Value();
        }
        
    } else if (method_name == "filter") {
        if (arguments_.size() > 0) {
            Value callback = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            if (callback.is_function()) {
                Function* callback_fn = callback.as_function();
                auto result_array = ObjectFactory::create_array(0);
                uint32_t result_index = 0;
                
                uint32_t length = array->get_length();
                for (uint32_t i = 0; i < length; ++i) {
                    Value element = array->get_element(i);
                    std::vector<Value> args = {element, Value(static_cast<double>(i)), Value(array)};
                    
                    Value test_result = callback_fn->call(ctx, args);
                    if (ctx.has_exception()) return Value();
                    
                    if (test_result.to_boolean()) {
                        result_array->set_element(result_index++, element);
                    }
                }
                return Value(result_array.release());
            } else {
                ctx.throw_exception(Value(std::string("Callback is not a function")));
                return Value();
            }
        } else {
            ctx.throw_exception(Value(std::string("Array.filter requires a callback function")));
            return Value();
        }
        
    } else if (method_name == "reduce") {
        if (arguments_.size() > 0) {
            Value callback = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            if (callback.is_function()) {
                Function* callback_fn = callback.as_function();
                uint32_t length = array->get_length();
                
                if (length == 0 && arguments_.size() < 2) {
                    ctx.throw_exception(Value(std::string("Reduce of empty array with no initial value")));
                    return Value();
                }
                
                Value accumulator;
                uint32_t start_index = 0;
                
                if (arguments_.size() >= 2) {
                    accumulator = arguments_[1]->evaluate(ctx);
                    if (ctx.has_exception()) return Value();
                } else {
                    accumulator = array->get_element(0);
                    start_index = 1;
                }
                
                for (uint32_t i = start_index; i < length; ++i) {
                    Value element = array->get_element(i);
                    std::vector<Value> args = {accumulator, element, Value(static_cast<double>(i)), Value(array)};
                    
                    accumulator = callback_fn->call(ctx, args);
                    if (ctx.has_exception()) return Value();
                }
                
                return accumulator;
            } else {
                ctx.throw_exception(Value(std::string("Callback is not a function")));
                return Value();
            }
        } else {
            ctx.throw_exception(Value(std::string("Array.reduce requires a callback function")));
            return Value();
        }
        
    } else if (method_name == "forEach") {
        if (arguments_.size() > 0) {
            Value callback = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            if (callback.is_function()) {
                Function* callback_fn = callback.as_function();
                
                uint32_t length = array->get_length();
                for (uint32_t i = 0; i < length; ++i) {
                    Value element = array->get_element(i);
                    std::vector<Value> args = {element, Value(static_cast<double>(i)), Value(array)};
                    
                    callback_fn->call(ctx, args);
                    if (ctx.has_exception()) return Value();
                }
                
                return Value();
            } else {
                ctx.throw_exception(Value(std::string("Callback is not a function")));
                return Value();
            }
        } else {
            ctx.throw_exception(Value(std::string("Array.forEach requires a callback function")));
            return Value();
        }
        
    } else if (method_name == "slice") {
        uint32_t length = array->get_length();
        int32_t start = 0;
        int32_t end = static_cast<int32_t>(length);
        
        if (arguments_.size() > 0) {
            Value start_val = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            start = static_cast<int32_t>(start_val.to_number());
            if (start < 0) start = std::max(0, static_cast<int32_t>(length) + start);
            if (start >= static_cast<int32_t>(length)) start = length;
        }
        
        if (arguments_.size() > 1) {
            Value end_val = arguments_[1]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            end = static_cast<int32_t>(end_val.to_number());
            if (end < 0) end = std::max(0, static_cast<int32_t>(length) + end);
            if (end > static_cast<int32_t>(length)) end = length;
        }
        
        auto result_array = ObjectFactory::create_array(0);
        uint32_t result_index = 0;
        
        for (int32_t i = start; i < end; ++i) {
            Value element = array->get_element(static_cast<uint32_t>(i));
            result_array->set_element(result_index++, element);
        }
        
        return Value(result_array.release());
        
    } else if (method_name == "concat") {
        auto result_array = ObjectFactory::create_array(0);
        uint32_t result_index = 0;

        uint32_t length = array->get_length();
        for (uint32_t i = 0; i < length; ++i) {
            result_array->set_element(result_index++, array->get_element(i));
        }

        for (const auto& arg : arguments_) {
            Value arg_value = arg->evaluate(ctx);
            if (ctx.has_exception()) return Value();

            if (arg_value.is_object() && arg_value.as_object()->is_array()) {
                Object* arg_array = arg_value.as_object();
                uint32_t arg_length = arg_array->get_length();
                for (uint32_t i = 0; i < arg_length; ++i) {
                    result_array->set_element(result_index++, arg_array->get_element(i));
                }
            } else {
                result_array->set_element(result_index++, arg_value);
            }
        }

        result_array->set_length(result_index);
        return Value(result_array.release());
        
    } else if (method_name == "lastIndexOf") {
        if (arguments_.size() > 0) {
            Value search_value = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            uint32_t length = array->get_length();
            if (length == 0) return Value(-1.0);
            
            int32_t start_pos = static_cast<int32_t>(length) - 1;
            
            if (arguments_.size() > 1) {
                Value start_val = arguments_[1]->evaluate(ctx);
                if (ctx.has_exception()) return Value();
                start_pos = static_cast<int32_t>(start_val.to_number());
                if (start_pos < 0) {
                    start_pos = static_cast<int32_t>(length) + start_pos;
                    if (start_pos < 0) return Value(-1.0);
                }
                if (start_pos >= static_cast<int32_t>(length)) {
                    start_pos = static_cast<int32_t>(length) - 1;
                }
            }
            
            for (int32_t i = start_pos; i >= 0; --i) {
                Value element = array->get_element(static_cast<uint32_t>(i));
                if (element.strict_equals(search_value)) {
                    return Value(static_cast<double>(i));
                }
            }
        }
        return Value(-1.0);
        
    } else if (method_name == "reduceRight") {
        if (arguments_.size() > 0) {
            Value callback = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            if (callback.is_function()) {
                Function* callback_fn = callback.as_function();
                uint32_t length = array->get_length();
                
                if (length == 0 && arguments_.size() < 2) {
                    ctx.throw_exception(Value(std::string("ReduceRight of empty array with no initial value")));
                    return Value();
                }
                
                Value accumulator;
                int32_t start_index;
                
                if (arguments_.size() > 1) {
                    accumulator = arguments_[1]->evaluate(ctx);
                    if (ctx.has_exception()) return Value();
                    start_index = static_cast<int32_t>(length) - 1;
                } else {
                    if (length == 0) {
                        ctx.throw_exception(Value(std::string("ReduceRight of empty array with no initial value")));
                        return Value();
                    }
                    accumulator = array->get_element(length - 1);
                    start_index = static_cast<int32_t>(length) - 2;
                }
                
                for (int32_t i = start_index; i >= 0; --i) {
                    Value element = array->get_element(static_cast<uint32_t>(i));
                    std::vector<Value> args = {accumulator, element, Value(static_cast<double>(i)), Value(array)};
                    
                    accumulator = callback_fn->call(ctx, args);
                    if (ctx.has_exception()) return Value();
                }
                
                return accumulator;
            } else {
                ctx.throw_exception(Value(std::string("Callback is not a function")));
                return Value();
            }
        } else {
            ctx.throw_exception(Value(std::string("Array.reduceRight requires a callback function")));
            return Value();
        }
        
        
    } else if (method_name == "splice") {
        uint32_t length = array->get_length();

        if (arguments_.size() == 0) {
            // No arguments: return empty array, don't modify
            auto result_array = ObjectFactory::create_array(0);
            return Value(result_array.release());
        }

        int32_t start = 0;
        uint32_t delete_count = 0;

        Value start_val = arguments_[0]->evaluate(ctx);
        if (ctx.has_exception()) return Value();
        start = static_cast<int32_t>(start_val.to_number());
        if (start < 0) start = std::max(0, static_cast<int32_t>(length) + start);
        if (start >= static_cast<int32_t>(length)) start = length;

        if (arguments_.size() > 1) {
            Value delete_val = arguments_[1]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            delete_count = std::max(0, static_cast<int32_t>(delete_val.to_number()));
            delete_count = std::min(delete_count, length - static_cast<uint32_t>(start));
        } else {
            // Only start provided: delete to end
            delete_count = length - static_cast<uint32_t>(start);
        }
        
        auto result_array = ObjectFactory::create_array(0);
        for (uint32_t i = 0; i < delete_count; ++i) {
            result_array->set_element(i, array->get_element(static_cast<uint32_t>(start) + i));
        }
        
        for (uint32_t i = static_cast<uint32_t>(start) + delete_count; i < length; ++i) {
            array->set_element(static_cast<uint32_t>(start) + i - delete_count, array->get_element(i));
        }
        
        uint32_t new_length = length - delete_count;
        
        for (size_t i = 2; i < arguments_.size(); ++i) {
            Value new_val = arguments_[i]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            for (uint32_t j = new_length; j > static_cast<uint32_t>(start) + (i - 2); --j) {
                array->set_element(j, array->get_element(j - 1));
            }
            array->set_element(static_cast<uint32_t>(start) + (i - 2), new_val);
            new_length++;
        }
        
        array->set_property("length", Value(static_cast<double>(new_length)));
        
        return Value(result_array.release());
        
    } else if (method_name == "reverse") {
        uint32_t length = array->get_length();
        for (uint32_t i = 0; i < length / 2; ++i) {
            Value temp = array->get_element(i);
            array->set_element(i, array->get_element(length - 1 - i));
            array->set_element(length - 1 - i, temp);
        }
        return Value(array);
        
    } else if (method_name == "sort") {
        uint32_t length = array->get_length();
        if (length <= 1) return Value(array);
        
        Function* compareFn = nullptr;
        if (arguments_.size() > 0) {
            Value compare_val = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) {
                return Value();
            }
            if (compare_val.is_function()) {
                compareFn = compare_val.as_function();
            } else {
            }
        } else {
        }
        
        for (uint32_t i = 0; i < length - 1; ++i) {
            for (uint32_t j = 0; j < length - i - 1; ++j) {
                Value a = array->get_element(j);
                Value b = array->get_element(j + 1);
                
                bool should_swap = false;
                if (compareFn) {
                    std::vector<Value> args = {a, b};
                    Value result = compareFn->call(ctx, args);
                    if (ctx.has_exception()) return Value();
                    should_swap = result.to_number() > 0;
                } else {
                    should_swap = a.to_string() > b.to_string();
                }
                
                if (should_swap) {
                    array->set_element(j, b);
                    array->set_element(j + 1, a);
                }
            }
        }
        return Value(array);
        
    } else if (method_name == "find") {
        if (arguments_.size() > 0) {
            Value callback = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            if (callback.is_function()) {
                Function* callback_fn = callback.as_function();
                uint32_t length = array->get_length();
                
                for (uint32_t i = 0; i < length; ++i) {
                    Value element = array->get_element(i);
                    std::vector<Value> args = {element, Value(static_cast<double>(i)), Value(array)};
                    
                    Value result = callback_fn->call(ctx, args);
                    if (ctx.has_exception()) return Value();
                    
                    if (result.to_boolean()) {
                        return element;
                    }
                }
                return Value();
            } else {
                ctx.throw_exception(Value(std::string("Callback is not a function")));
                return Value();
            }
        } else {
            ctx.throw_exception(Value(std::string("Array.find requires a callback function")));
            return Value();
        }
        
    } else if (method_name == "findIndex") {
        if (arguments_.size() > 0) {
            Value callback = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            if (callback.is_function()) {
                Function* callback_fn = callback.as_function();
                uint32_t length = array->get_length();
                
                for (uint32_t i = 0; i < length; ++i) {
                    Value element = array->get_element(i);
                    std::vector<Value> args = {element, Value(static_cast<double>(i)), Value(array)};
                    
                    Value result = callback_fn->call(ctx, args);
                    if (ctx.has_exception()) return Value();
                    
                    if (result.to_boolean()) {
                        return Value(static_cast<double>(i));
                    }
                }
                return Value(-1.0);
            } else {
                ctx.throw_exception(Value(std::string("Callback is not a function")));
                return Value();
            }
        } else {
            ctx.throw_exception(Value(std::string("Array.findIndex requires a callback function")));
            return Value();
        }
        
    } else if (method_name == "some") {
        if (arguments_.size() > 0) {
            Value callback = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            if (callback.is_function()) {
                Function* callback_fn = callback.as_function();
                uint32_t length = array->get_length();
                
                for (uint32_t i = 0; i < length; ++i) {
                    Value element = array->get_element(i);
                    std::vector<Value> args = {element, Value(static_cast<double>(i)), Value(array)};
                    
                    Value result = callback_fn->call(ctx, args);
                    if (ctx.has_exception()) return Value();
                    
                    if (result.to_boolean()) {
                        return Value(true);
                    }
                }
                return Value(false);
            } else {
                ctx.throw_exception(Value(std::string("Callback is not a function")));
                return Value();
            }
        } else {
            ctx.throw_exception(Value(std::string("Array.some requires a callback function")));
            return Value();
        }
        
    } else if (method_name == "every") {
        if (arguments_.size() > 0) {
            Value callback = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            if (callback.is_function()) {
                Function* callback_fn = callback.as_function();
                uint32_t length = array->get_length();
                
                for (uint32_t i = 0; i < length; ++i) {
                    Value element = array->get_element(i);
                    std::vector<Value> args = {element, Value(static_cast<double>(i)), Value(array)};
                    
                    Value result = callback_fn->call(ctx, args);
                    if (ctx.has_exception()) return Value();
                    
                    if (!result.to_boolean()) {
                        return Value(false);
                    }
                }
                return Value(true);
            } else {
                ctx.throw_exception(Value(std::string("Callback is not a function")));
                return Value();
            }
        } else {
            ctx.throw_exception(Value(std::string("Array.every requires a callback function")));
            return Value();
        }
        
    } else if (method_name == "includes") {
        if (arguments_.size() > 0) {
            Value search_value = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();

            int64_t from_index = 0;
            if (arguments_.size() > 1) {
                Value start_val = arguments_[1]->evaluate(ctx);
                if (ctx.has_exception()) return Value();

                if (start_val.is_symbol()) {
                    ctx.throw_exception(Value(std::string("TypeError: Cannot convert a Symbol value to a number")));
                    return Value();
                }

                from_index = static_cast<int64_t>(start_val.to_number());
            }

            uint32_t length = array->get_length();

            if (from_index < 0) {
                from_index = static_cast<int64_t>(length) + from_index;
                if (from_index < 0) from_index = 0;
            }

            for (uint32_t i = static_cast<uint32_t>(from_index); i < length; ++i) {
                Value element = array->get_element(i);

                if (search_value.is_number() && element.is_number()) {
                    double search_num = search_value.to_number();
                    double element_num = element.to_number();

                    if (std::isnan(search_num) && std::isnan(element_num)) {
                        return Value(true);
                    }

                    if (search_num == element_num) {
                        return Value(true);
                    }
                } else if (element.strict_equals(search_value)) {
                    return Value(true);
                }
            }
        }
        return Value(false);
        
    } else {
        return Value();
    }
}

Value CallExpression::handle_member_expression_call(Context& ctx,
                                                    const std::vector<Value>* preset_args) {
    auto call_args = [&](Context& c) {
        return preset_args ? *preset_args : process_arguments_with_spread(arguments_, c);
    };
    MemberExpression* member = static_cast<MemberExpression*>(callee_.get());

    // ES6: super.method() - call parent prototype method with current this
    if (member->get_object()->get_type() == ASTNode::Type::IDENTIFIER &&
        static_cast<Identifier*>(member->get_object())->get_name() == "super") {
        Value method_value = member->evaluate(ctx);
        if (ctx.has_exception()) return Value();
        if (method_value.is_function()) {
            std::vector<Value> arg_values = call_args(ctx);
            if (ctx.has_exception()) return Value();
            Function* method = method_value.as_function();
            // this should be the current instance, not the parent constructor
            Object* this_obj = ctx.get_this_binding();
            return method->call(ctx, arg_values, Value(static_cast<Object*>(this_obj)));
        } else {
            ctx.throw_exception(Value(std::string("super." +
                (member->get_property()->get_type() == ASTNode::Type::IDENTIFIER
                    ? static_cast<Identifier*>(member->get_property())->get_name()
                    : std::string("method")) + " is not a function")));
            return Value();
        }
    }

    if (member->get_object()->get_type() == ASTNode::Type::IDENTIFIER &&
        member->get_property()->get_type() == ASTNode::Type::IDENTIFIER) {

        Identifier* obj = static_cast<Identifier*>(member->get_object());
        Identifier* prop = static_cast<Identifier*>(member->get_property());

        if (obj->get_name() == "console") {
            std::string method_name = prop->get_name();
            
            if (method_name == "log") {
                std::vector<Value> arg_values = call_args(ctx);
                if (ctx.has_exception()) return Value();
                
                for (size_t i = 0; i < arg_values.size(); ++i) {
                    if (i > 0) std::cout << " ";
                    try {
                        std::string str_val = arg_values[i].to_string();
                        std::cout << str_val;
                    } catch (...) {
                        std::cout << "[Error: Cannot convert value to string]";
                    }
                }
                std::cout << std::endl;
                std::cout.flush();

                return Value();
            }
        }
    }
    
    
    // See g_optional_chain_shortcircuit's doc comment (ast_internal.h). This
    // handles e.g. `a?.b.c()`: callee_ (`a?.b.c`) is a MemberExpression, not
    // itself an OptionalChainingExpression, so the earlier checks in
    // evaluate() don't catch it -- it lands here instead.
    if (!is_chain_link_type(member->get_object()->get_type())) g_optional_chain_shortcircuit = false;

    Value object_value = member->get_object()->evaluate(ctx);
    if (ctx.has_exception()) {
        return Value();
    }

    if (object_value.is_null() || object_value.is_undefined()) {
        bool short_circuited = g_optional_chain_shortcircuit;
        g_optional_chain_shortcircuit = false;
        if (short_circuited) {
            g_optional_chain_shortcircuit = true;
            return Value();
        }
        ctx.throw_type_error("Cannot read property of null or undefined");
        return Value();
    }
    g_optional_chain_shortcircuit = false;

    if (object_value.is_string()) {
        Value method_value = member->evaluate(ctx);
        if (ctx.has_exception()) return Value();
        std::vector<Value> arg_values = call_args(ctx);
        if (ctx.has_exception()) return Value();
        if (method_value.is_function()) {
            Function* method = method_value.as_function();
            return method->call(ctx, arg_values, object_value);
        }
        ctx.throw_type_error(member->to_string() + " is not a function");
        return Value();
        
    } else if (object_value.is_bigint()) {
        Value method_value = member->evaluate(ctx);
        if (ctx.has_exception()) return Value();
        std::vector<Value> arg_values = call_args(ctx);
        if (ctx.has_exception()) return Value();
        if (method_value.is_function()) {
            Function* method = method_value.as_function();
            return method->call(ctx, arg_values, object_value);
        } else {
            ctx.throw_type_error(member->to_string() + " is not a function");
            return Value();
        }

    } else if (object_value.is_number()) {
        Value method_value = member->evaluate(ctx);
        if (ctx.has_exception()) return Value();
        std::vector<Value> arg_values = call_args(ctx);
        if (ctx.has_exception()) return Value();
        if (method_value.is_function()) {
            Function* method = method_value.as_function();
            return method->call(ctx, arg_values, object_value);
        } else {
            ctx.throw_type_error(member->to_string() + " is not a function");
            return Value();
        }

    } else if (object_value.is_boolean()) {
        Value method_value = member->evaluate(ctx);
        if (ctx.has_exception()) return Value();
        std::vector<Value> arg_values = call_args(ctx);
        if (ctx.has_exception()) return Value();
        if (method_value.is_function()) {
            Function* method = method_value.as_function();
            return method->call(ctx, arg_values, object_value);
        } else {
            ctx.throw_type_error(member->to_string() + " is not a function");
            return Value();
        }

    } else if (object_value.is_symbol()) {
        Value method_value = member->evaluate(ctx);
        if (ctx.has_exception()) return Value();
        std::vector<Value> arg_values = call_args(ctx);
        if (ctx.has_exception()) return Value();
        if (method_value.is_function()) {
            return method_value.as_function()->call(ctx, arg_values, object_value);
        } else {
            ctx.throw_type_error(member->to_string() + " is not a function");
            return Value();
        }

    } else if (object_value.is_object() || object_value.is_function()) {
        Object* obj = object_value.is_object() ? object_value.as_object() : object_value.as_function();

        std::string method_name;
        if (member->is_computed()) {
            Value key_value = member->get_property()->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            method_name = key_value.to_property_key();
        } else {
            if (member->get_property()->get_type() == ASTNode::Type::IDENTIFIER) {
                Identifier* prop = static_cast<Identifier*>(member->get_property());
                method_name = prop->get_name();
            } else {
                ctx.throw_exception(Value(std::string("Invalid method name")));
                return Value();
            }
        }

        Object* private_method_owner = nullptr;
        std::string private_qualified;
        if (!member->is_computed() && !method_name.empty() && method_name[0] == '#') {
            if (!private_brand_check(ctx, obj, method_name)) {
                ctx.throw_type_error("Cannot read private member " + method_name + " from an object whose class did not declare it");
                return Value();
            }
            private_qualified = resolve_private_storage_key(method_name, obj);
            if (obj->has_private_slot(private_qualified)) {
                method_name = private_qualified;
            } else {
                // Private method: lives on the declaring class's own prototype,
                // under the qualified key (bare-name fallback for older paths).
                private_method_owner = resolve_private_accessor_owner(method_name);
            }
        }

        Value method_value;
        if (private_method_owner) {
            method_value = private_method_owner->get_private_slot_value(private_qualified);
            if (method_value.is_undefined()) method_value = private_method_owner->get_property(method_name);
        } else {
            // Resumed-async paths have no declaring frame: the qualified key
            // (recovered by resolve_private_storage_key's prototype scan) still
            // finds the method through the normal prototype-chain lookup.
            if (!private_qualified.empty() && private_qualified != method_name) {
                method_value = obj->get_property(private_qualified);
            }
            if (method_value.is_undefined()) method_value = obj->get_property(method_name);
        }
        if (ctx.has_exception()) return Value();

        std::vector<Value> arg_values = call_args(ctx);
        if (ctx.has_exception()) return Value();

        if (method_value.is_function()) {
            Function* method = method_value.as_function();
            return method->call(ctx, arg_values, object_value);
        } else if (method_value.is_object() &&
                   method_value.as_object()->get_type() == Object::ObjectType::Proxy) {
            Proxy* proxy = static_cast<Proxy*>(method_value.as_object());
            return proxy->apply_trap(arg_values, object_value);
        } else {
            ctx.throw_type_error(method_name.empty() ? "is not a function" : method_name + " is not a function");
            return Value();
        }
    }

    ctx.throw_exception(Value(std::string("Unsupported method call")));
    return Value();
}


} // namespace Quanta
