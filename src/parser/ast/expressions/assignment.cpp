/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/parser/AST.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/engine/Engine.h"
#include "quanta/core/engine/CallStack.h"
#include "quanta/core/gc/Collector.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/RegExp.h"
#include "quanta/core/runtime/Async.h"
#include "quanta/core/runtime/BigInt.h"
#include "quanta/core/runtime/Iterator.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/core/runtime/Generator.h"
#include "quanta/core/runtime/ProxyReflect.h"
#include "quanta/core/runtime/String.h"
#include "quanta/core/runtime/Math.h"
#include "quanta/core/runtime/TypedArray.h"
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

// Defined in member.cpp (GetSuperBase / super [[Set]]).
Object* resolve_super_base(Context&);
void super_set_on(Context&, Object*, const std::string&, const Value&);

static bool is_anonymous_function_def(const ASTNode* node);

Value AssignmentExpression::evaluate(Context& ctx) {
    // Declare right_value at function scope (will be evaluated at the right time)
    Value right_value;

    if (left_->get_type() == ASTNode::Type::IDENTIFIER) {
        Identifier* id = static_cast<Identifier*>(left_.get());
        std::string name = id->get_name();

        // ES5: Cannot assign to eval or arguments in strict mode
        if (ctx.is_strict_mode() && (name == "eval" || name == "arguments")) {
            ctx.throw_syntax_error("'" + name + "' cannot be assigned in strict mode");
            return Value();
        }

        // NamedEvaluation: static initializers observe the class name via
        // this.name during evaluation, so it must be inferred beforehand.
        if (operator_ == Operator::ASSIGN && !lhs_is_paren_ &&
            right_->get_type() == ASTNode::Type::CLASS_DECLARATION) {
            auto* cd = static_cast<ClassDeclaration*>(right_.get());
            if (cd->is_expression() && cd->get_id() && cd->get_id()->get_name().empty()) {
                cd->set_inferred_name(name);
            }
        }

        // For compound assignments, capture left value BEFORE evaluating right side.
        // Capture ref_env BEFORE get_binding because a getter may delete the property (object environment record: PutValue must write to the original env's binding object).
        Value left_value;
        Environment* ref_env = nullptr;
        if (operator_ != Operator::ASSIGN) {
            bool is_logical = operator_ == Operator::LOGICAL_AND_ASSIGN ||
                              operator_ == Operator::LOGICAL_OR_ASSIGN  ||
                              operator_ == Operator::NULLISH_ASSIGN;
            // Capture the binding env before GetValue (getter may delete the property).
            // find_binding_env checks @@unscopables exactly once; avoid has_binding/get_binding which would each re-check it and trigger extra Proxy traps.
            ref_env = ctx.find_binding_env(name);
            if (!ref_env) {
                ctx.throw_reference_error("'" + name + "' is not defined");
                return Value();
            }
            // GetValue on the left runs before the right side is evaluated, so
            // a binding still in its temporal dead zone throws here. put_value
            // repeats the check for the write, but by then the right side has
            // already run and its side effects have happened.
            if (ref_env && ref_env->binding_in_tdz(name)) {
                ctx.throw_reference_error("Cannot access '" + name + "' before initialization");
                return Value();
            }
            left_value = ref_env->get_binding_direct(name, &ctx);
            if (ctx.has_exception()) return Value();
        }

        // Logical assignment: short-circuit before evaluating RHS
        if (operator_ == Operator::LOGICAL_AND_ASSIGN ||
            operator_ == Operator::LOGICAL_OR_ASSIGN  ||
            operator_ == Operator::NULLISH_ASSIGN) {
            bool skip_assign =
                (operator_ == Operator::LOGICAL_AND_ASSIGN && !left_value.to_boolean()) ||
                (operator_ == Operator::LOGICAL_OR_ASSIGN  &&  left_value.to_boolean()) ||
                (operator_ == Operator::NULLISH_ASSIGN     && !left_value.is_null() && !left_value.is_undefined());
            if (skip_assign) return left_value;
            // Evaluate RHS with NamedEvaluation
            right_value = right_->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            if (right_value.is_function() && is_anonymous_function_def(right_.get())) {
                const std::string& fname = right_value.as_function()->get_name();
                if (fname.empty() || fname == "<arrow>") {
                    right_value.as_function()->set_name(name);
                }
            }
            // A logical assignment that reaches here is a real write, so a
            // refused one raises like `x = v` does. Every other assignment
            // form in this file already checks; this one did not.
            if (!ctx.set_binding(name, right_value) &&
                (ctx.is_strict_mode() || ctx.is_strict_const(name))) {
                ctx.throw_type_error("Assignment to constant variable '" + name + "'");
                return Value();
            }
            return right_value;
        }

        // For ASSIGN: capture ref_env before RHS evaluation (RHS may delete the binding).
        // A single find_binding_env call (not has_binding+find_binding_env) avoids double-firing a binding object's @@unscopables getter / Proxy traps, and;
        // avoids a second HasBinding racing against side effects from the first (e.g. the getter deleting the very property it's being looked up for).
        if (operator_ == Operator::ASSIGN) {
            ref_env = ctx.find_binding_env(name);
        }

        // Now evaluate right side
        right_value = right_->evaluate(ctx);
        if (ctx.has_exception()) {
            return Value();
        }

        // PutValue helper: for object env records (with scopes), write directly to the
        // binding object that was captured before GetValue - even if the property was
        // deleted by the getter.  Strict mode + deleted property -> ReferenceError.
        auto put_value = [&](const Value& val) {
            if (ref_env && ref_env->binding_in_tdz(name)) {
                ctx.throw_reference_error("Cannot access '" + name + "' before initialization");
                return;
            }
            if (ref_env && ref_env->get_type() == Environment::Type::Object &&
                ref_env->get_binding_object()) {
                Object* bobj = ref_env->get_binding_object();
                bool still_exists = bobj->has_own_property(name);
                if (!still_exists && ctx.is_strict_mode()) {
                    ctx.throw_reference_error("'" + name + "' is not defined");
                    return;
                }
                bool ok = bobj->set_property(name, val);
                if (!ok && (ctx.is_strict_mode() || ctx.is_strict_const(name))) {
                    ctx.throw_type_error("Assignment to constant variable '" + name + "'");
                }
            } else if (ref_env && ref_env->get_type() != Environment::Type::Object &&
                       ref_env->has_own_binding(name)) {
                // Write directly to the captured env (not the current chain) so that
                // eval-introduced inner bindings don't shadow the original reference.
                bool ok = ref_env->set_binding(name, val);
                if (!ok && (ctx.is_strict_mode() || ctx.is_strict_const(name))) {
                    ctx.throw_type_error("Assignment to constant variable '" + name + "'");
                }
            } else {
                bool ok = ctx.set_binding(name, val);
                if (!ok && (ctx.is_strict_mode() || ctx.is_strict_const(name))) {
                    ctx.throw_type_error("Assignment to constant variable '" + name + "'");
                }
            }
        };

        // ToPrimitive for compound += (spec 13.15.3 via 13.8.1)
        auto to_primitive_add = [&ctx](Value v) -> Value {
            if (!v.is_object()) return v;
            Object* obj = v.as_object();
            if (!obj) return v;
            Symbol* tp_sym = Symbol::get_well_known(Symbol::TO_PRIMITIVE);
            if (tp_sym) {
                Value tp = obj->get_property(tp_sym->to_property_key());
                if (tp.is_function()) {
                    Value r = tp.as_function()->call(ctx, {Value(std::string("default"))}, v);
                    if (!r.is_object()) return r;
                    return v;
                }
                // GetMethod: a present but non-callable @@toPrimitive is a TypeError.
                if (!tp.is_undefined() && !tp.is_null()) {
                    ctx.throw_type_error("Symbol.toPrimitive is not a function");
                    return Value();
                }
            }
            Value vof = obj->get_property("valueOf");
            if (vof.is_function()) {
                try {
                    Value r = vof.as_function()->call(ctx, {}, v);
                    if (!r.is_object()) return r;
                } catch (...) {}
            }
            Value ts = obj->get_property("toString");
            if (ts.is_function()) {
                try {
                    Value r = ts.as_function()->call(ctx, {}, v);
                    if (!r.is_object()) return r;
                } catch (...) {}
            }
            return v;
        };

        switch (operator_) {
            case Operator::ASSIGN: {
                if (ref_env && ref_env->binding_in_tdz(name)) {
                    ctx.throw_reference_error("Cannot access '" + name + "' before initialization");
                    return Value();
                }
                // SetFunctionName: x = (function(){}) -> x.name = 'x'
                // Spec: only when IsIdentifierRef(LHS) -- parenthesized LHS is not an IdentifierRef.
                if (!lhs_is_paren_ && right_value.is_function() && is_anonymous_function_def(right_.get())) {
                    const std::string& fname = right_value.as_function()->get_name();
                    if (fname.empty() || fname == "<arrow>") {
                        right_value.as_function()->set_name(name);
                    }
                }
                if (!ref_env) {
                    // Unresolvable reference (binding didn't exist when we captured ref_env)
                    if (ctx.is_strict_mode()) {
                        ctx.throw_reference_error("'" + name + "' is not defined");
                        return Value();
                    }
                    // ES5 8.7.2: PutValue on unresolvable reference -- set on global object (deletable)
                    Object* global = ctx.get_global_object();
                    if (global) global->set_property(name, right_value);
                } else if (ref_env->get_type() == Environment::Type::Object &&
                           ref_env->get_binding_object()) {
                    // Object Environment Record PutValue: always write to binding object
                    Object* bobj = ref_env->get_binding_object();
                    bool still_exists = bobj->has_own_property(name);
                    if (!still_exists && ctx.is_strict_mode()) {
                        ctx.throw_reference_error("'" + name + "' is not defined");
                        return Value();
                    }
                    bool ok = bobj->set_property(name, right_value);
                    if (!ok && (ctx.is_strict_mode() || ctx.is_strict_const(name))) {
                        ctx.throw_type_error("Assignment to constant variable '" + name + "'");
                        return Value();
                    }
                } else {
                    // Write to the captured ref_env directly (not a fresh ctx.set_binding chain walk), so a closer same-named binding the RHS introduced (e.g. via eval) doesn't hijack a write meant for the originally resolved reference.
                    bool success = ref_env->set_binding(name, right_value);
                    if (!success) {
                        if (ctx.is_strict_mode() || ctx.is_strict_const(name)) {
                            ctx.throw_type_error("Assignment to constant variable '" + name + "'");
                            return Value();
                        }
                    }
                }
                return right_value;
            }
            default: break;
        }

        // Every compound operator applies the same ApplyStringOrNumericBinary
        // Operator the plain binary form does, so it delegates rather than
        // reimplementing: the hand-written versions here reached for
        // to_number() and disagreed with `a - b` on BigInt, both in what they
        // threw and in what they said.
        BinaryExpression::Operator bin_op;
        switch (operator_) {
            case Operator::PLUS_ASSIGN:   bin_op = BinaryExpression::Operator::ADD; break;
            case Operator::MINUS_ASSIGN:  bin_op = BinaryExpression::Operator::SUBTRACT; break;
            case Operator::MUL_ASSIGN:    bin_op = BinaryExpression::Operator::MULTIPLY; break;
            case Operator::DIV_ASSIGN:    bin_op = BinaryExpression::Operator::DIVIDE; break;
            case Operator::MOD_ASSIGN:    bin_op = BinaryExpression::Operator::MODULO; break;
            case Operator::BITWISE_AND_ASSIGN: bin_op = BinaryExpression::Operator::BITWISE_AND; break;
            case Operator::BITWISE_OR_ASSIGN:  bin_op = BinaryExpression::Operator::BITWISE_OR; break;
            case Operator::BITWISE_XOR_ASSIGN: bin_op = BinaryExpression::Operator::BITWISE_XOR; break;
            case Operator::LEFT_SHIFT_ASSIGN:  bin_op = BinaryExpression::Operator::LEFT_SHIFT; break;
            case Operator::RIGHT_SHIFT_ASSIGN: bin_op = BinaryExpression::Operator::RIGHT_SHIFT; break;
            case Operator::UNSIGNED_RIGHT_SHIFT_ASSIGN:
                bin_op = BinaryExpression::Operator::UNSIGNED_RIGHT_SHIFT; break;
            default:
                ctx.throw_exception(Value(std::string("Unsupported assignment operator")));
                return Value();
        }
        {
            Value result = BinaryExpression::apply_operator(ctx, bin_op, left_value, right_value);
            if (ctx.has_exception()) return Value();
            put_value(result); if (ctx.has_exception()) return Value();
            return result;
        }
        
        return right_value;
    }
    
    if (left_->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
        MemberExpression* member = static_cast<MemberExpression*>(left_.get());

        // ES6: super.prop = val -- write to 'this', not to super
        bool is_super_assignment = (member->get_object()->get_type() == ASTNode::Type::IDENTIFIER &&
            static_cast<Identifier*>(member->get_object())->get_name() == "super");

        // Spec: evaluate base, then key expression, then RHS, then ToPropertyKey
        Value object_value = member->get_object()->evaluate(ctx);
        if (ctx.has_exception()) return Value();

        // For super.x = val, the write always goes to 'this', not to the super prototype.
        // The object_value (super prototype) is only used for setter lookup.
        Value write_target;
        // GetSuperBase, resolved before the key expression evaluates (its ToPropertyKey can have
        // side effects). __super__ may be a non-function sentinel for object-literal methods.
        Object* super_lookup_proto = nullptr;
        if (is_super_assignment) {
            write_target = ctx.get_binding("this");
            super_lookup_proto = resolve_super_base(ctx);
        }

        // Evaluate key expression once (before RHS per spec)
        Value computed_key_value;
        if (member->is_computed()) {
            computed_key_value = member->get_property()->evaluate(ctx);
            if (ctx.has_exception()) return Value();
        }

        // Logical assignment: get current value, short-circuit before evaluating RHS
        if (operator_ == Operator::LOGICAL_AND_ASSIGN ||
            operator_ == Operator::LOGICAL_OR_ASSIGN  ||
            operator_ == Operator::NULLISH_ASSIGN) {
            // Resolve property name from already-evaluated key
            std::string lprop;
            if (member->is_computed()) {
                if (computed_key_value.is_symbol())
                    lprop = computed_key_value.as_symbol()->to_property_key();
                else
                    lprop = computed_key_value.to_string();
            } else if (member->get_property()->get_type() == ASTNode::Type::IDENTIFIER) {
                lprop = static_cast<Identifier*>(member->get_property())->get_name();
            }
            // Spec: GetValue(ref) -> ToObject(base) throws TypeError for null/undefined.
            // For super, the read side uses the resolved super base, not 'this'.
            Object* lobj;
            if (is_super_assignment) {
                if (!super_lookup_proto) {
                    ctx.throw_type_error("Cannot read properties of null (reading super property)");
                    return Value();
                }
                lobj = super_lookup_proto;
            } else {
                if (object_value.is_null() || object_value.is_undefined()) {
                    ctx.throw_type_error("Cannot read property of null or undefined");
                    return Value();
                }
                lobj = object_value.is_object() ? object_value.as_object()
                     : object_value.is_function() ? static_cast<Object*>(object_value.as_function())
                     : nullptr;
            }
            // Private references: fields live under the qualified key on the
            // instance; methods/accessors live (qualified) on the declaring
            // prototype/constructor and read/write through getter/setter.
            Object* priv_owner = nullptr;
            PropertyDescriptor priv_desc;
            if (lobj && !member->is_computed() && !lprop.empty() && lprop[0] == '#') {
                std::string qualified = resolve_private_storage_key(lprop, lobj);
                if (lobj->has_private_slot(qualified)) {
                    lprop = qualified;
                } else {
                    priv_owner = resolve_private_accessor_owner(lprop);
                    if (!priv_owner) {
                        for (Object* p = lobj->get_prototype(); p; p = p->get_prototype()) {
                            if (p->has_private_slot(qualified)) { priv_owner = p; break; }
                        }
                    }
                    if (priv_owner) {
                        priv_desc = priv_owner->get_property_descriptor(qualified);
                        if (!priv_desc.is_accessor_descriptor() && !priv_desc.has_value()) {
                            priv_desc = priv_owner->get_property_descriptor(lprop);
                        }
                    }
                }
            }
            Value cur;
            if (priv_owner && priv_desc.is_accessor_descriptor()) {
                if (!priv_desc.has_getter()) {
                    ctx.throw_type_error("'" + lprop + "' accessor has no getter");
                    return Value();
                }
                Function* getter_fn = as_function(priv_desc.get_getter());
                cur = getter_fn ? getter_fn->call(ctx, {}, object_value) : Value();
            } else if (priv_owner && priv_desc.has_value()) {
                cur = priv_desc.get_value();
            } else {
                cur = lobj ? lobj->get_property(lprop) : Value();
            }
            if (ctx.has_exception()) return Value();
            bool skip =
                (operator_ == Operator::LOGICAL_AND_ASSIGN && !cur.to_boolean()) ||
                (operator_ == Operator::LOGICAL_OR_ASSIGN  &&  cur.to_boolean()) ||
                (operator_ == Operator::NULLISH_ASSIGN     && !cur.is_null() && !cur.is_undefined());
            if (skip) return cur;
            right_value = right_->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            if (priv_owner) {
                if (priv_desc.is_accessor_descriptor()) {
                    if (!priv_desc.has_setter()) {
                        ctx.throw_type_error("'" + lprop + "' was defined without a setter");
                        return Value();
                    }
                    Function* setter_fn = as_function(priv_desc.get_setter());
                    if (setter_fn) setter_fn->call(ctx, {right_value}, object_value);
                    if (ctx.has_exception()) return Value();
                    return right_value;
                }
                ctx.throw_type_error("'" + lprop + "' is a private method and cannot be assigned to");
                return Value();
            }
            // Private field slot: raw write (see the compound path below).
            if (lobj && !lprop.empty() && lprop[0] == '#' && lobj->has_private_slot(lprop)) {
                lobj->set_private_slot_value(lprop, right_value);
                return right_value;
            }
            if (is_super_assignment) {
                super_set_on(ctx, super_lookup_proto, lprop, right_value);
                if (ctx.has_exception()) return Value();
            } else if (lobj) {
                bool ok = lobj->ordinary_set(lprop, right_value);
                if (!ok && ctx.is_strict_mode()) {
                    ctx.throw_type_error("Cannot assign to read only property '" + lprop + "'");
                    return Value();
                }
            }
            return right_value;
        }

        // Spec: for compound operators GetValue(lref) happens before RHS eval.
        // This means CheckObjectCoercible(base) and ToPropertyKey(key) must happen first.
        if (operator_ != Operator::ASSIGN) {
            if (is_super_assignment) {
                if (!super_lookup_proto) {
                    ctx.throw_type_error("Cannot read properties of null (reading super property)");
                    return Value();
                }
            } else if (object_value.is_null() || object_value.is_undefined()) {
                ctx.throw_type_error(std::string("Cannot read properties of ") +
                    (object_value.is_null() ? "null" : "undefined"));
                return Value();
            }
            if (member->is_computed() && !computed_key_value.is_symbol() &&
                (computed_key_value.is_object() || computed_key_value.is_function())) {
                Object* pobj = computed_key_value.is_function()
                    ? static_cast<Object*>(computed_key_value.as_function())
                    : computed_key_value.as_object();
                Value ts = pobj ? pobj->get_property("toString") : Value();
                if (ts.is_function()) {
                    Value str_result = ts.as_function()->call(ctx, {}, computed_key_value);
                    if (ctx.has_exception()) return Value();
                    if (!str_result.is_object() && !str_result.is_function())
                        computed_key_value = str_result;
                }
            }
        }

        right_value = right_->evaluate(ctx);
        if (ctx.has_exception()) return Value();

        std::string str_value = object_value.is_string() ? object_value.to_string() : "";
        if (str_value.length() >= 6 && str_value.substr(0, 6) == "ARRAY:" && member->is_computed()) {
            int index = static_cast<int>(computed_key_value.to_number());
            if (index >= 0) {
                std::string array_content = str_value.substr(6);
                array_content = array_content.substr(1, array_content.length() - 2);
                
                std::vector<std::string> elements;
                if (!array_content.empty()) {
                    std::stringstream ss(array_content);
                    std::string item;
                    while (std::getline(ss, item, ',')) {
                        elements.push_back(item);
                    }
                }
                
                while (static_cast<int>(elements.size()) <= index) {
                    elements.push_back("undefined");
                }
                
                std::string value_str = right_value.to_string();
                if (right_value.is_number()) {
                    value_str = std::to_string(right_value.as_number());
                } else if (right_value.is_boolean()) {
                    value_str = right_value.as_boolean() ? "true" : "false";
                } else if (right_value.is_null()) {
                    value_str = "null";
                }
                elements[index] = value_str;
                
                std::string new_array = "ARRAY:[";
                for (size_t i = 0; i < elements.size(); ++i) {
                    if (i > 0) new_array += ",";
                    new_array += elements[i];
                }
                new_array += "]";
                
                if (member->get_object()->get_type() == ASTNode::Type::IDENTIFIER) {
                    Identifier* array_id = static_cast<Identifier*>(member->get_object());
                    ctx.set_binding(array_id->get_name(), Value(new_array));
                }
                
                return right_value;
            }
        }
        
        Object* obj = nullptr;
        bool is_string_object = false;

        // Stores the final value. A super write is not an ordinary write to `this`:
        // the lookup walks the super base's chain, so a setter or a read-only data
        // property up there still governs the result (spec super [[Set]]).
        auto store = [&](const std::string& key, const Value& val) -> bool {
            if (is_super_assignment) {
                super_set_on(ctx, super_lookup_proto, key, val);
                return !ctx.has_exception();
            }
            if (!obj) return true;
            if (obj->ordinary_set(key, val)) return true;
            if (ctx.has_exception()) return false;
            if (ctx.is_strict_mode()) {
                ctx.throw_type_error("Cannot assign to read only property '" + key + "'");
                return false;
            }
            return true;
        };

        // For super.x = val: write to 'this', not to super's prototype
        Value effective_object = is_super_assignment ? write_target : object_value;

        // PutValue step 5a: ToObject(V.[[Base]]) throws TypeError for null/undefined. For super,
        // [[Base]] is the super base (super_lookup_proto), not 'this' -- 'this' can be perfectly
        // valid while the super base is null (e.g. Object.setPrototypeOf(HomeObject, null)).
        if (is_super_assignment) {
            if (!super_lookup_proto) {
                ctx.throw_type_error("Cannot set properties of null (super property)");
                return Value();
            }
        } else if (effective_object.is_null() || effective_object.is_undefined()) {
            ctx.throw_type_error(std::string("Cannot set properties of ") +
                (effective_object.is_null() ? "null" : "undefined"));
            return Value();
        }

        if (effective_object.is_object()) {
            obj = effective_object.as_object();
        } else if (effective_object.is_function()) {
            obj = effective_object.as_function();
        } else if (effective_object.is_string() || effective_object.is_number() || effective_object.is_boolean() || effective_object.is_symbol()) {
            std::string str_val = effective_object.is_string() ? effective_object.to_string() : "";
            if (effective_object.is_string() && str_val.length() >= 7 && str_val.substr(0, 7) == "OBJECT:") {
                is_string_object = true;
            } else {
                // ES5: Check for accessor setter on prototype before failing
                std::string ctor_name = effective_object.is_string() ? "String" :
                    (effective_object.is_number() ? "Number" :
                    (effective_object.is_boolean() ? "Boolean" : "Symbol"));
                std::string prop_name;
                if (member->is_computed()) {
                    if (computed_key_value.is_symbol()) {
                        prop_name = computed_key_value.as_symbol()->to_property_key();
                    } else {
                        prop_name = computed_key_value.to_string();
                    }
                } else if (member->get_property()->get_type() == ASTNode::Type::IDENTIFIER) {
                    prop_name = static_cast<Identifier*>(member->get_property())->get_name();
                }
                if (!prop_name.empty()) {
                    Value ctor = ctx.get_binding(ctor_name);
                    if (ctor.is_function()) {
                        Value proto = ctor.as_function()->get_property("prototype");
                        // Walk the whole prototype chain (not just XPrototype) for an inherited
                        // setter/Proxy trap, passing the original primitive as receiver.
                        Object* level = proto.is_object() ? proto.as_object() : nullptr;
                        while (level) {
                            if (level->get_type() == Object::ObjectType::Proxy) {
                                static_cast<Proxy*>(level)->set_trap(Value(prop_name), right_value, object_value);
                                if (ctx.has_exception()) return Value();
                                return right_value;
                            }
                            PropertyDescriptor desc = level->get_property_descriptor(prop_name);
                            if (desc.is_accessor_descriptor()) {
                                if (desc.has_setter()) {
                                    Function* setter = as_function(desc.get_setter());
                                    if (setter) setter->call(ctx, {right_value}, object_value);
                                }
                                return right_value;
                            }
                            if (desc.has_value()) break; // non-writable (or shadowed) data property: stop, fall through to no-op/throw below
                            level = level->get_prototype();
                        }
                    }
                }
                // No setter found - silently fail or throw in strict mode
                if (ctx.is_strict_mode()) {
                    ctx.throw_type_error("Cannot set property on primitive");
                }
                return right_value;
            }
        } else {
            // ES1: In non-strict mode, setting property on primitive fails silently
            if (ctx.is_strict_mode()) {
                ctx.throw_type_error("Cannot set property on non-object");
            }
            return right_value;
        }
        
        // Plain `=` only. The shortcut stores the right-hand value as it stands,
        // which for `a[i] += x` is the operand rather than the result: the read
        // and the operator both belong to the general path below.
        if (operator_ == Operator::ASSIGN && member->is_computed() && obj && obj->is_array()) {
            Value prop_value = computed_key_value;

            if (__builtin_expect(prop_value.is_number(), 1)) {
                double idx_double = prop_value.as_number();
                if (__builtin_expect(idx_double >= 0 && idx_double <= 4294967294.0 && idx_double == std::floor(idx_double), 1)) {
                    uint32_t index = static_cast<uint32_t>(idx_double);
                    // If index is unowned and a Proxy is the prototype, delegate so the "set" trap fires with the right receiver.
                    std::string index_key = std::to_string(index);
                    // An index the array does not own is not this array's to
                    // create on its own: [[Set]] consults the whole prototype
                    // chain first, where an accessor has to run and a
                    // non-writable data property has to block. Listing the
                    // exotic prototypes to step around (Proxy, typed array)
                    // missed the ordinary ones. ordinary_set is what the
                    // compiled path calls and it covers all of them.
                    bool ok = obj->has_own_property(index_key)
                        // Owned: set_property, so an own accessor descriptor still wins.
                        ? obj->set_property(index_key, right_value)
                        : obj->ordinary_set(index_key, right_value);
                    if (ctx.has_exception()) return Value();
                    if (!ok && ctx.is_strict_mode()) {
                        ctx.throw_type_error("Cannot assign to read only property '" + index_key + "'");
                        return Value();
                    }
                    return right_value;
                }
            }
        }

        std::string prop_name;
        if (member->is_computed()) {
            // Use pre-evaluated key value (evaluated before RHS per spec)
            Value prop_value = computed_key_value;
            if (prop_value.is_symbol()) {
                prop_name = prop_value.as_symbol()->to_property_key();
            } else if (prop_value.is_object() || prop_value.is_function()) {
                // ToPropertyKey uses ToPrimitive(hint="string"): toString first, then valueOf
                Object* pobj = prop_value.is_function()
                    ? static_cast<Object*>(prop_value.as_function())
                    : prop_value.as_object();
                Value ts = pobj ? pobj->get_property("toString") : Value();
                bool resolved = false;
                if (!ctx.has_exception() && ts.is_function()) {
                    Value prim = ts.as_function()->call(ctx, {}, prop_value);
                    if (ctx.has_exception()) return Value();
                    if (!prim.is_object() && !prim.is_function()) {
                        prop_name = prim.to_string();
                        resolved = true;
                    }
                }
                if (ctx.has_exception()) return Value();
                if (!resolved) {
                    Value vo = pobj ? pobj->get_property("valueOf") : Value();
                    if (!ctx.has_exception() && vo.is_function()) {
                        Value prim = vo.as_function()->call(ctx, {}, prop_value);
                        if (ctx.has_exception()) return Value();
                        if (prim.is_symbol()) {
                            ctx.throw_type_error("Cannot convert a Symbol value to a string");
                            return Value();
                        }
                        prop_name = prim.is_object() ? prop_value.to_string() : prim.to_string();
                    } else {
                        prop_name = prop_value.to_string();
                    }
                }
            } else {
                prop_name = prop_value.to_string();
            }
        } else {
            if (member->get_property()->get_type() == ASTNode::Type::IDENTIFIER) {
                Identifier* id = static_cast<Identifier*>(member->get_property());
                prop_name = id->get_name();
            } else {
                ctx.throw_exception(Value(std::string("Invalid property access")));
                return Value();
            }
        }

        // For a private accessor/method, the descriptor lives on the declaring class's own
        // prototype, not necessarily the closest "#name" in obj's actual chain.
        // Only the literal `.#name` syntax is a private reference -- a computed
        // key that happens to spell "#name" is an ordinary property.
        Object* private_owner = nullptr;
        if (obj && !is_string_object && !member->is_computed() &&
            !prop_name.empty() && prop_name[0] == '#') {
            if (!private_brand_check(ctx, obj, prop_name, false)) {
                ctx.throw_type_error("Cannot write private member " + prop_name + " to an object whose class did not declare it");
                return Value();
            }
            // Fields are stored under a qualified key (see resolve_private_storage_key); left untouched for methods/getters/setters, which aren't found directly on the instance.
            // A bare "#x" own property can only be an ordinary computed-key
            // property -- it never counts as the private slot here.
            bool on_own_slot = false;
            {
                std::string qualified = resolve_private_storage_key(prop_name, obj);
                if (obj->has_private_slot(qualified)) {
                    prop_name = qualified;
                    on_own_slot = true;
                }
            }
            // For any assignment (including =), check if target is a private method or uninitialized field
            if (on_own_slot) {
                // Slot is directly on obj (fields; static private members on the
                // class constructor). Fully raw access -- never through Proxy
                // traps or exotic overrides (spec: private state bypasses
                // [[Get]]/[[Set]]) -- including the compound read-modify-write.
                PropertyDescriptor own_pd;
                bool is_own_accessor = obj->get_private_slot_descriptor(prop_name, own_pd) &&
                                       own_pd.is_accessor_descriptor();
                if (!is_own_accessor && own_pd.has_value() && own_pd.get_value().is_function()) {
                    Function* mfn = own_pd.get_value().as_function();
                    if (mfn && mfn->has_internal_slot("__private_class_brand__")) {
                        ctx.throw_type_error("'" + prop_name + "' is a private method and cannot be assigned to");
                        return Value();
                    }
                }
                Value final_value = right_value;
                if (operator_ != Operator::ASSIGN) {
                    Value cur;
                    if (is_own_accessor) {
                        if (!own_pd.has_getter()) {
                            ctx.throw_type_error("'" + prop_name + "' accessor has no getter");
                            return Value();
                        }
                        Function* getter_fn = as_function(own_pd.get_getter());
                        cur = getter_fn ? getter_fn->call(ctx, {}, object_value) : Value();
                        if (ctx.has_exception()) return Value();
                    } else {
                        cur = obj->get_private_slot_value(prop_name);
                    }
                    switch (operator_) {
                        case Operator::PLUS_ASSIGN:        final_value = cur.add(right_value); break;
                        case Operator::MINUS_ASSIGN:       final_value = cur.subtract(right_value); break;
                        case Operator::MUL_ASSIGN:         final_value = cur.multiply(right_value); break;
                        case Operator::DIV_ASSIGN:         final_value = cur.divide(right_value); break;
                        case Operator::MOD_ASSIGN:         final_value = cur.modulo(right_value); break;
                        case Operator::BITWISE_AND_ASSIGN: final_value = cur.bitwise_and(right_value); break;
                        case Operator::BITWISE_OR_ASSIGN:  final_value = cur.bitwise_or(right_value); break;
                        case Operator::BITWISE_XOR_ASSIGN: final_value = cur.bitwise_xor(right_value); break;
                        case Operator::LEFT_SHIFT_ASSIGN:  final_value = cur.left_shift(right_value); break;
                        case Operator::RIGHT_SHIFT_ASSIGN: final_value = cur.right_shift(right_value); break;
                        case Operator::UNSIGNED_RIGHT_SHIFT_ASSIGN: final_value = cur.unsigned_right_shift(right_value); break;
                        default: break;
                    }
                    if (ctx.has_exception()) return Value();
                }
                if (is_own_accessor) {
                    if (!own_pd.has_setter()) {
                        ctx.throw_type_error("'" + prop_name + "' was defined without a setter");
                        return Value();
                    }
                    Function* setter_fn = as_function(own_pd.get_setter());
                    if (setter_fn) setter_fn->call(ctx, {final_value}, object_value);
                    if (ctx.has_exception()) return Value();
                    return final_value;
                }
                obj->set_private_slot_value(prop_name, final_value);
                return final_value;
            } else {
                // Methods/accessors live under the qualified key on the declaring
                // prototype/constructor (bare fallback for older paths).
                std::string owner_qualified = resolve_private_storage_key(prop_name, obj);
                private_owner = resolve_private_accessor_owner(prop_name);
                PropertyDescriptor pd;
                if (private_owner) {
                    pd = private_owner->get_property_descriptor(owner_qualified);
                    if (pd.has_value() || pd.is_accessor_descriptor()) {
                        prop_name = owner_qualified;
                    } else {
                        pd = private_owner->get_property_descriptor(prop_name);
                    }
                }
                bool found_on_proto = pd.has_value() || pd.is_accessor_descriptor();
                if (!found_on_proto) {
                    // Fallback: no frame declared this name (e.g. resumed after await/yield).
                    private_owner = nullptr;
                    Object* proto = obj->get_prototype();
                    while (proto) {
                        pd = proto->get_property_descriptor(owner_qualified);
                        if (pd.has_value() || pd.is_accessor_descriptor()) {
                            prop_name = owner_qualified;
                            found_on_proto = true; private_owner = proto; break;
                        }
                        pd = proto->get_property_descriptor(prop_name);
                        if (pd.has_value() || pd.is_accessor_descriptor()) { found_on_proto = true; private_owner = proto; break; }
                        proto = proto->get_prototype();
                    }
                }
                if (found_on_proto) {
                    if (!pd.is_accessor_descriptor() && pd.get_value().is_function()) {
                        ctx.throw_type_error("'" + prop_name + "' is a private method and cannot be assigned to");
                        return Value();
                    }
                    if (pd.is_accessor_descriptor() && !pd.has_setter()) {
                        ctx.throw_type_error("'" + prop_name + "' was defined without a setter");
                        return Value();
                    }
                    if (pd.is_accessor_descriptor() && operator_ != Operator::ASSIGN) {
                        // Compound (+= etc.) on a prototype-held private accessor:
                        // read and write through the accessor pair with `this` =
                        // the instance. The generic switch below would call the
                        // getter via read_base->get_property, i.e. with the
                        // declaring prototype as the receiver.
                        if (!pd.has_getter()) {
                            ctx.throw_type_error("'" + prop_name + "' accessor has no getter");
                            return Value();
                        }
                        Function* getter_fn = as_function(pd.get_getter());
                        Value cur = getter_fn ? getter_fn->call(ctx, {}, object_value) : Value();
                        if (ctx.has_exception()) return Value();
                        Value final_value = right_value;
                        switch (operator_) {
                            case Operator::PLUS_ASSIGN:        final_value = cur.add(right_value); break;
                            case Operator::MINUS_ASSIGN:       final_value = cur.subtract(right_value); break;
                            case Operator::MUL_ASSIGN:         final_value = cur.multiply(right_value); break;
                            case Operator::DIV_ASSIGN:         final_value = cur.divide(right_value); break;
                            case Operator::MOD_ASSIGN:         final_value = cur.modulo(right_value); break;
                            case Operator::BITWISE_AND_ASSIGN: final_value = cur.bitwise_and(right_value); break;
                            case Operator::BITWISE_OR_ASSIGN:  final_value = cur.bitwise_or(right_value); break;
                            case Operator::BITWISE_XOR_ASSIGN: final_value = cur.bitwise_xor(right_value); break;
                            case Operator::LEFT_SHIFT_ASSIGN:  final_value = cur.left_shift(right_value); break;
                            case Operator::RIGHT_SHIFT_ASSIGN: final_value = cur.right_shift(right_value); break;
                            case Operator::UNSIGNED_RIGHT_SHIFT_ASSIGN: final_value = cur.unsigned_right_shift(right_value); break;
                            default: break;
                        }
                        if (ctx.has_exception()) return Value();
                        Function* setter_fn = as_function(pd.get_setter());
                        if (setter_fn) setter_fn->call(ctx, {final_value}, object_value);
                        if (ctx.has_exception()) return Value();
                        return final_value;
                    }
                } else {
                    // Private instance field not yet initialized on this object
                    ctx.throw_type_error("Cannot set private field " + prop_name + " on an object that has not been initialized");
                    return Value();
                }
            }
        }

        // Descriptor lookup starts at the super base / private_owner, but the write target is always 'this'/obj.
        Object* read_base = is_super_assignment ? super_lookup_proto : (private_owner ? private_owner : obj);

        // For a Proxy, [[Set]] must go straight to set_trap; this descriptor pre-check would fire an extra getOwnPropertyDescriptor trap call.
        if (read_base && !is_string_object && read_base->get_type() != Object::ObjectType::Proxy) {
            // Check own descriptor first, then prototype chain for setter
            PropertyDescriptor desc = read_base->get_property_descriptor(prop_name);
            bool found_inherited = false;
            if (!desc.is_accessor_descriptor() && !read_base->has_own_property(prop_name)) {
                // Walk prototype chain for accessor or non-writable data descriptor
                Object* proto = read_base->get_prototype();
                while (proto) {
                    // Stop at a Proxy -- its [[Set]] handles everything; we'd fire an unexpected trap here.
                    if (proto->get_type() == Object::ObjectType::Proxy) break;
                    // Integer-Indexed exotic [[Set]] (10.4.5.5): a canonical-but-invalid numeric key on a TypedArray ancestor (receiver != this typed array) is a no-op success right here, it must not keep walking up to that TypedArray's own .prototype for the same key.
                    if (proto->is_typed_array()) {
                        double num_idx;
                        if (TypedArrayBase::canonical_numeric_index(prop_name, num_idx) &&
                            !static_cast<TypedArrayBase*>(proto)->is_valid_integer_index(num_idx)) {
                            return right_value;
                        }
                    }
                    PropertyDescriptor proto_desc = proto->get_property_descriptor(prop_name);
                    if (proto_desc.is_accessor_descriptor()) {
                        desc = proto_desc;
                        found_inherited = true;
                        break;
                    }
                    if (proto_desc.has_value()) {
                        // Inherited non-writable data property blocks shadowing (spec 10.1.9 step 3b)
                        if (!proto_desc.is_writable() && ctx.is_strict_mode()) {
                            ctx.throw_type_error("Cannot assign to read only property '" + prop_name + "'");
                            return Value();
                        }
                        found_inherited = !proto_desc.is_writable();
                        if (!found_inherited) desc = proto_desc;
                        break;
                    }
                    proto = proto->get_prototype();
                }
            }
            (void)found_inherited;
            // For plain assignment only: invoke setter directly here.
            // Compound assignments (+=, &= etc.) need to read the current value first,
            // so they go through the switch and call set_property (which invokes setters).
            if (operator_ == Operator::ASSIGN && desc.is_accessor_descriptor() && desc.has_setter()) {
                Object* setter = desc.get_setter();
                if (setter) {
                    Function* setter_fn = as_function(setter);
                    if (setter_fn) {
                        try {
                            setter_fn->call(ctx, {right_value}, Value(obj));
                            if (ctx.has_exception()) return Value();
                            return right_value;
                        } catch (const std::exception& e) {
                            ctx.throw_exception(Value(std::string("Setter call failed: ") + e.what()));
                            return Value();
                        }
                    }
                }
            }
        }
        
        switch (operator_) {
            case Operator::ASSIGN:
                if (is_string_object) {
                    std::string str_val = object_value.to_string();
                    std::string new_prop = prop_name + "=" + right_value.to_string();
                    
                    if (str_val == "OBJECT:{}") {
                        str_val = "OBJECT:{" + new_prop + "}";
                    } else {
                        size_t close_pos = str_val.rfind('}');
                        if (close_pos != std::string::npos) {
                            str_val = str_val.substr(0, close_pos) + "," + new_prop + "}";
                        }
                    }
                    
                    if (member->get_object()->get_type() == ASTNode::Type::IDENTIFIER) {
                        Identifier* obj_id = static_cast<Identifier*>(member->get_object());
                        std::string var_name = obj_id->get_name();
                        ctx.set_binding(var_name, Value(str_val));
                        
                        if (var_name == "this") {
                            ctx.set_binding("this", Value(str_val));
                        }
                    }
                } else {
                    if (!store(prop_name, right_value)) return Value();
                }
                break;
            case Operator::PLUS_ASSIGN: {
                if (is_string_object) {
                    std::string str_val = object_value.to_string();
                    
                    std::string search_pattern = prop_name + "=";
                    size_t prop_start = str_val.find(search_pattern);
                    Value current_value = Value(0);
                    
                    if (prop_start != std::string::npos) {
                        size_t value_start = prop_start + search_pattern.length();
                        size_t value_end = str_val.find(",", value_start);
                        if (value_end == std::string::npos) {
                            value_end = str_val.find("}", value_start);
                        }
                        
                        if (value_end != std::string::npos) {
                            std::string current_value_str = str_val.substr(value_start, value_end - value_start);
                            try {
                                double num = std::stod(current_value_str);
                                current_value = Value(num);
                            } catch (...) {
                                current_value = Value(0);
                            }
                        }
                    }
                    
                    double new_value = current_value.to_number() + right_value.to_number();
                    std::string new_value_str = std::to_string(new_value);
                    
                    if (prop_start != std::string::npos) {
                        size_t value_start = prop_start + search_pattern.length();
                        size_t value_end = str_val.find(",", value_start);
                        if (value_end == std::string::npos) {
                            value_end = str_val.find("}", value_start);
                        }
                        
                        if (value_end != std::string::npos) {
                            str_val = str_val.substr(0, value_start) + new_value_str + str_val.substr(value_end);
                        }
                    } else {
                        std::string new_prop = prop_name + "=" + new_value_str;
                        size_t close_pos = str_val.rfind('}');
                        if (close_pos != std::string::npos) {
                            str_val = str_val.substr(0, close_pos) + "," + new_prop + "}";
                        }
                    }
                    
                    if (member->get_object()->get_type() == ASTNode::Type::IDENTIFIER) {
                        Identifier* obj_id = static_cast<Identifier*>(member->get_object());
                        std::string var_name = obj_id->get_name();
                        ctx.set_binding(var_name, Value(str_val));
                        
                        if (var_name == "this") {
                            ctx.set_binding("this", Value(str_val));
                        }
                    }
                } else {
                    Value current_value = read_base ? read_base->get_property(prop_name) : Value();
                    if (ctx.has_exception()) return Value();
                    // String concatenation or numeric addition
                    Value computed;
                    if (current_value.is_string() || right_value.is_string()) {
                        computed = Value(current_value.to_string() + right_value.to_string());
                    } else {
                        computed = Value(current_value.to_number() + right_value.to_number());
                    }
                    if (!store(prop_name, computed)) return Value();
                    return computed;
                }
                break;
            }
            case Operator::MINUS_ASSIGN:
            case Operator::MUL_ASSIGN:
            case Operator::DIV_ASSIGN:
            case Operator::MOD_ASSIGN:
            case Operator::BITWISE_AND_ASSIGN:
            case Operator::BITWISE_OR_ASSIGN:
            case Operator::BITWISE_XOR_ASSIGN:
            case Operator::LEFT_SHIFT_ASSIGN:
            case Operator::RIGHT_SHIFT_ASSIGN:
            case Operator::UNSIGNED_RIGHT_SHIFT_ASSIGN: {
                if (!obj) { ctx.throw_type_error("Cannot set property of null"); return Value(); }
                Value cur = read_base ? read_base->get_property(prop_name) : Value();
                if (ctx.has_exception()) return Value();
                double l = cur.to_number();
                double r = right_value.to_number();
                double result;
                switch (operator_) {
                    case Operator::MINUS_ASSIGN:               result = l - r; break;
                    case Operator::MUL_ASSIGN:                 result = l * r; break;
                    case Operator::DIV_ASSIGN:                 result = l / r; break;
                    case Operator::MOD_ASSIGN:                 result = std::fmod(l, r); break;
                    case Operator::BITWISE_AND_ASSIGN:         result = static_cast<double>(static_cast<int32_t>(l) & static_cast<int32_t>(r)); break;
                    case Operator::BITWISE_OR_ASSIGN:          result = static_cast<double>(static_cast<int32_t>(l) | static_cast<int32_t>(r)); break;
                    case Operator::BITWISE_XOR_ASSIGN:         result = static_cast<double>(static_cast<int32_t>(l) ^ static_cast<int32_t>(r)); break;
                    case Operator::LEFT_SHIFT_ASSIGN:          result = static_cast<double>(static_cast<int32_t>(l) << (static_cast<uint32_t>(r) & 0x1F)); break;
                    case Operator::RIGHT_SHIFT_ASSIGN:         result = static_cast<double>(static_cast<int32_t>(l) >> (static_cast<uint32_t>(r) & 0x1F)); break;
                    case Operator::UNSIGNED_RIGHT_SHIFT_ASSIGN:result = static_cast<double>(static_cast<uint32_t>(l) >> (static_cast<uint32_t>(r) & 0x1F)); break;
                    default: result = l; break;
                }
                if (!store(prop_name, Value(result))) return Value();
                return Value(result);
            }
            default:
                ctx.throw_exception(Value(std::string("Unsupported assignment operator for member expression")));
                return Value();
        }
        
        return right_value;
    }
    
    // ES6: Destructuring assignment with object or array pattern
    if (operator_ == Operator::ASSIGN &&
        (left_->get_type() == ASTNode::Type::OBJECT_LITERAL ||
         left_->get_type() == ASTNode::Type::ARRAY_LITERAL)) {
        right_value = right_->evaluate(ctx);
        if (ctx.has_exception()) return Value();

        destructuring_assign(ctx, left_.get(), right_value);
        if (ctx.has_exception()) return Value();
        return right_value;
    }

    if (operator_ == Operator::ASSIGN &&
        left_->get_type() == ASTNode::Type::DESTRUCTURING_ASSIGNMENT) {
        right_value = right_->evaluate(ctx);
        if (ctx.has_exception()) return Value();
        auto* destr = static_cast<DestructuringAssignment*>(left_.get());
        destr->evaluate_with_value(ctx, right_value);
        if (ctx.has_exception()) return Value();
        return right_value;
    }

    ctx.throw_exception(Value(std::string("Invalid assignment target")));
    return Value();
}

// Helper: recursively perform destructuring assignment from an ObjectLiteral or ArrayLiteral pattern
// A getter run while destructuring throws into Object::current_context_, which
// is not the context we are binding in whenever the two differ -- inside a
// generator's own fiber context, notably. Adopting it here is what stops the
// rest loop below from spinning forever on a throwing `value` getter.
static void adopt_foreign_exception(Context& ctx) {
    if (!ctx.has_exception() && Object::current_context_ && Object::current_context_ != &ctx
            && Object::current_context_->has_exception()) {
        ctx.throw_exception(Object::current_context_->get_exception(), true);
        Object::current_context_->clear_exception();
    }
}

// CopyDataProperties for an object pattern's `...rest`: every own enumerable
// property of the source the pattern did not already take. Shared with
// Op::CopyRestProperties so both paths build the same object.
Value build_rest_object(Context& ctx, const Value& source_value, Object* source_obj,
                        const std::vector<std::string>& assigned_keys) {
auto rest_obj = ObjectFactory::create_object();
if (source_value.is_string()) {
    // For strings, create indexed char properties (spec 12.15.5.2).
    const std::string& raw = source_value.as_string()->str();
    uint32_t char_idx = 0;
    size_t pos = 0;
    while (pos < raw.size()) {
        unsigned char c = static_cast<unsigned char>(raw[pos]);
        size_t cl = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
        if (pos + cl > raw.size()) cl = 1;
        std::string char_key = std::to_string(char_idx);
        bool already_assigned = false;
        for (const auto& ak : assigned_keys) {
            if (ak == char_key) { already_assigned = true; break; }
        }
        if (!already_assigned) {
            PropertyDescriptor cdesc(Value(raw.substr(pos, cl)),
                static_cast<PropertyAttributes>(PropertyAttributes::Writable |
                    PropertyAttributes::Enumerable | PropertyAttributes::Configurable));
            rest_obj->set_property_descriptor(char_key, cdesc);
        }
        pos += cl;
        char_idx++;
    }
} else if (source_obj->get_type() == Object::ObjectType::Proxy) {
    // get_enumerable_keys()/get_property() don't know about Proxy traps, so go through ownKeys/getOwnPropertyDescriptor/get directly per spec.
    Proxy* proxy = static_cast<Proxy*>(source_obj);
    for (const auto& k : proxy->own_keys_trap()) {
        bool already_assigned = false;
        for (const auto& ak : assigned_keys) {
            if (ak == k) { already_assigned = true; break; }
        }
        if (already_assigned) continue;
        // own_keys_trap() returns symbol keys as their "@@sym:" string encoding; decode back to the real Symbol so traps receive the original key, not its string form.
        Symbol* sym = Symbol::find_by_property_key(k);
        Value key_value = sym ? Value(sym) : Value(k);
        PropertyDescriptor kdesc = proxy->get_own_property_descriptor_trap(key_value);
        if (!kdesc.is_data_descriptor() && !kdesc.is_accessor_descriptor()) continue;
        if (!kdesc.is_enumerable()) continue;
        Value val = proxy->get_trap(key_value);
        if (ctx.has_exception()) return Value();
        PropertyDescriptor rdesc(val,
            static_cast<PropertyAttributes>(PropertyAttributes::Writable |
                PropertyAttributes::Enumerable | PropertyAttributes::Configurable));
        rest_obj->set_property_descriptor(k, rdesc);
    }
} else {
    // For objects: use enumerable keys only (spec excludes non-enumerable).
    auto keys = source_obj->get_enumerable_keys();
    for (const auto& k : keys) {
        bool already_assigned = false;
        for (const auto& ak : assigned_keys) {
            if (ak == k) { already_assigned = true; break; }
        }
        if (!already_assigned) {
            Value val = source_obj->get_property(k);
            if (ctx.has_exception()) return Value();
            PropertyDescriptor rdesc(val,
                static_cast<PropertyAttributes>(PropertyAttributes::Writable |
                    PropertyAttributes::Enumerable | PropertyAttributes::Configurable));
            rest_obj->set_property_descriptor(k, rdesc);
        }
    }
}
    return Value(rest_obj.release());
}

void AssignmentExpression::destructuring_assign(Context& ctx, ASTNode* pattern, const Value& source_value,
                                                DestructureMode mode) {
    if (pattern->get_type() == ASTNode::Type::OBJECT_LITERAL) {
        if (source_value.is_null() || source_value.is_undefined()) {
            ctx.throw_type_error("Cannot destructure " + std::string(source_value.is_null() ? "null" : "undefined"));
            return;
        }
        Object* source_obj = nullptr;
        if (source_value.is_object()) source_obj = source_value.as_object();
        else if (source_value.is_function()) source_obj = static_cast<Object*>(source_value.as_function());
        else if (source_value.is_string()) {
            // ES6: Box string with proper prototype chain
            auto wrapper = ObjectFactory::create_string(source_value.as_string()->str());
            Value ctor = ctx.get_binding("String");
            if (ctor.is_function()) {
                Value proto_val = ctor.as_function()->get_property("prototype");
                if (proto_val.is_object()) {
                    wrapper->set_prototype(proto_val.as_object());
                }
            }
            source_obj = wrapper.release();
        } else if (source_value.is_number() || source_value.is_boolean()) {
            // ES6: Box number/boolean with proper prototype chain
            std::string ctor_name = source_value.is_number() ? "Number" : "Boolean";
            Value ctor = ctx.get_binding(ctor_name);
            if (ctor.is_function()) {
                Value proto_val = ctor.as_function()->get_property("prototype");
                if (proto_val.is_object()) {
                    auto wrapper = ObjectFactory::create_object();
                    wrapper->set_prototype(proto_val.as_object());
                    source_obj = wrapper.release();
                }
            }
            if (!source_obj) {
                auto* wrapper = ObjectFactory::create_object().release();
                source_obj = wrapper;
            }
        } else if (source_value.is_symbol()) {
            // Symbols are object-coercible; ToObject(symbol) succeeds
            auto sym_wrapper = ObjectFactory::create_object();
            source_obj = sym_wrapper.release();
        } else if (source_value.is_bigint()) {
            // BigInts are object-coercible; ToObject(bigint) succeeds
            auto bigint_wrapper = ObjectFactory::create_object();
            Value ctor = ctx.get_binding("BigInt");
            if (ctor.is_function()) {
                Value proto_val = ctor.as_function()->get_property("prototype");
                if (proto_val.is_object()) bigint_wrapper->set_prototype(proto_val.as_object());
            }
            source_obj = bigint_wrapper.release();
        }
        if (!source_obj) {
            ctx.throw_type_error("Cannot destructure non-object value");
            return;
        }

        auto* obj_lit = static_cast<ObjectLiteral*>(pattern);
        std::vector<std::string> assigned_keys;

        for (const auto& prop : obj_lit->get_properties()) {
            // Handle rest element: {...rest}
            if (prop->type == ObjectLiteral::PropertyType::Value &&
                prop->value && prop->value->get_type() == ASTNode::Type::SPREAD_ELEMENT) {
                auto* spread = static_cast<SpreadElement*>(prop->value.get());
                ASTNode* rest_target = spread->get_argument();
                // Create object with remaining enumerable own properties.
                // Use set_property_descriptor with explicit WEC attrs so that numeric keys
                // whose getter returns undefined still appear in Object.keys (they'd be lost
                // as "holes" if stored via set_property/set_element with undefined value).
                Value rest_value = build_rest_object(ctx, source_value, source_obj, assigned_keys);
                if (ctx.has_exception()) return;
                assign_to_target(ctx, rest_target, rest_value, nullptr, nullptr, mode);
                if (ctx.has_exception()) return;
                continue;
            }

            // Get property name from key
            std::string prop_name;
            if (prop->computed) {
                Value key_val = prop->key->evaluate(ctx);
                if (ctx.has_exception()) return;
                // ToPropertyKey, not a raw stringify: an object key's own
                // @@toPrimitive/toString has to run.
                prop_name = key_val.to_property_key();
                if (ctx.has_exception()) return;
            } else if (prop->key->get_type() == ASTNode::Type::IDENTIFIER) {
                prop_name = static_cast<Identifier*>(prop->key.get())->get_name();
            } else if (prop->key->get_type() == ASTNode::Type::STRING_LITERAL) {
                prop_name = static_cast<StringLiteral*>(prop->key.get())->get_value();
            } else if (prop->key->get_type() == ASTNode::Type::NUMBER_LITERAL) {
                double kv = static_cast<NumberLiteral*>(prop->key.get())->get_value();
                if (kv == std::floor(kv) && kv >= static_cast<double>(LLONG_MIN) && kv <= static_cast<double>(LLONG_MAX)) {
                    prop_name = std::to_string(static_cast<long long>(kv));
                } else {
                    std::ostringstream koss;
                    koss << kv;
                    prop_name = koss.str();
                }
            }
            assigned_keys.push_back(prop_name);

            Value prop_value = source_obj->get_property(prop_name);
            // Getter may throw into Object::current_context_ rather than ctx
            if (!ctx.has_exception() && Object::current_context_ && Object::current_context_ != &ctx
                    && Object::current_context_->has_exception()) {
                ctx.throw_exception(Object::current_context_->get_exception(), true);
                Object::current_context_->clear_exception();
            }
            if (ctx.has_exception()) return;

            // Determine assignment target
            ASTNode* target = prop->shorthand ? prop->key.get() : prop->value.get();

            // Check for defaults: shorthand with AssignmentExpression value means {a = default}
            if (prop->shorthand && prop->value &&
                prop->value->get_type() == ASTNode::Type::ASSIGNMENT_EXPRESSION) {
                auto* assign = static_cast<AssignmentExpression*>(prop->value.get());
                ASTNode* lhs = assign->left_.get();
                if (prop_value.is_undefined()) {
                    prop_value = assign->right_->evaluate(ctx);
                    if (ctx.has_exception()) return;
                    if (prop_value.is_function() && is_anonymous_function_def(assign->right_.get()) &&
                            lhs && lhs->get_type() == ASTNode::Type::IDENTIFIER) {
                        Function* fn = prop_value.as_function();
                        if (fn->get_name().empty() || fn->get_name() == "<arrow>") {
                            fn->set_name(static_cast<Identifier*>(lhs)->get_name());
                        }
                    }
                }
                target = lhs;
            }

            // Non-shorthand with AssignmentExpression value: {key: target = default}
            if (!prop->shorthand && target &&
                target->get_type() == ASTNode::Type::ASSIGNMENT_EXPRESSION) {
                auto* assign = static_cast<AssignmentExpression*>(target);
                ASTNode* lhs = assign->left_.get();
                if (prop_value.is_undefined()) {
                    prop_value = assign->right_->evaluate(ctx);
                    if (ctx.has_exception()) return;
                    if (prop_value.is_function() && is_anonymous_function_def(assign->right_.get()) &&
                            lhs && lhs->get_type() == ASTNode::Type::IDENTIFIER) {
                        Function* fn = prop_value.as_function();
                        if (fn->get_name().empty() || fn->get_name() == "<arrow>") {
                            fn->set_name(static_cast<Identifier*>(lhs)->get_name());
                        }
                    }
                }
                target = lhs;
            }

            assign_to_target(ctx, target, prop_value, nullptr, nullptr, mode);
            if (ctx.has_exception()) return;
        }
    } else if (pattern->get_type() == ASTNode::Type::ARRAY_LITERAL) {
        if (source_value.is_null() || source_value.is_undefined()) {
            ctx.throw_type_error("Cannot destructure " + std::string(source_value.is_null() ? "null" : "undefined"));
            return;
        }
        Object* source_arr = nullptr;
        uint32_t source_len = 0;
        bool is_string_source = false;
        std::string str_source;
        // Deferred IteratorClose: must happen AFTER default value evaluation (not before).
        Value deferred_iter_close_obj;
        bool deferred_iter_close_needed = false;

        // MemberExpression targets get their reference evaluated once up front, before iterator.next() -- cache those values per element index so the assignment below reuses them instead of re-evaluating.
        size_t elem_count_for_precompute = static_cast<ArrayLiteral*>(pattern)->get_elements().size();
        std::vector<Value> precomputed_member_obj(elem_count_for_precompute);
        std::vector<Value> precomputed_member_key(elem_count_for_precompute);
        std::vector<bool> has_precomputed_member(elem_count_for_precompute, false);

        // For codepoint-aware string destructuring
        std::vector<std::string> str_codepoints;

        if (source_value.is_string()) {
            is_string_source = true;
            str_source = source_value.as_string()->str();
            // Split into UTF-8 codepoints
            size_t pos = 0;
            while (pos < str_source.length()) {
                unsigned char ch = static_cast<unsigned char>(str_source[pos]);
                size_t cl = 1;
                if (ch >= 0xF0) cl = 4;
                else if (ch >= 0xE0) cl = 3;
                else if (ch >= 0xC0) cl = 2;
                if (pos + cl > str_source.length()) cl = 1;
                str_codepoints.push_back(str_source.substr(pos, cl));
                pos += cl;
            }
            source_len = static_cast<uint32_t>(str_codepoints.size());
        } else if (source_value.is_number() || source_value.is_boolean() || source_value.is_symbol()) {
            // Primitives (number/boolean/symbol) are not iterable -- throw TypeError
            ctx.throw_type_error("Cannot destructure a non-iterable value");
            return;
        } else if (source_value.is_object() || source_value.is_function()) {
            source_arr = source_value.is_function()
                ? static_cast<Object*>(source_value.as_function())
                : source_value.as_object();
            // For arrays: verify Symbol.iterator is callable (deleted iterator -> TypeError)
            if (source_arr && source_arr->is_array()) {
                Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
                if (iter_sym) {
                    Value iter_method = source_arr->get_property(iter_sym->to_property_key());
                    if (!iter_method.is_function()) {
                        ctx.throw_type_error("Cannot destructure: Symbol.iterator is not callable");
                        return;
                    }
                }
            }
            // Everything that is not a plain Array goes through the real iterator
            // protocol -- and so does an Array once anything has replaced
            // @@iterator or %ArrayIteratorPrototype%.next, which is what the
            // protector tracks. Skipping it there would ignore a patched
            // iterator, the same deviation the spread path had.
            if (source_arr && (!source_arr->is_array() || !Object::array_iterator_protector_intact())) {
                Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
                if (iter_sym) {
                    Value iter_method = source_arr->get_property(iter_sym->to_property_key());
                    // Array destructuring is defined in terms of GetIterator, so a
                    // source without a callable @@iterator is simply not iterable --
                    // an array-like shape does not stand in for one.
                    if (!iter_method.is_function()) {
                        ctx.throw_type_error("Cannot destructure a non-iterable value");
                        return;
                    }
                    if (iter_method.is_function()) {
                        Value iter_obj = iter_method.as_function()->call(ctx, {}, source_value);
                        if (!ctx.has_exception() && iter_obj.is_object()) {
                            Value next_fn = iter_obj.as_object()->get_property("next");
                            {
                                // next() may not be callable (e.g. a custom iterator object that only implements return()) 
                                // per spec, that's only discovered (and throws) when next() is actually invoked, not before. 
                                // Target reference evaluation for each element still happens first.
                                auto call_next = [&]() -> Value {
                                    if (!next_fn.is_function()) {
                                        ctx.throw_type_error("Iterator's next() is not callable");
                                        return Value();
                                    }
                                    return next_fn.as_function()->call(ctx, {}, iter_obj);
                                };
                                // Determine how many elements we need from pattern
                                auto* arr_lit_check = static_cast<ArrayLiteral*>(pattern);
                                const auto& elems_check = arr_lit_check->get_elements();
                                bool has_rest = false;
                                size_t needed = elems_check.size();
                                for (size_t ei = 0; ei < elems_check.size(); ei++) {
                                    if (elems_check[ei] && elems_check[ei]->get_type() == ASTNode::Type::SPREAD_ELEMENT) {
                                        has_rest = true;
                                        break;
                                    }
                                }
                                // IteratorClose per spec 7.4.6:
                                // - If there was a pending exception (throw completion): call return(),
                                //   discard its result, restore original exception
                                // - If no pending exception (normal completion): call return(),
                                //   propagate its errors, TypeError if result is not Object
                                auto close_iter = [&]() {
                                    bool had_exception = ctx.has_exception();
                                    Value saved_exc = ctx.get_exception();
                                    ctx.clear_exception();
                                    Value ret_m = iter_obj.as_object()->get_property("return");
                                    if (!ret_m.is_function()) {
                                        if (had_exception) ctx.throw_exception(saved_exc, true);
                                        return;
                                    }
                                    Value inner = ret_m.as_function()->call(ctx, {}, iter_obj);
                                    if (had_exception) {
                                        ctx.clear_exception();
                                        ctx.throw_exception(saved_exc, true);
                                    } else if (!ctx.has_exception() && !inner.is_object()) {
                                        ctx.throw_type_error("Iterator return() must return an Object");
                                    }
                                };

                                // Spec 7.4.6 IteratorClose with a return completion (the generator being destructured into was itself resumed via .return() while
                                // call iter_obj's return(), propagate its throw or a TypeError for a non-Object result, otherwise rethrow the original GeneratorReturnException.
                                auto close_iter_for_generator_return = [&]() {
                                    Value ret_m = iter_obj.as_object()->get_property("return");
                                    if (ret_m.is_function()) {
                                        Value inner = ret_m.as_function()->call(ctx, {}, iter_obj);
                                        if (ctx.has_exception()) return;
                                        if (!inner.is_object()) {
                                            ctx.throw_type_error("Iterator return() result must be an Object");
                                            return;
                                        }
                                    }
                                    throw;
                                };

                                if (has_rest) {
                                    const auto& elems_r = arr_lit_check->get_elements();
                                    auto temp = ObjectFactory::create_array(0);
                                    uint32_t cnt = 0;
                                    bool iter_done = false;
                                    ASTNode* rest_target = nullptr;

                                    // Spec: leading (non-rest) elements consume the iterator first, each evaluating its target reference before calling next()
                                    // same ordering as the non-rest loop below.
                                    // Only once all of them are done do we reach the rest element itself.
                                    for (size_t ri = 0; ri < elems_r.size() && !iter_done; ri++) {
                                        if (elems_r[ri] && elems_r[ri]->get_type() == ASTNode::Type::SPREAD_ELEMENT) {
                                            rest_target = static_cast<SpreadElement*>(elems_r[ri].get())->get_argument();
                                            break;
                                        }
                                        ASTNode* tgt = elems_r[ri] ? elems_r[ri].get() : nullptr;
                                        if (tgt && tgt->get_type() == ASTNode::Type::ASSIGNMENT_EXPRESSION) {
                                            tgt = static_cast<AssignmentExpression*>(tgt)->left_.get();
                                        }
                                        if (tgt && tgt->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
                                            auto* mem = static_cast<MemberExpression*>(tgt);
                                            try {
                                                mem->get_object()->evaluate(ctx);
                                            } catch (const GeneratorReturnException&) {
                                                close_iter_for_generator_return();
                                                return;
                                            }
                                            if (ctx.has_exception()) { close_iter(); return; }
                                            if (mem->is_computed()) {
                                                try {
                                                    mem->get_property()->evaluate(ctx);
                                                } catch (const GeneratorReturnException&) {
                                                    close_iter_for_generator_return();
                                                    return;
                                                }
                                                if (ctx.has_exception()) { close_iter(); return; }
                                            }
                                        }
                                        // Per spec, if next() throws, do NOT close the iterator
                                        // (no IteratorClose on abrupt next).
                                        Value res = call_next();
                                        if (ctx.has_exception()) return;
                                        if (!res.is_object()) { iter_done = true; break; }
                                        Value done_v = res.as_object()->get_property("done");
                                        adopt_foreign_exception(ctx);
                                        if (ctx.has_exception()) return;
                                        if (done_v.to_boolean()) { iter_done = true; break; }
                                        Value val_v = res.as_object()->get_property("value");
                                        adopt_foreign_exception(ctx);
                                        if (ctx.has_exception()) return;
                                        temp->set_element(cnt++, val_v);
                                    }

                                    // Spec AssignmentRestElement step 1
                                    // for a MemberExpression target,evaluate the reference (object + key) BEFORE consuming the rest of the iterator.
                                    // ReturnIfAbrupt: if evaluation throws, close.
                                    if (!iter_done && rest_target && rest_target->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
                                        auto* mem = static_cast<MemberExpression*>(rest_target);
                                        try {
                                            mem->get_object()->evaluate(ctx);
                                        } catch (const GeneratorReturnException&) {
                                            close_iter_for_generator_return();
                                            return;
                                        }
                                        if (ctx.has_exception()) { close_iter(); return; }
                                        if (mem->is_computed()) {
                                            try {
                                                mem->get_property()->evaluate(ctx);
                                            } catch (const GeneratorReturnException&) {
                                                close_iter_for_generator_return();
                                                return;
                                            }
                                            if (ctx.has_exception()) { close_iter(); return; }
                                        }
                                    }

                                    // Rest: collect all remaining into temp array
                                    if (!iter_done) {
                                        for (;;) {
                                            Collector::safepoint();
                                            // Per spec, if next() throws, do NOT close the iterator
                                            // (no IteratorClose on abrupt next).
                                            Value res = call_next();
                                            if (ctx.has_exception()) return;
                                            if (!res.is_object()) { iter_done = true; break; }
                                            Value done_v = res.as_object()->get_property("done");
                                            adopt_foreign_exception(ctx);
                                            if (ctx.has_exception()) return;
                                            if (done_v.to_boolean()) { iter_done = true; break; }
                                            Value val_v = res.as_object()->get_property("value");
                                            adopt_foreign_exception(ctx);
                                            if (ctx.has_exception()) return;
                                            temp->set_element(cnt++, val_v);
                                        }
                                    }
                                    if (!iter_done) { close_iter(); }
                                    temp->set_length(cnt);
                                    source_arr = temp.release();
                                } else {
                                    // Spec: for each non-rest element, evaluate target ref FIRST,
                                    // then call next(). This allows member expression key errors
                                    // to occur before iterator.next() is called.
                                    auto temp = ObjectFactory::create_array(0);
                                    uint32_t cnt = 0;
                                    bool iter_done = false;
                                    const auto& elems_c = arr_lit_check->get_elements();

                                    for (size_t ei = 0; ei < needed && !iter_done; ei++) {
                                        const auto& elem_c = elems_c[ei];
                                        // Get actual target (skip elisions)
                                        ASTNode* tgt = elem_c ? elem_c.get() : nullptr;
                                        // Unwrap default
                                        if (tgt && tgt->get_type() == ASTNode::Type::ASSIGNMENT_EXPRESSION) {
                                            tgt = static_cast<AssignmentExpression*>(tgt)->left_.get();
                                        }
                                        // If target is MemberExpression: evaluate base+key NOW (before next())
                                        // so that errors in key evaluation prevent next() from being called
                                        if (tgt && tgt->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
                                            auto* mem = static_cast<MemberExpression*>(tgt);
                                            try {
                                                precomputed_member_obj[ei] = mem->get_object()->evaluate(ctx);
                                            } catch (const GeneratorReturnException&) {
                                                close_iter_for_generator_return();
                                                return;
                                            }
                                            if (ctx.has_exception()) { close_iter(); return; }
                                            has_precomputed_member[ei] = true;
                                            if (mem->is_computed()) {
                                                try {
                                                    precomputed_member_key[ei] = mem->get_property()->evaluate(ctx);
                                                } catch (const GeneratorReturnException&) {
                                                    close_iter_for_generator_return();
                                                    return;
                                                }
                                                if (ctx.has_exception()) { close_iter(); return; }
                                            }
                                        }
                                        // Now call next(). Per spec, if next()/done/value throw,
                                        // do NOT close the iterator (no IteratorClose on abrupt next).
                                        Value res = call_next();
                                        if (ctx.has_exception()) return;
                                        if (!res.is_object()) { iter_done = true; break; }
                                        Value done_v = res.as_object()->get_property("done");
                                        adopt_foreign_exception(ctx);
                                        if (ctx.has_exception()) return;
                                        if (done_v.to_boolean()) { iter_done = true; break; }
                                        Value val_v = res.as_object()->get_property("value");
                                        adopt_foreign_exception(ctx);
                                        if (ctx.has_exception()) return;
                                        temp->set_element(cnt++, val_v);
                                    }
                                    if (!iter_done) {
                                        // Defer IteratorClose until after default value evaluation.
                                        deferred_iter_close_needed = true;
                                        deferred_iter_close_obj = iter_obj;
                                    }
                                    temp->set_length(cnt);
                                    source_arr = temp.release();
                                }
                            }
                        }
                    }
                }
            }
            source_len = source_arr->get_length();
        }

        auto* arr_lit = static_cast<ArrayLiteral*>(pattern);
        const auto& elements = arr_lit->get_elements();

        for (size_t i = 0; i < elements.size(); i++) {
            const auto& elem = elements[i];
            if (!elem) continue; // hole/elision

            // Handle rest element: [...rest]
            if (elem->get_type() == ASTNode::Type::SPREAD_ELEMENT) {
                auto* spread = static_cast<SpreadElement*>(elem.get());
                ASTNode* rest_target = spread->get_argument();
                auto rest_arr = ObjectFactory::create_array(0);
                uint32_t rest_idx = 0;
                for (uint32_t j = static_cast<uint32_t>(i); j < source_len; j++) {
                    Value val;
                    if (is_string_source) {
                        val = (j < str_codepoints.size()) ? Value(str_codepoints[j]) : Value();
                    } else {
                        val = source_arr->get_element(j);
                    }
                    rest_arr->set_element(rest_idx++, val);
                }
                rest_arr->set_length(rest_idx);
                assign_to_target(ctx, rest_target, Value(rest_arr.release()), nullptr, nullptr, mode);
                break;
            }

            Value elem_value;
            if (is_string_source) {
                elem_value = (i < str_codepoints.size()) ? Value(str_codepoints[i]) : Value();
            } else if (source_arr) {
                elem_value = (i < source_len) ? source_arr->get_element(static_cast<uint32_t>(i)) : Value();
            }

            ASTNode* target = elem.get();

            // Check for default: element is AssignmentExpression like (a = default)
            if (target->get_type() == ASTNode::Type::ASSIGNMENT_EXPRESSION) {
                auto* assign = static_cast<AssignmentExpression*>(target);
                ASTNode* lhs = assign->left_.get();
                if (elem_value.is_undefined()) {
                    // Default may yield/return. On GeneratorReturnException, close iter first.
                    if (deferred_iter_close_needed) {
                        try {
                            elem_value = assign->right_->evaluate(ctx);
                        } catch (const GeneratorReturnException&) {
                            // Spec 7.4.6 IteratorClose with return completion:
                            // Call iterator.return(); propagate its throw or TypeError for non-Object.
                            // If it succeeds and returns Object, propagate the original return.
                            Value ret_m = deferred_iter_close_obj.as_object()->get_property("return");
                            if (ret_m.is_function()) {
                                Value inner = ret_m.as_function()->call(ctx, {}, deferred_iter_close_obj);
                                if (ctx.has_exception()) {
                                    // iterator.return() threw -- propagate that throw.
                                    return;
                                }
                                if (!inner.is_object()) {
                                    ctx.throw_type_error("Iterator return() result must be an Object");
                                    return;
                                }
                            }
                            throw; // iterator.return() ok -- propagate original return
                        }
                    } else {
                        elem_value = assign->right_->evaluate(ctx);
                    }
                    // Break (not return) so the deferred IteratorClose below still runs -- it preserves this exception over any of its own.
                    if (ctx.has_exception()) break;
                    if (elem_value.is_function() && is_anonymous_function_def(assign->right_.get()) &&
                            lhs && lhs->get_type() == ASTNode::Type::IDENTIFIER) {
                        Function* fn = elem_value.as_function();
                        if (fn->get_name().empty() || fn->get_name() == "<arrow>") {
                            fn->set_name(static_cast<Identifier*>(lhs)->get_name());
                        }
                    }
                }
                target = lhs;
            }

            if (i < has_precomputed_member.size() && has_precomputed_member[i]) {
                assign_to_target(ctx, target, elem_value, &precomputed_member_obj[i], &precomputed_member_key[i], mode);
            } else {
                assign_to_target(ctx, target, elem_value, nullptr, nullptr, mode);
            }
            if (ctx.has_exception()) break;
        }
        // Spec ArrayAssignmentPattern step 5: IteratorClose if iterator not exhausted.
        if (deferred_iter_close_needed && deferred_iter_close_obj.is_object()) {
            bool had_exc = ctx.has_exception();
            Value saved_exc = had_exc ? ctx.get_exception() : Value();
            if (had_exc) ctx.clear_exception();
            Value ret_m = deferred_iter_close_obj.as_object()->get_property("return");
            if (ret_m.is_function()) {
                Value inner = ret_m.as_function()->call(ctx, {}, deferred_iter_close_obj);
                if (had_exc) {
                    // Throw completion: suppress inner errors, restore original.
                    ctx.clear_exception();
                    ctx.throw_exception(saved_exc, true);
                } else if (!ctx.has_exception() && !inner.is_object()) {
                    ctx.throw_type_error("Iterator return() result must be an Object");
                }
            } else if (had_exc) {
                ctx.throw_exception(saved_exc, true);
            }
        }
    }
}

// Helper: assign a value to a target node (Identifier, MemberExpression, or nested pattern)
void AssignmentExpression::assign_to_target(Context& ctx, ASTNode* target, const Value& value,
                                              const Value* precomputed_obj, const Value* precomputed_key,
                                              DestructureMode mode) {
    if (!target) return;

    if (target->get_type() == ASTNode::Type::IDENTIFIER) {
        std::string name = static_cast<Identifier*>(target)->get_name();
        // A declaration binds a fresh name; only the assignment form writes
        // through an existing reference.
        if (mode != DestructureMode::Assign) {
            if (mode == DestructureMode::Let) {
                ctx.create_lexical_binding(name, value, true);
            } else if (mode == DestructureMode::Const) {
                ctx.create_lexical_binding(name, value, false);
            } else if (!ctx.has_binding(name)) {
                ctx.create_binding(name, value, true);
            } else if (!ctx.set_binding(name, value) && ctx.is_strict_const(name)) {
                ctx.throw_type_error("Assignment to constant variable '" + name + "'");
            }
            return;
        }
        if (ctx.has_binding(name)) {
            // This path gates on has_binding rather than resolving the
            // reference, so it has no environment to ask; resolving one just
            // for the question would fire an object environment's HasBinding a
            // second time. Left on the chain walk until the path resolves once.
            if (ctx.is_in_tdz(name)) {
                ctx.throw_reference_error("Cannot access '" + name + "' before initialization");
                return;
            }
            bool ok = ctx.set_binding(name, value);
            if (!ok) {
                if (ctx.is_strict_mode() || ctx.is_strict_const(name)) {
                    ctx.throw_type_error("Assignment to constant variable '" + name + "'");
                }
            }
        } else {
            if (ctx.is_strict_mode()) {
                ctx.throw_reference_error("'" + name + "' is not defined");
                return;
            }
            // ES5 8.7.2: PutValue on unresolvable reference -- always sets a property
            // on the global object (creating an implicit global var), regardless of
            // which scope the assignment executes in.
            Object* global = ctx.get_global_object();
            if (global) global->set_property(name, value);
        }
    } else if (target->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
        auto* member = static_cast<MemberExpression*>(target);
        // Reuse a reference already evaluated earlier (e.g. destructuring's evaluate-target-before-next() ordering) instead of re-evaluating and re-triggering side effects.
        Value obj_val = precomputed_obj ? *precomputed_obj : member->get_object()->evaluate(ctx);
        if (ctx.has_exception()) return;
        if (obj_val.is_null() || obj_val.is_undefined()) {
            ctx.throw_type_error(std::string("Cannot set properties of ") +
                (obj_val.is_null() ? "null" : "undefined"));
            return;
        }
        if (obj_val.is_object_like()) {
            Object* obj = obj_val.is_object() ? obj_val.as_object()
                                              : static_cast<Object*>(obj_val.as_function());
            std::string prop_name;
            if (member->is_computed()) {
                Value key_val = precomputed_key ? *precomputed_key : member->get_property()->evaluate(ctx);
                if (ctx.has_exception()) return;
                prop_name = key_val.to_property_key();
                if (ctx.has_exception()) return;
            } else if (member->get_property()->get_type() == ASTNode::Type::IDENTIFIER) {
                prop_name = static_cast<Identifier*>(member->get_property())->get_name();
            }
            // Fields are stored under a qualified key (see resolve_private_storage_key); fall back to the bare key for methods/getters/setters, which live unqualified on the prototype.
            // Only the literal `.#name` syntax is a private reference.
            if (!member->is_computed() && !prop_name.empty() && prop_name[0] == '#') {
                // require_exists=true: unlike the normal assignment path (binary.cpp/
                // assignment.cpp's identifier-LHS branch), this destructuring target
                // has no separate follow-up "does the slot actually exist" check of
                // its own, so private_brand_check must do that check itself here --
                // for fields specifically that's the *only* real check (no brand).
                if (!private_brand_check(ctx, obj, prop_name, true)) {
                    ctx.throw_type_error("Cannot write private member " + prop_name + " to an object whose class did not declare it");
                    return;
                }
                std::string qualified = resolve_private_storage_key(prop_name, obj);
                if (obj->has_private_slot(qualified)) {
                    // Raw write: private state bypasses [[Set]] (Proxy traps,
                    // exotic overrides) entirely.
                    obj->set_private_slot_value(qualified, value);
                    return;
                }
            }
            obj->ordinary_set(prop_name, value);
        }
    } else if (target->get_type() == ASTNode::Type::OBJECT_LITERAL ||
               target->get_type() == ASTNode::Type::ARRAY_LITERAL) {
        // Nested destructuring
        destructuring_assign(ctx, target, value, mode);
    }
}

std::string AssignmentExpression::to_string() const {
    std::string op_str;
    switch (operator_) {
        case Operator::ASSIGN: op_str = " = "; break;
        case Operator::PLUS_ASSIGN: op_str = " += "; break;
        case Operator::MINUS_ASSIGN: op_str = " -= "; break;
        case Operator::MUL_ASSIGN: op_str = " *= "; break;
        case Operator::DIV_ASSIGN: op_str = " /= "; break;
        case Operator::MOD_ASSIGN: op_str = " %= "; break;
        case Operator::LOGICAL_AND_ASSIGN: op_str = " &&= "; break;
        case Operator::LOGICAL_OR_ASSIGN: op_str = " ||= "; break;
        case Operator::NULLISH_ASSIGN: op_str = " ??= "; break;
        default: op_str = " op= "; break;
    }
    return left_->to_string() + op_str + right_->to_string();
}

std::unique_ptr<ASTNode> AssignmentExpression::clone() const {
    return std::make_unique<AssignmentExpression>(
        left_->clone(), operator_, right_->clone(), start_, end_, lhs_is_paren_
    );
}


static bool is_anonymous_function_def(const ASTNode* node) {
    if (!node) return false;
    auto t = node->get_type();
    return t == ASTNode::Type::FUNCTION_EXPRESSION ||
           t == ASTNode::Type::ARROW_FUNCTION_EXPRESSION ||
           t == ASTNode::Type::ASYNC_FUNCTION_EXPRESSION ||
           t == ASTNode::Type::CLASS_DECLARATION;
}

// Walks the pattern literal, which is the same shape AssignmentExpression's
// own destructuring walks: an ObjectLiteral property's value is the target
// (an Identifier, a nested literal, or an AssignmentExpression carrying a
// default), an ArrayLiteral element likewise, with SpreadElement for rest.
static void walk_pattern_targets(const ASTNode* pattern,
                                 const std::function<void(const ASTNode*)>& on_target,
                                 const std::function<void(const ASTNode*)>& on_expression) {
    if (!pattern) return;
    auto visit_target = [&](const ASTNode* t) {
        if (!t) return;
        if (t->get_type() == ASTNode::Type::ASSIGNMENT_EXPRESSION) {
            const auto* ae = static_cast<const AssignmentExpression*>(t);
            if (on_expression && ae->get_right()) on_expression(ae->get_right());
            walk_pattern_targets(ae->get_left(), on_target, on_expression);
            if (ae->get_left() && ae->get_left()->get_type() != ASTNode::Type::OBJECT_LITERAL &&
                ae->get_left()->get_type() != ASTNode::Type::ARRAY_LITERAL) {
                on_target(ae->get_left());
            }
            return;
        }
        if (t->get_type() == ASTNode::Type::OBJECT_LITERAL ||
            t->get_type() == ASTNode::Type::ARRAY_LITERAL) {
            walk_pattern_targets(t, on_target, on_expression);
            return;
        }
        if (t->get_type() == ASTNode::Type::SPREAD_ELEMENT) {
            walk_pattern_targets(static_cast<const SpreadElement*>(t)->get_argument(),
                                 on_target, on_expression);
            on_target(static_cast<const SpreadElement*>(t)->get_argument());
            return;
        }
        on_target(t);
    };

    if (pattern->get_type() == ASTNode::Type::OBJECT_LITERAL) {
        for (const auto& prop : static_cast<const ObjectLiteral*>(pattern)->get_properties()) {
            if (prop->computed && prop->key && on_expression) on_expression(prop->key.get());
            visit_target(prop->value ? prop->value.get() : nullptr);
        }
    } else if (pattern->get_type() == ASTNode::Type::ARRAY_LITERAL) {
        for (const auto& el : static_cast<const ArrayLiteral*>(pattern)->get_elements()) {
            if (el) visit_target(el.get());
        }
    }
}

void DestructuringAssignment::collect_bound_names(std::vector<std::string>& out) const {
    walk_pattern_targets(pattern_literal_.get(),
        [&](const ASTNode* t) {
            if (t && t->get_type() == ASTNode::Type::IDENTIFIER) {
                const std::string& n = static_cast<const Identifier*>(t)->get_name();
                if (!n.empty()) out.push_back(n);
            }
        },
        nullptr);
}

void DestructuringAssignment::for_each_expression(const std::function<void(const ASTNode*)>& fn) const {
    walk_pattern_targets(pattern_literal_.get(),
        [&](const ASTNode* t) {
            // A member-expression target (`[o.x] = v`) is itself an expression.
            if (t && t->get_type() != ASTNode::Type::IDENTIFIER) fn(t);
        },
        fn);
}

Value DestructuringAssignment::evaluate_with_value(Context& ctx, const Value& source_value,
                                                   bool as_lexical, bool is_const) {
    // One binder for both destructuring forms: the pattern literal is exactly
    // the shape AssignmentExpression::destructuring_assign already walks, so a
    // declaration differs from an assignment only in how leaves are bound.
    AssignmentExpression::DestructureMode mode = !as_lexical ? AssignmentExpression::DestructureMode::Var
                         : (is_const ? AssignmentExpression::DestructureMode::Const
                                     : AssignmentExpression::DestructureMode::Let);
    if (pattern_literal_) {
        AssignmentExpression::destructuring_assign(ctx, pattern_literal_.get(), source_value, mode);
    }
    return Value();
}

Value DestructuringAssignment::evaluate(Context& ctx) {
    if (!source_) {
        ctx.throw_exception(Value(std::string("DestructuringAssignment: source is null")));
        return Value();
    }

    Value source_value = source_->evaluate(ctx);
    if (ctx.has_exception()) return Value();

    return evaluate_with_value(ctx, source_value);
}

std::string DestructuringAssignment::to_string() const {
    return pattern_literal_ ? pattern_literal_->to_string() : std::string("[destructuring]");
}

std::unique_ptr<ASTNode> DestructuringAssignment::clone() const {
    auto cloned = std::make_unique<DestructuringAssignment>(
        std::vector<std::unique_ptr<Identifier>>{}, source_ ? source_->clone() : nullptr,
        type_, start_, end_);
    if (pattern_literal_) cloned->set_pattern_literal(pattern_literal_->clone());
    return cloned;
}


} // namespace Quanta
