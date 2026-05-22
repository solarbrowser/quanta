/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/parser/AST.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/engine/Engine.h"
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

        // For compound assignments, capture left value BEFORE evaluating right side
        // This ensures correct ES1 left-to-right evaluation order
        Value left_value;
        if (operator_ != Operator::ASSIGN) {
            left_value = ctx.get_binding(name);
            if (ctx.has_exception()) return Value();
        }

        // Now evaluate right side
        right_value = right_->evaluate(ctx);
        if (ctx.has_exception()) {
            return Value();
        }

        switch (operator_) {
            case Operator::ASSIGN: {
                // SetFunctionName: x = (function(){}) -> x.name = 'x'
                if (right_value.is_function() && is_anonymous_function_def(right_.get())) {
                    const std::string& fname = right_value.as_function()->get_name();
                    if (fname.empty() || fname == "<arrow>") {
                        right_value.as_function()->set_name(name);
                    }
                }
                bool has_it = ctx.has_binding(name);
                if (!has_it) {
                    if (ctx.is_strict_mode()) {
                        ctx.throw_reference_error("'" + name + "' is not defined");
                        return Value();
                    } else {
                        // ES1: Assignments without 'var' create deletable global bindings
                        ctx.create_binding(name, right_value, true, true);
                    }
                } else {
                    bool success = ctx.set_binding(name, right_value);
                    if (!success) {
                        if (ctx.is_strict_mode() || ctx.is_lexical_const(name)) {
                            ctx.throw_type_error("Assignment to constant variable '" + name + "'");
                            return Value();
                        }
                    }
                }
                return right_value;
            }
            case Operator::PLUS_ASSIGN: {
                // Use add() method to handle both string concatenation and numeric addition
                Value result = left_value.add(right_value);
                ctx.set_binding(name, result);
                return result;
            }
            case Operator::MINUS_ASSIGN: {
                Value result = Value(left_value.to_number() - right_value.to_number());
                ctx.set_binding(name, result);
                return result;
            }
            case Operator::MUL_ASSIGN: {
                Value result = Value(left_value.to_number() * right_value.to_number());
                ctx.set_binding(name, result);
                return result;
            }
            case Operator::DIV_ASSIGN: {
                Value result = Value(left_value.to_number() / right_value.to_number());
                ctx.set_binding(name, result);
                return result;
            }
            case Operator::MOD_ASSIGN: {
                double left_num = left_value.to_number();
                double right_num = right_value.to_number();
                Value result = Value(std::fmod(left_num, right_num));
                ctx.set_binding(name, result);
                return result;
            }
            case Operator::BITWISE_AND_ASSIGN: {
                Value result = left_value.bitwise_and(right_value);
                ctx.set_binding(name, result);
                return result;
            }
            case Operator::BITWISE_OR_ASSIGN: {
                Value result = left_value.bitwise_or(right_value);
                ctx.set_binding(name, result);
                return result;
            }
            case Operator::BITWISE_XOR_ASSIGN: {
                Value result = left_value.bitwise_xor(right_value);
                ctx.set_binding(name, result);
                return result;
            }
            case Operator::LEFT_SHIFT_ASSIGN: {
                Value result = left_value.left_shift(right_value);
                ctx.set_binding(name, result);
                return result;
            }
            case Operator::RIGHT_SHIFT_ASSIGN: {
                Value result = left_value.right_shift(right_value);
                ctx.set_binding(name, result);
                return result;
            }
            case Operator::UNSIGNED_RIGHT_SHIFT_ASSIGN: {
                Value result = left_value.unsigned_right_shift(right_value);
                ctx.set_binding(name, result);
                return result;
            }
            default:
                ctx.throw_exception(Value(std::string("Unsupported assignment operator")));
                return Value();
        }
        
        return right_value;
    }
    
    if (left_->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
        MemberExpression* member = static_cast<MemberExpression*>(left_.get());

        // Spec: evaluate base, then key expression, then RHS, then ToPropertyKey
        Value object_value = member->get_object()->evaluate(ctx);
        if (ctx.has_exception()) return Value();

        // Evaluate key expression (but defer ToString/ToPropertyKey until after RHS)
        Value computed_key_value;
        if (member->is_computed()) {
            computed_key_value = member->get_property()->evaluate(ctx);
            if (ctx.has_exception()) return Value();
        }

        right_value = right_->evaluate(ctx);
        if (ctx.has_exception()) return Value();
        
        std::string str_value = object_value.is_string() ? object_value.to_string() : "";
        if (str_value.length() >= 6 && str_value.substr(0, 6) == "ARRAY:" && member->is_computed()) {
            Value index_value = member->get_property()->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            int index = static_cast<int>(index_value.to_number());
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


        if (object_value.is_object()) {
            obj = object_value.as_object();
        } else if (object_value.is_function()) {
            obj = object_value.as_function();
        } else if (object_value.is_string() || object_value.is_number() || object_value.is_boolean()) {
            std::string str_val = object_value.is_string() ? object_value.to_string() : "";
            if (object_value.is_string() && str_val.length() >= 7 && str_val.substr(0, 7) == "OBJECT:") {
                is_string_object = true;
            } else {
                // ES5: Check for accessor setter on prototype before failing
                std::string ctor_name = object_value.is_string() ? "String" :
                    (object_value.is_number() ? "Number" : "Boolean");
                std::string prop_name;
                if (member->is_computed()) {
                    Value pv = member->get_property()->evaluate(ctx);
                    if (ctx.has_exception()) return Value();
                    if (pv.is_symbol()) {
                        prop_name = pv.as_symbol()->to_property_key();
                    } else {
                        prop_name = pv.to_string();
                    }
                } else if (member->get_property()->get_type() == ASTNode::Type::IDENTIFIER) {
                    prop_name = static_cast<Identifier*>(member->get_property())->get_name();
                }
                if (!prop_name.empty()) {
                    Value ctor = ctx.get_binding(ctor_name);
                    if (ctor.is_function()) {
                        Value proto = ctor.as_function()->get_property("prototype");
                        if (proto.is_object()) {
                            PropertyDescriptor desc = proto.as_object()->get_property_descriptor(prop_name);
                            if (desc.is_accessor_descriptor() && desc.has_setter()) {
                                Function* setter = dynamic_cast<Function*>(desc.get_setter());
                                if (setter) {
                                    setter->call(ctx, {right_value}, object_value);
                                    return right_value;
                                }
                            }
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
        
        if (member->is_computed() && obj && obj->is_array()) {
            Value prop_value = member->get_property()->evaluate(ctx);
            if (ctx.has_exception()) return Value();

            if (__builtin_expect(prop_value.is_number(), 1)) {
                double idx_double = prop_value.as_number();
                if (__builtin_expect(idx_double >= 0 && idx_double == static_cast<uint32_t>(idx_double) && idx_double < 0xFFFFFFFF, 1)) {
                    uint32_t index = static_cast<uint32_t>(idx_double);
                    obj->set_element(index, right_value);
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
                Object* pobj = prop_value.is_function()
                    ? static_cast<Object*>(prop_value.as_function())
                    : prop_value.as_object();
                Value vo = pobj ? pobj->get_property("valueOf") : Value();
                if (vo.is_function()) {
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

        if (obj && !is_string_object) {
            // Check own descriptor first, then prototype chain for setter
            PropertyDescriptor desc = obj->get_property_descriptor(prop_name);
            if (!desc.is_accessor_descriptor()) {
                // Walk prototype chain for accessor descriptor
                Object* proto = obj->get_prototype();
                while (proto) {
                    PropertyDescriptor proto_desc = proto->get_property_descriptor(prop_name);
                    if (proto_desc.is_accessor_descriptor()) {
                        desc = proto_desc;
                        break;
                    }
                    if (proto_desc.has_value()) break;
                    proto = proto->get_prototype();
                }
            }
            if (desc.is_accessor_descriptor() && desc.has_setter()) {
                Object* setter = desc.get_setter();
                if (setter) {
                    Function* setter_fn = dynamic_cast<Function*>(setter);
                    if (setter_fn) {
                        try {
                            setter_fn->call(ctx, {right_value}, Value(obj));
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
                    if (obj) {
                        // ES5: Strict mode checks for property assignment
                        if (ctx.is_strict_mode()) {
                            PropertyDescriptor desc = obj->get_property_descriptor(prop_name);
                            if (desc.is_accessor_descriptor() && !desc.has_setter()) {
                                ctx.throw_type_error("Cannot set property '" + prop_name + "' which has only a getter");
                                return Value();
                            }
                        }
                        bool success = obj->set_property(prop_name, right_value);
                        if (!success && ctx.is_strict_mode()) {
                            ctx.throw_type_error("Cannot assign to read only property '" + prop_name + "'");
                            return Value();
                        }
                    }
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
                    Value current_value = obj->get_property(prop_name);
                    obj->set_property(prop_name, Value(current_value.to_number() + right_value.to_number()));
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
                Value cur = obj->get_property(prop_name);
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
                bool success = obj->set_property(prop_name, Value(result));
                if (!success && ctx.is_strict_mode()) {
                    ctx.throw_type_error("Cannot assign to read only property '" + prop_name + "'");
                    return Value();
                }
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
void AssignmentExpression::destructuring_assign(Context& ctx, ASTNode* pattern, const Value& source_value) {
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
                // Create object with remaining properties
                auto rest_obj = ObjectFactory::create_object();
                auto keys = source_obj->get_own_property_keys();
                for (const auto& k : keys) {
                    bool already_assigned = false;
                    for (const auto& ak : assigned_keys) {
                        if (ak == k) { already_assigned = true; break; }
                    }
                    if (!already_assigned) {
                        rest_obj->set_property(k, source_obj->get_property(k));
                    }
                }
                assign_to_target(ctx, rest_target, Value(rest_obj.release()));
                if (ctx.has_exception()) return;
                continue;
            }

            // Get property name from key
            std::string prop_name;
            if (prop->computed) {
                Value key_val = prop->key->evaluate(ctx);
                if (ctx.has_exception()) return;
                if (key_val.is_symbol()) {
                    prop_name = key_val.as_symbol()->to_property_key();
                } else {
                    prop_name = key_val.to_string();
                }
            } else if (prop->key->get_type() == ASTNode::Type::IDENTIFIER) {
                prop_name = static_cast<Identifier*>(prop->key.get())->get_name();
            } else if (prop->key->get_type() == ASTNode::Type::STRING_LITERAL) {
                prop_name = static_cast<StringLiteral*>(prop->key.get())->get_value();
            } else if (prop->key->get_type() == ASTNode::Type::NUMBER_LITERAL) {
                prop_name = prop->key->to_string();
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
                if (prop_value.is_undefined()) {
                    prop_value = assign->right_->evaluate(ctx);
                    if (ctx.has_exception()) return;
                }
                target = assign->left_.get();
            }

            // Non-shorthand with AssignmentExpression value: {key: target = default}
            if (!prop->shorthand && target &&
                target->get_type() == ASTNode::Type::ASSIGNMENT_EXPRESSION) {
                auto* assign = static_cast<AssignmentExpression*>(target);
                if (prop_value.is_undefined()) {
                    prop_value = assign->right_->evaluate(ctx);
                    if (ctx.has_exception()) return;
                }
                target = assign->left_.get();
            }

            assign_to_target(ctx, target, prop_value);
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
            // ES6: Check for Symbol.iterator on non-array objects
            if (source_arr && !source_arr->is_array()) {
                Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
                if (iter_sym) {
                    Value iter_method = source_arr->get_property(iter_sym->to_property_key());
                    if (iter_method.is_function()) {
                        Value iter_obj = iter_method.as_function()->call(ctx, {}, source_value);
                        if (!ctx.has_exception() && iter_obj.is_object()) {
                            Value next_fn = iter_obj.as_object()->get_property("next");
                            if (next_fn.is_function()) {
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

                                if (has_rest) {
                                    // Rest: collect all remaining into temp array
                                    auto temp = ObjectFactory::create_array(0);
                                    uint32_t cnt = 0;
                                    bool iter_done = false;
                                    for (uint32_t ii = 0; ii < 100000; ii++) {
                                        Value res = next_fn.as_function()->call(ctx, {}, iter_obj);
                                        if (ctx.has_exception()) { close_iter(); return; }
                                        if (!res.is_object()) { iter_done = true; break; }
                                        Value done_v = res.as_object()->get_property("done");
                                        if (ctx.has_exception()) { close_iter(); return; }
                                        if (done_v.to_boolean()) { iter_done = true; break; }
                                        Value val_v = res.as_object()->get_property("value");
                                        if (ctx.has_exception()) { close_iter(); return; }
                                        temp->set_element(cnt++, val_v);
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
                                            mem->get_object()->evaluate(ctx);
                                            if (ctx.has_exception()) { close_iter(); return; }
                                            if (mem->is_computed()) {
                                                mem->get_property()->evaluate(ctx);
                                                if (ctx.has_exception()) { close_iter(); return; }
                                            }
                                        }
                                        // Now call next()
                                        Value res = next_fn.as_function()->call(ctx, {}, iter_obj);
                                        if (ctx.has_exception()) { close_iter(); return; }
                                        if (!res.is_object()) { iter_done = true; break; }
                                        Value done_v = res.as_object()->get_property("done");
                                        if (ctx.has_exception()) { close_iter(); return; }
                                        if (done_v.to_boolean()) { iter_done = true; break; }
                                        Value val_v = res.as_object()->get_property("value");
                                        if (ctx.has_exception()) { close_iter(); return; }
                                        temp->set_element(cnt++, val_v);
                                    }
                                    if (!iter_done) { close_iter(); }
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
                assign_to_target(ctx, rest_target, Value(rest_arr.release()));
                if (ctx.has_exception()) return;
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
                if (elem_value.is_undefined()) {
                    elem_value = assign->right_->evaluate(ctx);
                    if (ctx.has_exception()) return;
                }
                target = assign->left_.get();
            }

            assign_to_target(ctx, target, elem_value);
            if (ctx.has_exception()) return;
        }
    }
}

// Helper: assign a value to a target node (Identifier, MemberExpression, or nested pattern)
void AssignmentExpression::assign_to_target(Context& ctx, ASTNode* target, const Value& value) {
    if (!target) return;

    if (target->get_type() == ASTNode::Type::IDENTIFIER) {
        std::string name = static_cast<Identifier*>(target)->get_name();
        if (ctx.has_binding(name)) {
            bool ok = ctx.set_binding(name, value);
            if (!ok) {
                if (ctx.is_strict_mode() || ctx.is_lexical_const(name)) {
                    ctx.throw_type_error("Assignment to constant variable '" + name + "'");
                }
            }
        } else {
            if (ctx.is_strict_mode()) {
                ctx.throw_reference_error("'" + name + "' is not defined");
                return;
            }
            ctx.create_binding(name, value, true);
        }
    } else if (target->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
        auto* member = static_cast<MemberExpression*>(target);
        Value obj_val = member->get_object()->evaluate(ctx);
        if (ctx.has_exception()) return;
        if (obj_val.is_object_like()) {
            Object* obj = obj_val.is_object() ? obj_val.as_object()
                                              : static_cast<Object*>(obj_val.as_function());
            std::string prop_name;
            if (member->is_computed()) {
                Value key_val = member->get_property()->evaluate(ctx);
                if (ctx.has_exception()) return;
                if (key_val.is_symbol()) {
                    prop_name = key_val.as_symbol()->to_property_key();
                } else {
                    prop_name = key_val.to_string();
                }
            } else if (member->get_property()->get_type() == ASTNode::Type::IDENTIFIER) {
                prop_name = static_cast<Identifier*>(member->get_property())->get_name();
            }
            obj->set_property(prop_name, value);
        }
    } else if (target->get_type() == ASTNode::Type::OBJECT_LITERAL ||
               target->get_type() == ASTNode::Type::ARRAY_LITERAL) {
        // Nested destructuring
        destructuring_assign(ctx, target, value);
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
    }
    return left_->to_string() + op_str + right_->to_string();
}

std::unique_ptr<ASTNode> AssignmentExpression::clone() const {
    return std::make_unique<AssignmentExpression>(
        left_->clone(), operator_, right_->clone(), start_, end_
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

Value DestructuringAssignment::evaluate_with_value(Context& ctx, const Value& source_value) {
    if (type_ == Type::ARRAY) {
        // ES6: Strings are iterable and can be array-destructured
        bool is_string_source = source_value.is_string();
        std::string str_src;
        Object* array_obj = nullptr;

        // For codepoint-aware string destructuring
        std::vector<std::string> str_cps;

        if (is_string_source) {
            str_src = source_value.as_string()->str();
            // Split into UTF-8 codepoints
            size_t pos = 0;
            while (pos < str_src.length()) {
                unsigned char ch = static_cast<unsigned char>(str_src[pos]);
                size_t cl = 1;
                if (ch >= 0xF0) cl = 4;
                else if (ch >= 0xE0) cl = 3;
                else if (ch >= 0xC0) cl = 2;
                if (pos + cl > str_src.length()) cl = 1;
                str_cps.push_back(str_src.substr(pos, cl));
                pos += cl;
            }
        } else if (source_value.is_object_like()) {
            array_obj = source_value.is_object() ? source_value.as_object()
                                                 : static_cast<Object*>(source_value.as_function());
        } else {
            ctx.throw_type_error("Cannot destructure non-object as array");
            return Value();
        }

        // ES6: Check for Symbol.iterator -- use iterator protocol for all iterable objects.
        // For arrays: Symbol.iterator MUST be callable (throw TypeError if deleted).
        // For non-arrays: only use iterator if Symbol.iterator is present.
        if (!is_string_source && array_obj) {
            Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
            if (iter_sym) {
                Value iter_method = array_obj->get_property(iter_sym->to_property_key());
                if (array_obj->is_array() && !iter_method.is_function()) {
                    ctx.throw_type_error("Cannot destructure: object is not iterable");
                    return Value();
                }
                if (iter_method.is_function()) {
                    Value iterator_obj = iter_method.as_function()->call(ctx, {}, source_value);
                    if (ctx.has_exception()) return Value();
                    if (iterator_obj.is_object()) {
                        Object* iterator = iterator_obj.as_object();
                        Value next_method = iterator->get_property("next");
                        if (next_method.is_function()) {
                            // Determine how many elements we need
                            bool has_rest = false;
                            size_t needed = targets_.size();
                            for (size_t ti = 0; ti < targets_.size(); ti++) {
                                const std::string& tn = targets_[ti]->get_name();
                                if (tn.length() >= 3 && tn.substr(0, 3) == "...") {
                                    has_rest = true;
                                    break;
                                }
                            }
                            auto temp_arr = ObjectFactory::create_array(0);
                            uint32_t count = 0;
                            bool iterator_done = false;
                            Context* prev_oc2 = Object::current_context_;
                            Object::current_context_ = &ctx;
                            if (has_rest) {
                                // Rest: collect all elements
                                for (uint32_t iter_i = 0; iter_i < 100000; iter_i++) {
                                    Value result = next_method.as_function()->call(ctx, {}, iterator_obj);
                                    if (ctx.has_exception()) { Object::current_context_ = prev_oc2; return Value(); }
                                    if (!result.is_object()) { iterator_done = true; break; }
                                    Value dv2 = result.as_object()->get_property("done");
                                    if (ctx.has_exception()) { Object::current_context_ = prev_oc2; return Value(); }
                                    if (dv2.to_boolean()) { iterator_done = true; break; }
                                    Value vv2 = result.as_object()->get_property("value");
                                    if (ctx.has_exception()) { Object::current_context_ = prev_oc2; return Value(); }
                                    temp_arr->set_element(count++, vv2);
                                }
                            } else {
                                // No rest: collect only needed elements
                                for (uint32_t iter_i = 0; iter_i < needed; iter_i++) {
                                    Value result = next_method.as_function()->call(ctx, {}, iterator_obj);
                                    if (ctx.has_exception()) { Object::current_context_ = prev_oc2; return Value(); }
                                    if (!result.is_object()) { iterator_done = true; break; }
                                    Value dv3 = result.as_object()->get_property("done");
                                    if (ctx.has_exception()) { Object::current_context_ = prev_oc2; return Value(); }
                                    if (dv3.to_boolean()) { iterator_done = true; break; }
                                    Value vv3 = result.as_object()->get_property("value");
                                    if (ctx.has_exception()) { Object::current_context_ = prev_oc2; return Value(); }
                                    temp_arr->set_element(count++, vv3);
                                }
                            }
                            Object::current_context_ = prev_oc2;
                            // ES6: Iterator closing -- if not exhausted, call return()
                            // Per spec, return() must return an Object or throw TypeError
                            if (!iterator_done) {
                                Object::current_context_ = &ctx;
                                Value return_method = iterator->get_property("return");
                                Object::current_context_ = prev_oc2;
                                if (return_method.is_function()) {
                                    Value ret_val = return_method.as_function()->call(ctx, {}, iterator_obj);
                                    if (!ctx.has_exception() && !ret_val.is_object() && !ret_val.is_function()) {
                                        ctx.throw_type_error("Iterator return() returned a non-object value");
                                        return Value();
                                    }
                                }
                                if (ctx.has_exception()) { return Value(); }
                            }
                            temp_arr->set_length(count);
                            array_obj = temp_arr.release();
                        }
                    }
                }
            }
        }

        uint32_t src_len = is_string_source ? static_cast<uint32_t>(str_cps.size())
                                            : array_obj->get_length();

        if (true) {
            for (size_t i = 0; i < targets_.size(); i++) {
                const std::string& var_name = targets_[i]->get_name();

                if (var_name.empty()) {
                    continue;
                }

                if (var_name.length() >= 3 && var_name.substr(0, 3) == "...") {
                    std::string rest_name = var_name.substr(3);

                    auto rest_array = ObjectFactory::create_array(0);
                    uint32_t rest_index = 0;

                    for (size_t j = i; j < src_len; j++) {
                        Value rest_element;
                        if (is_string_source) {
                            rest_element = (j < str_cps.size()) ? Value(str_cps[j]) : Value();
                        } else {
                            rest_element = array_obj->get_element(static_cast<uint32_t>(j));
                        }
                        rest_array->set_element(rest_index++, rest_element);
                    }

                    rest_array->set_length(rest_index);

                    if (rest_name == "__nested_rest__" && nested_rest_pattern_) {
                        // ...[pattern] - apply nested destructuring to rest array
                        DestructuringAssignment* nested =
                            static_cast<DestructuringAssignment*>(nested_rest_pattern_.get());
                        nested->evaluate_with_value(ctx, Value(rest_array.release()));
                    } else {
                        if (!ctx.has_binding(rest_name)) {
                            ctx.create_binding(rest_name, Value(rest_array.release()), true);
                        } else {
                            ctx.set_binding(rest_name, Value(rest_array.release()));
                        }
                    }

                    break;
                } else if (var_name.length() >= 14 && var_name.substr(0, 14) == "__nested_vars:") {
                    Value nested_array;
                    if (is_string_source) {
                        nested_array = (i < str_cps.size()) ? Value(str_cps[i]) : Value();
                    } else {
                        nested_array = array_obj->get_element(static_cast<uint32_t>(i));
                    }
                    if (nested_array.is_undefined()) {
                        for (const auto& dv : default_values_) {
                            if (dv.index == i) {
                                nested_array = dv.expr->evaluate(ctx);
                                if (ctx.has_exception()) return Value();
                                break;
                            }
                        }
                    }
                    if (nested_array.is_null() || nested_array.is_undefined()) {
                        ctx.throw_type_error(std::string("Cannot destructure ") + (nested_array.is_null() ? "null" : "undefined") + " as array");
                        return Value();
                    }
                    if (nested_array.is_object()) {
                        Object* nested_obj = nested_array.as_object();

                        std::string vars_string = var_name.substr(14);

                        std::vector<std::string> nested_var_names;
                        std::string current_var = "";
                        for (char c : vars_string) {
                            if (c == ',') {
                                // Always push (even empty = elision sentinel)
                                nested_var_names.push_back(current_var);
                                current_var = "";
                            } else {
                                current_var += c;
                            }
                        }
                        // Push last entry only if non-empty or sentinel
                        if (!current_var.empty() || (!vars_string.empty() && vars_string.back() == ','))
                            nested_var_names.push_back(current_var);

                        // Use iterator protocol if available (e.g. generators), else direct access
                        std::vector<Value> nested_elements;
                        Symbol* nest_iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
                        bool used_iterator = false;
                        if (nest_iter_sym && !nested_obj->is_array()) {
                            Value nest_iter_method = nested_obj->get_property(nest_iter_sym->to_property_key());
                            if (nest_iter_method.is_function()) {
                                Value nest_iter_obj = nest_iter_method.as_function()->call(ctx, {}, nested_array);
                                if (!ctx.has_exception() && nest_iter_obj.is_object()) {
                                    Value nest_next = nest_iter_obj.as_object()->get_property("next");
                                    if (nest_next.is_function()) {
                                        used_iterator = true;
                                        for (size_t ni = 0; ni < nested_var_names.size(); ni++) {
                                            if (nested_var_names[ni].length() >= 3 && nested_var_names[ni].substr(0,3) == "...") {
                                                auto rest_a = ObjectFactory::create_array(0); uint32_t ri2 = 0;
                                                for (;;) {
                                                    Value nr = nest_next.as_function()->call(ctx, {}, nest_iter_obj);
                                                    if (ctx.has_exception()) return Value();
                                                    if (!nr.is_object()) break;
                                                    if (nr.as_object()->get_property("done").to_boolean()) break;
                                                    rest_a->set_element(ri2++, nr.as_object()->get_property("value"));
                                                }
                                                rest_a->set_length(ri2);
                                                nested_elements.push_back(Value(rest_a.release()));
                                            } else {
                                                Value nr = nest_next.as_function()->call(ctx, {}, nest_iter_obj);
                                                if (ctx.has_exception()) return Value();
                                                if (!nr.is_object() || nr.as_object()->get_property("done").to_boolean())
                                                    nested_elements.push_back(Value());
                                                else
                                                    nested_elements.push_back(nr.as_object()->get_property("value"));
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        for (size_t j = 0; j < nested_var_names.size(); j++) {
                            const std::string& nested_var_name = nested_var_names[j];
                            Value val_to_bind;
                            if (nested_var_name.length() >= 3 && nested_var_name.substr(0, 3) == "...") {
                                std::string rest_binding = nested_var_name.substr(3);
                                if (used_iterator && j < nested_elements.size()) {
                                    val_to_bind = nested_elements[j];
                                } else {
                                    auto rest_arr = ObjectFactory::create_array(0);
                                    uint32_t ri = 0;
                                    for (size_t rj = j; rj < nested_obj->get_length(); rj++) {
                                        rest_arr->set_element(ri++, nested_obj->get_element(static_cast<uint32_t>(rj)));
                                    }
                                    rest_arr->set_length(ri);
                                    val_to_bind = Value(rest_arr.release());
                                }
                                if (!ctx.has_binding(rest_binding)) ctx.create_binding(rest_binding, val_to_bind, true);
                                else ctx.set_binding(rest_binding, val_to_bind);
                                break;
                            } else if (nested_var_name.empty() || nested_var_name == "\x01") {
                                // Elision or empty -- skip binding but value was consumed from iterator
                            } else if (used_iterator && j < nested_elements.size()) {
                                val_to_bind = nested_elements[j];
                                if (!ctx.has_binding(nested_var_name)) ctx.create_binding(nested_var_name, val_to_bind, true);
                                else ctx.set_binding(nested_var_name, val_to_bind);
                            } else if (!used_iterator && j < nested_obj->get_length()) {
                                val_to_bind = nested_obj->get_element(static_cast<uint32_t>(j));
                                if (!ctx.has_binding(nested_var_name)) ctx.create_binding(nested_var_name, val_to_bind, true);
                                else ctx.set_binding(nested_var_name, val_to_bind);
                            }
                        }
                    }
                } else if (var_name.length() >= 13 && var_name.substr(0, 13) == "__nested_obj:") {
                    // Nested object destructuring in array: [a, {x:b, c}]
                    Value element;
                    if (is_string_source) {
                        element = (i < str_cps.size()) ? Value(str_cps[i]) : Value();
                    } else {
                        element = array_obj->get_element(static_cast<uint32_t>(i));
                    }
                    if (element.is_undefined()) {
                        for (const auto& dv : default_values_) {
                            if (dv.index == i) {
                                element = dv.expr->evaluate(ctx);
                                if (ctx.has_exception()) return Value();
                                break;
                            }
                        }
                    }
                    if (element.is_null() || element.is_undefined()) {
                        ctx.throw_type_error(std::string("Cannot destructure ") + (element.is_null() ? "null" : "undefined") + " as object");
                        return Value();
                    }
                    if (element.is_object() || element.is_function()) {
                        Object* obj = element.is_function() ?
                            static_cast<Object*>(element.as_function()) :
                            element.as_object();
                        // Parse mappings: prop1>var1,prop2>var2
                        std::string mappings_str = var_name.substr(13);
                        std::vector<std::pair<std::string,std::string>> mappings;
                        std::string current = "";
                        for (size_t ci = 0; ci <= mappings_str.length(); ci++) {
                            char c = (ci < mappings_str.length()) ? mappings_str[ci] : ',';
                            if (c == ',') {
                                size_t arrow = current.find('>');
                                if (arrow != std::string::npos) {
                                    mappings.emplace_back(current.substr(0, arrow), current.substr(arrow + 1));
                                }
                                current = "";
                            } else {
                                current += c;
                            }
                        }
                        for (const auto& m : mappings) {
                            Value val = obj->get_property(m.first);
                            if (!ctx.has_binding(m.second)) {
                                ctx.create_binding(m.second, val, true);
                            } else {
                                ctx.set_binding(m.second, val);
                            }
                        }
                    }
                } else {
                    Value element;
                    if (is_string_source) {
                        element = (i < str_cps.size()) ? Value(str_cps[i]) : Value();
                    } else {
                        element = array_obj->get_element(static_cast<uint32_t>(i));
                    }

                    if (element.is_undefined()) {
                        for (const auto& default_val : default_values_) {
                            if (default_val.index == i) {
                                element = default_val.expr->evaluate(ctx);
                                if (ctx.has_exception()) return Value();
                                if (element.is_function() && is_anonymous_function_def(default_val.expr.get()) &&
                                    (element.as_function()->get_name().empty() || element.as_function()->get_name() == "<arrow>")) {
                                    element.as_function()->set_name(var_name);
                                }
                                break;
                            }
                        }
                    }

                    if (!ctx.has_binding(var_name)) {
                        ctx.create_binding(var_name, element, true);
                    } else {
                        ctx.set_binding(var_name, element);
                    }
                }
            }
        }
    } else {
        if (source_value.is_object_like()) {
            Object* obj = source_value.is_object() ? source_value.as_object()
                                                    : static_cast<Object*>(source_value.as_function());

            if (!handle_complex_object_destructuring(obj, ctx)) {
                return Value();
            }
        } else if (source_value.is_number() || source_value.is_string() || source_value.is_boolean()) {
            // ES6: Primitive boxing for object destructuring
            std::string ctor_name = source_value.is_string() ? "String"
                                  : source_value.is_number() ? "Number" : "Boolean";
            Value ctor = ctx.get_binding(ctor_name);
            if (ctor.is_function()) {
                Value proto_val = ctor.as_function()->get_property("prototype");
                if (proto_val.is_object()) {
                    Object* proto = proto_val.as_object();
                    // Look up each property mapping on the prototype
                    for (const auto& mapping : property_mappings_) {
                        Value prop_value = proto->get_property(mapping.property_name);
                        if (!ctx.has_binding(mapping.variable_name)) {
                            ctx.create_binding(mapping.variable_name, prop_value, true);
                        } else {
                            ctx.set_binding(mapping.variable_name, prop_value);
                        }
                    }
                    // Also handle shorthand targets
                    for (const auto& target : targets_) {
                        const std::string& name = target->get_name();
                        if (name.empty() || name.find("...") == 0 || name.find("__") == 0) continue;
                        // Only if not already handled by property_mappings_
                        bool in_mappings = false;
                        for (const auto& m : property_mappings_) {
                            if (m.variable_name == name) { in_mappings = true; break; }
                        }
                        if (!in_mappings) {
                            Value prop_value = proto->get_property(name);
                            if (!ctx.has_binding(name)) {
                                ctx.create_binding(name, prop_value, true);
                            } else {
                                ctx.set_binding(name, prop_value);
                            }
                        }
                    }
                }
            }
        } else {
            ctx.throw_type_error("Cannot destructure non-object");
            return Value();
        }
    }
    
    return source_value;
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

bool DestructuringAssignment::handle_complex_object_destructuring(Object* obj, Context& ctx) {
    // Helper: property getters run in Object::current_context_ which may differ from ctx.
    // After each get_property call, propagate any exception from current_context_ to ctx.
    auto check_getter_exc = [&]() -> bool {
        if (ctx.has_exception()) return true;
        Context* cur = Object::current_context_;
        if (cur && cur != &ctx && cur->has_exception()) {
            ctx.throw_exception(cur->get_exception(), true);
            cur->clear_exception();
            return true;
        }
        return false;
    };

    for (const auto& mapping : property_mappings_) {

        if (mapping.variable_name.find("__nested:") != std::string::npos ||
            mapping.variable_name.find(":__nested:") != std::string::npos) {
        }
        Value prop_value;
        if (mapping.property_name.length() > 11 && mapping.property_name.substr(0, 11) == "__computed:") {
            if (mapping.computed_key) {
                Value key_val = mapping.computed_key->evaluate(ctx);
                if (ctx.has_exception()) return false;
                prop_value = obj->get_property(key_val.to_string());
                if (check_getter_exc()) return false;
            }
        } else {
            prop_value = obj->get_property(mapping.property_name);
            if (check_getter_exc()) return false;
        }

        // Apply default if property is undefined (for nested patterns too)
        if (prop_value.is_undefined()) {
            for (const auto& dv : default_values_) {
                if (dv.index < targets_.size()) {
                    const std::string& tname = targets_[dv.index]->get_name();
                    if (tname == mapping.property_name || tname == mapping.variable_name) {
                        prop_value = dv.expr->evaluate(ctx);
                        if (ctx.has_exception()) return false;
                        if (prop_value.is_function() && is_anonymous_function_def(dv.expr.get()) &&
                            (prop_value.as_function()->get_name().empty() || prop_value.as_function()->get_name() == "<arrow>")) {
                            prop_value.as_function()->set_name(mapping.variable_name);
                        }
                        break;
                    }
                }
            }
        }

        // Handle nested array-in-object: {x: [a, b]} encoded as __nested_array:a,b
        if (mapping.variable_name.length() > 15 && mapping.variable_name.substr(0, 15) == "__nested_array:") {
            if (prop_value.is_null() || prop_value.is_undefined()) {
                ctx.throw_type_error(std::string("Cannot destructure ") + (prop_value.is_null() ? "null" : "undefined") + " as array");
                return false;
            }
            std::string vars_str = mapping.variable_name.substr(15);
            // Split vars by comma
            std::vector<std::string> var_names;
            std::string current;
            for (size_t ci = 0; ci < vars_str.length(); ++ci) {
                if (vars_str[ci] == ',') {
                    if (!current.empty()) { var_names.push_back(current); current.clear(); }
                } else {
                    current += vars_str[ci];
                }
            }
            if (!current.empty()) var_names.push_back(current);

            if (prop_value.is_object()) {
                Object* arr_obj = prop_value.as_object();
                for (size_t ai = 0; ai < var_names.size(); ++ai) {
                    Value elem = arr_obj->get_element(static_cast<uint32_t>(ai));
                    if (!ctx.has_binding(var_names[ai])) {
                        ctx.create_binding(var_names[ai], elem, true);
                    } else {
                        ctx.set_binding(var_names[ai], elem);
                    }
                }
            } else {
                for (const auto& vn : var_names) {
                    if (!ctx.has_binding(vn)) {
                        ctx.create_binding(vn, Value(), true);
                    } else {
                        ctx.set_binding(vn, Value());
                    }
                }
            }
            continue;
        }

        if ((mapping.variable_name.length() > 9 && mapping.variable_name.substr(0, 9) == "__nested:") ||
            mapping.variable_name.find(":__nested:") != std::string::npos ||
            mapping.variable_name.find(':') != std::string::npos) {

            if (prop_value.is_null() || prop_value.is_undefined()) {
                ctx.throw_type_error(std::string("Cannot destructure ") + (prop_value.is_null() ? "null" : "undefined") + " as object");
                return false;
            }

            if (mapping.variable_name.find(":__nested:") != std::string::npos) {

                if (prop_value.is_object()) {
                    Object* nested_obj = prop_value.as_object();
                    handle_infinite_depth_destructuring(nested_obj, mapping.variable_name, ctx);
                } else {
                }
                continue;
            } else if (mapping.variable_name.find(':') != std::string::npos &&
                      mapping.variable_name.find("__nested:") == std::string::npos) {

                if (prop_value.is_object() || prop_value.is_function()) {
                    Object* nested_obj = prop_value.is_function()
                        ? static_cast<Object*>(prop_value.as_function())
                        : prop_value.as_object();

                    // "prefix:x,y,z" means shorthand multi-var pattern -- extract each var
                    if (mapping.variable_name.find(',') != std::string::npos) {
                        size_t colon = mapping.variable_name.find(':');
                        std::string vars_part = (colon != std::string::npos)
                            ? mapping.variable_name.substr(colon + 1)
                            : mapping.variable_name;
                        std::string cur;
                        for (size_t ci = 0; ci <= vars_part.size(); ++ci) {
                            char c = (ci < vars_part.size()) ? vars_part[ci] : ',';
                            if (c == ',') {
                                if (!cur.empty()) {
                                    Value val = nested_obj->get_property(cur);
                                    if (!ctx.has_binding(cur)) ctx.create_binding(cur, val, true);
                                    else ctx.set_binding(cur, val);
                                    cur.clear();
                                }
                            } else {
                                cur += c;
                            }
                        }
                    } else {
                        handle_infinite_depth_destructuring(nested_obj, mapping.variable_name, ctx);
                    }
                }
                continue;
            }

            std::string vars_string = mapping.variable_name.substr(9);

            std::vector<std::string> nested_var_names;
            std::string current_var = "";
            int nested_depth = 0;

            for (size_t i = 0; i < vars_string.length(); ++i) {
                char c = vars_string[i];

                if (i + 9 <= vars_string.length() &&
                    vars_string.substr(i, 9) == "__nested:") {
                    nested_depth++;
                    current_var += "__nested:";
                    i += 8;
                } else if (c == ',' && nested_depth == 0) {
                    if (!current_var.empty()) {
                        nested_var_names.push_back(current_var);
                        current_var = "";
                    }
                } else {
                    current_var += c;
                    if (nested_depth > 0 && i == vars_string.length() - 1) {
                        nested_depth = 0;
                    }
                }
            }
            if (!current_var.empty()) {
                nested_var_names.push_back(current_var);
            }

            if (prop_value.is_object()) {
                Object* nested_obj = prop_value.as_object();


                std::vector<std::string> property_aware_var_names = nested_var_names;

                bool found_nested_mappings = false;

                for (const auto& our_mapping : property_mappings_) {
                    if (our_mapping.property_name == mapping.property_name &&
                        our_mapping.variable_name.find("__nested:") == 0) {

                        std::string vars_part = our_mapping.variable_name.substr(9);

                        std::vector<std::string> enhanced_vars;
                        std::stringstream ss(vars_part);
                        std::string var;

                        while (std::getline(ss, var, ',')) {
                            enhanced_vars.push_back(var);
                        }

                        property_aware_var_names = enhanced_vars;
                        found_nested_mappings = true;
                        break;
                    }
                }

                std::vector<std::string> smart_var_names = nested_var_names;





                bool has_property_renaming = false;
                std::map<std::string, std::string> detected_mappings;

                for (const auto& target : targets_) {
                    std::string target_name = target->get_name();
                    if (target_name == mapping.property_name) {
                        break;
                    }
                }

                std::vector<std::string> processed_var_names;
                for (const std::string& var_name : smart_var_names) {
                    size_t colon_pos = var_name.find(':');
                    bool is_malformed_nested = false;
                    if (colon_pos != std::string::npos) {
                        std::string after_colon = var_name.substr(colon_pos + 1);
                        if (after_colon.length() > 9 && after_colon.substr(0, 9) == "__nested:") {
                            is_malformed_nested = true;
                        }
                    }

                    if (!is_malformed_nested && var_name.find(':') != std::string::npos && var_name.find("__nested:") != 0) {
                        processed_var_names.push_back(var_name);
                        has_property_renaming = true;
                    } else {
                        processed_var_names.push_back(var_name);
                    }
                }

                for (size_t i = 0; i < smart_var_names.size(); ++i) {
                }

                if (has_property_renaming) {
                    handle_nested_object_destructuring_with_mappings(nested_obj, processed_var_names, ctx);
                } else {
                    for (const std::string& var_name : smart_var_names) {

                        bool is_nested_pattern = false;
                        if (var_name.length() > 9 && var_name.substr(0, 9) == "__nested:") {
                            is_nested_pattern = true;
                        } else {
                            size_t colon_pos = var_name.find(':');
                            if (colon_pos != std::string::npos) {
                                std::string after_colon = var_name.substr(colon_pos + 1);
                                if (after_colon.length() > 9 && after_colon.substr(0, 9) == "__nested:") {
                                    is_nested_pattern = true;
                                }
                            }
                        }

                        if (is_nested_pattern) {
                            handle_infinite_depth_destructuring(nested_obj, var_name, ctx);
                        } else {
                            Value prop_value = nested_obj->get_property(var_name);
                            if (!ctx.has_binding(var_name)) {
                                ctx.create_binding(var_name, prop_value, true);
                            } else {
                                ctx.set_binding(var_name, prop_value);
                            }
                        }
                    }
                }
            }
        } else {
            // Apply default value if property is undefined: {x: a = expr}
            if (prop_value.is_undefined()) {
                for (size_t i = 0; i < targets_.size(); i++) {
                    if (targets_[i]->get_name() == mapping.variable_name) {
                        for (const auto& dv : default_values_) {
                            if (dv.index == i) {
                                prop_value = dv.expr->evaluate(ctx);
                                if (ctx.has_exception()) return false;
                                break;
                            }
                        }
                        break;
                    }
                }
            }
            bool binding_created = false;
            if (!ctx.has_binding(mapping.variable_name)) {
                binding_created = ctx.create_binding(mapping.variable_name, prop_value, true);
            } else {
                ctx.set_binding(mapping.variable_name, prop_value);
                binding_created = true;
            }

            if (!binding_created) {
            }
        }
    }
    
    std::set<std::string> extracted_props;
    
    for (const auto& mapping : property_mappings_) {
        extracted_props.insert(mapping.property_name);
    }
    
    for (const auto& target : targets_) {
        std::string prop_name = target->get_name();

        if (prop_name.length() >= 3 && prop_name.substr(0, 3) == "...") {
            std::string rest_name = prop_name.substr(3);
            
            auto rest_obj = std::make_unique<Object>(Object::ObjectType::Ordinary);
            
            auto keys = obj->get_own_property_keys();
            for (const auto& key : keys) {
                if (extracted_props.find(key) == extracted_props.end()) {
                    // Only copy enumerable own properties (spec: CopyDataProperties)
                    PropertyDescriptor pd = obj->get_property_descriptor(key);
                    if (pd.is_data_descriptor() && !pd.is_enumerable()) continue;
                    Value prop_value = obj->get_property(key);
                    rest_obj->set_property(key, prop_value);
                }
            }
            
            if (!ctx.has_binding(rest_name)) {
                ctx.create_binding(rest_name, Value(rest_obj.release()), true);
            } else {
                ctx.set_binding(rest_name, Value(rest_obj.release()));
            }
            
            continue;
        }
        
        bool has_mapping = false;
        for (const auto& mapping : property_mappings_) {
            if (mapping.property_name == prop_name || mapping.variable_name == prop_name) {
                has_mapping = true;
                break;
            }
        }

        if (!has_mapping) {
            if (prop_name.length() >= 9 && prop_name.substr(0, 9) == "__nested:") {

                std::string vars_string = prop_name.substr(9);

                std::vector<std::string> nested_var_names;
                std::string current_var = "";
                int nested_depth = 0;

                for (size_t i = 0; i < vars_string.length(); ++i) {
                    char c = vars_string[i];

                    if (i + 9 <= vars_string.length() &&
                        vars_string.substr(i, 9) == "__nested:") {
                        nested_depth++;
                        current_var += "__nested:";
                        i += 8;
                    } else if (c == ',' && nested_depth == 0) {
                        if (!current_var.empty()) {
                            nested_var_names.push_back(current_var);
                            current_var = "";
                        }
                    } else {
                        current_var += c;
                        if (nested_depth > 0 && i == vars_string.length() - 1) {
                            nested_depth = 0;
                        }
                    }
                }
                if (!current_var.empty()) {
                    nested_var_names.push_back(current_var);
                }

                std::string actual_prop = "";
                for (const auto& mapping : property_mappings_) {
                    if (mapping.variable_name == prop_name) {
                        actual_prop = mapping.property_name;
                        break;
                    }
                }

                if (!actual_prop.empty()) {
                    Value nested_object = obj->get_property(actual_prop);
                    if (nested_object.is_object()) {
                        Object* nested_obj = nested_object.as_object();

                        for (const std::string& var_name : nested_var_names) {
                            if (var_name.length() > 9 && var_name.substr(0, 9) == "__nested:") {
                                handle_infinite_depth_destructuring(nested_obj, var_name, ctx);
                            } else {
                                Value prop_value = nested_obj->get_property(var_name);
                                if (!ctx.has_binding(var_name)) {
                                    ctx.create_binding(var_name, prop_value, true);
                                } else {
                                    ctx.set_binding(var_name, prop_value);
                                }
                            }
                        }
                    }
                }
            } else {
                Value prop_value = obj->get_property(prop_name);
                if (!ctx.has_exception() && Object::current_context_ && Object::current_context_ != &ctx
                        && Object::current_context_->has_exception()) {
                    ctx.throw_exception(Object::current_context_->get_exception(), true);
                    Object::current_context_->clear_exception();
                }
                if (ctx.has_exception()) return false;

                // Apply default value if property is undefined: {a = expr}
                if (prop_value.is_undefined()) {
                    for (size_t ti = 0; ti < targets_.size(); ti++) {
                        if (targets_[ti]->get_name() == prop_name) {
                            for (const auto& dv : default_values_) {
                                if (dv.index == ti) {
                                    prop_value = dv.expr->evaluate(ctx);
                                    if (ctx.has_exception()) return false;
                                    if (prop_value.is_function() && is_anonymous_function_def(dv.expr.get()) &&
                                        (prop_value.as_function()->get_name().empty() || prop_value.as_function()->get_name() == "<arrow>")) {
                                        prop_value.as_function()->set_name(prop_name);
                                    }
                                    break;
                                }
                            }
                            break;
                        }
                    }
                }

                extracted_props.insert(prop_name);

                if (!ctx.has_binding(prop_name)) {
                    ctx.create_binding(prop_name, prop_value, true);
                } else {
                    ctx.set_binding(prop_name, prop_value);
                }
            }
        }
    }
    
    return true;
}

void DestructuringAssignment::handle_nested_object_destructuring(Object* nested_obj, const std::vector<std::string>& var_names, Context& ctx) {

    for (const std::string& var_name : var_names) {

        if (var_name.length() > 9 && var_name.substr(0, 9) == "__nested:") {
            std::string deeper_vars_string = var_name.substr(9);

            std::vector<std::string> deeper_var_names;
            std::string current_var = "";
            int nested_depth = 0;

            for (size_t i = 0; i < deeper_vars_string.length(); ++i) {
                char c = deeper_vars_string[i];

                if (i + 9 <= deeper_vars_string.length() &&
                    deeper_vars_string.substr(i, 9) == "__nested:") {
                    nested_depth++;
                    current_var += "__nested:";
                    i += 8;
                } else if (c == ',' && nested_depth == 0) {
                    if (!current_var.empty()) {
                        deeper_var_names.push_back(current_var);
                        current_var = "";
                    }
                } else {
                    current_var += c;
                    if (nested_depth > 0 && i == deeper_vars_string.length() - 1) {
                        nested_depth = 0;
                    }
                }
            }
            if (!current_var.empty()) {
                deeper_var_names.push_back(current_var);
            }

            for (const auto& property_name : nested_obj->get_own_property_keys()) {
                Value property_value = nested_obj->get_property(property_name);
                if (property_value.is_object()) {
                    Object* deeper_obj = property_value.as_object();
                    for (const std::string& deep_var_name : deeper_var_names) {
                        if (deep_var_name.length() > 9 && deep_var_name.substr(0, 9) == "__nested:") {
                            handle_infinite_depth_destructuring(deeper_obj, deep_var_name, ctx);
                        } else {
                            Value prop_value = deeper_obj->get_property(deep_var_name);
                            if (!ctx.has_binding(deep_var_name)) {
                                ctx.create_binding(deep_var_name, prop_value, true);
                            } else {
                                ctx.set_binding(deep_var_name, prop_value);
                            }
                        }
                    }
                    break;
                }
            }
        } else {
            size_t colon_pos = var_name.find(':');
            if (colon_pos != std::string::npos && colon_pos > 0 && colon_pos < var_name.length() - 1) {
                if (var_name.find(',') != std::string::npos) {

                    std::vector<std::string> mappings;
                    std::string current_mapping = "";
                    int nested_depth = 0;

                    for (size_t i = 0; i < var_name.length(); ++i) {
                        char c = var_name[i];

                        if (i + 9 <= var_name.length() &&
                            var_name.substr(i, 9) == "__nested:") {
                            nested_depth++;
                            current_mapping += "__nested:";
                            i += 8;
                        } else if (c == ',' && nested_depth == 0) {
                            if (!current_mapping.empty()) {
                                mappings.push_back(current_mapping);
                                current_mapping = "";
                            }
                        } else {
                            current_mapping += c;
                            if (nested_depth > 0 && i == var_name.length() - 1) {
                                nested_depth = 0;
                            }
                        }
                    }
                    if (!current_mapping.empty()) {
                        mappings.push_back(current_mapping);
                    }

                    for (const auto& mapping : mappings) {
                        size_t mapping_colon = mapping.find(':');
                        if (mapping_colon != std::string::npos) {
                            std::string property_name = mapping.substr(0, mapping_colon);
                            std::string variable_name = mapping.substr(mapping_colon + 1);


                            Value prop_value = nested_obj->get_property(property_name);

                            if (!ctx.has_binding(variable_name)) {
                                ctx.create_binding(variable_name, prop_value, true);
                            } else {
                                ctx.set_binding(variable_name, prop_value);
                            }
                        }
                    }
                } else {
                    std::string property_name = var_name.substr(0, colon_pos);
                    std::string variable_name = var_name.substr(colon_pos + 1);


                    Value prop_value = nested_obj->get_property(property_name);

                    if (!ctx.has_binding(variable_name)) {
                        ctx.create_binding(variable_name, prop_value, true);
                    } else {
                        ctx.set_binding(variable_name, prop_value);
                    }
                }
            } else {
                Value prop_value = nested_obj->get_property(var_name);

                if (!ctx.has_binding(var_name)) {
                    ctx.create_binding(var_name, prop_value, true);
                } else {
                    ctx.set_binding(var_name, prop_value);
                }
            }
        }
    }
}

void DestructuringAssignment::handle_nested_object_destructuring_with_source(Object* nested_obj, const std::vector<std::string>& var_names, Context& ctx, DestructuringAssignment* source_destructuring) {

    for (const std::string& var_name : var_names) {
        if (var_name.length() > 9 && var_name.substr(0, 9) == "__nested:") {
            std::string deeper_vars_string = var_name.substr(9);
            std::vector<std::string> deeper_var_names;
            std::string current_var = "";
            int nested_depth = 0;

            for (size_t i = 0; i < deeper_vars_string.length(); ++i) {
                char c = deeper_vars_string[i];
                if (i + 9 <= deeper_vars_string.length() &&
                    deeper_vars_string.substr(i, 9) == "__nested:") {
                    nested_depth++;
                    current_var += "__nested:";
                    i += 8;
                } else if (c == ',' && nested_depth == 0) {
                    if (!current_var.empty()) {
                        deeper_var_names.push_back(current_var);
                        current_var = "";
                    }
                } else {
                    current_var += c;
                    if (nested_depth > 0 && i == deeper_vars_string.length() - 1) {
                        nested_depth = 0;
                    }
                }
            }
            if (!current_var.empty()) {
                deeper_var_names.push_back(current_var);
            }

            for (const auto& property_name : nested_obj->get_own_property_keys()) {
                Value property_value = nested_obj->get_property(property_name);
                if (property_value.is_object()) {
                    Object* deeper_obj = property_value.as_object();
                    handle_nested_object_destructuring_with_source(deeper_obj, deeper_var_names, ctx, source_destructuring);
                    break;
                }
            }
        } else {
            size_t colon_pos = var_name.find(':');
            if (colon_pos != std::string::npos && colon_pos > 0 && colon_pos < var_name.length() - 1) {
                std::string property_name = var_name.substr(0, colon_pos);
                std::string variable_name = var_name.substr(colon_pos + 1);

                Value prop_value = nested_obj->get_property(property_name);

                if (!ctx.has_binding(variable_name)) {
                    ctx.create_binding(variable_name, prop_value, true);
                } else {
                    ctx.set_binding(variable_name, prop_value);
                }
            } else {
                std::string actual_property = var_name;
                std::string target_variable = var_name;

                bool found_mapping = false;

                Value prop_value = nested_obj->get_property(actual_property);

                if (!ctx.has_binding(target_variable)) {
                    ctx.create_binding(target_variable, prop_value, true);
                } else {
                    ctx.set_binding(target_variable, prop_value);
                }
            }
        }
    }
}

void DestructuringAssignment::handle_nested_object_destructuring_with_mappings(Object* nested_obj, const std::vector<std::string>& var_names, Context& ctx) {

    for (const std::string& var_name : var_names) {
        if (var_name.length() > 9 && var_name.substr(0, 9) == "__nested:") {
            std::string deeper_vars_string = var_name.substr(9);
            std::vector<std::string> deeper_var_names;
            std::string current_var = "";
            int nested_depth = 0;

            for (size_t i = 0; i < deeper_vars_string.length(); ++i) {
                char c = deeper_vars_string[i];
                if (i + 9 <= deeper_vars_string.length() &&
                    deeper_vars_string.substr(i, 9) == "__nested:") {
                    nested_depth++;
                    current_var += "__nested:";
                    i += 8;
                } else if (c == ',' && nested_depth == 0) {
                    if (!current_var.empty()) {
                        deeper_var_names.push_back(current_var);
                        current_var = "";
                    }
                } else {
                    current_var += c;
                    if (nested_depth > 0 && i == deeper_vars_string.length() - 1) {
                        nested_depth = 0;
                    }
                }
            }
            if (!current_var.empty()) {
                deeper_var_names.push_back(current_var);
            }

            for (const auto& property_name : nested_obj->get_own_property_keys()) {
                Value property_value = nested_obj->get_property(property_name);
                if (property_value.is_object()) {
                    Object* deeper_obj = property_value.as_object();
                    handle_nested_object_destructuring_with_mappings(deeper_obj, deeper_var_names, ctx);
                    break;
                }
            }
        } else {
            size_t colon_pos = var_name.find(':');
            if (colon_pos != std::string::npos && colon_pos > 0 && colon_pos < var_name.length() - 1) {
                std::string property_name = var_name.substr(0, colon_pos);
                std::string variable_name = var_name.substr(colon_pos + 1);

                Value prop_value = nested_obj->get_property(property_name);

                if (!ctx.has_binding(variable_name)) {
                    ctx.create_binding(variable_name, prop_value, true);
                } else {
                    ctx.set_binding(variable_name, prop_value);
                }
            } else {



                Value prop_value = nested_obj->get_property(var_name);

                if (!ctx.has_binding(var_name)) {
                    ctx.create_binding(var_name, prop_value, true);
                } else {
                    ctx.set_binding(var_name, prop_value);
                }
            }
        }
    }
}

void DestructuringAssignment::handle_nested_object_destructuring_smart(Object* nested_obj, const std::vector<std::string>& var_names, Context& ctx, DestructuringAssignment* source) {

    static std::map<std::string, std::map<std::string, std::string>> global_property_mappings;

    std::string source_key = "destructuring_" + std::to_string(reinterpret_cast<uintptr_t>(source));
    auto& source_mappings = global_property_mappings[source_key];

    for (const auto& mapping : source->get_property_mappings()) {
        if (mapping.property_name != mapping.variable_name) {
            source_mappings[mapping.property_name] = mapping.variable_name;
        }
    }

    for (const std::string& var_name : var_names) {
        if (var_name.length() > 9 && var_name.substr(0, 9) == "__nested:") {
            std::string deeper_vars_string = var_name.substr(9);
            std::vector<std::string> deeper_var_names;
            std::string current_var = "";
            int nested_depth = 0;

            for (size_t i = 0; i < deeper_vars_string.length(); ++i) {
                char c = deeper_vars_string[i];
                if (i + 9 <= deeper_vars_string.length() &&
                    deeper_vars_string.substr(i, 9) == "__nested:") {
                    nested_depth++;
                    current_var += "__nested:";
                    i += 8;
                } else if (c == ',' && nested_depth == 0) {
                    if (!current_var.empty()) {
                        deeper_var_names.push_back(current_var);
                        current_var = "";
                    }
                } else {
                    current_var += c;
                    if (nested_depth > 0 && i == deeper_vars_string.length() - 1) {
                        nested_depth = 0;
                    }
                }
            }
            if (!current_var.empty()) {
                deeper_var_names.push_back(current_var);
            }

            for (const auto& property_name : nested_obj->get_own_property_keys()) {
                Value property_value = nested_obj->get_property(property_name);
                if (property_value.is_object()) {
                    Object* deeper_obj = property_value.as_object();
                    handle_nested_object_destructuring_smart(deeper_obj, deeper_var_names, ctx, source);
                    break;
                }
            }
        } else {
            size_t colon_pos = var_name.find(':');
            if (colon_pos != std::string::npos && colon_pos > 0 && colon_pos < var_name.length() - 1) {
                std::string property_name = var_name.substr(0, colon_pos);
                std::string variable_name = var_name.substr(colon_pos + 1);

                Value prop_value = nested_obj->get_property(property_name);
                if (!ctx.has_binding(variable_name)) {
                    ctx.create_binding(variable_name, prop_value, true);
                } else {
                    ctx.set_binding(variable_name, prop_value);
                }
            } else {
                std::string target_variable = var_name;

                if (source_mappings.find(var_name) != source_mappings.end()) {
                    target_variable = source_mappings[var_name];
                }

                Value prop_value = nested_obj->get_property(var_name);
                if (!ctx.has_binding(target_variable)) {
                    ctx.create_binding(target_variable, prop_value, true);
                } else {
                    ctx.set_binding(target_variable, prop_value);
                }
            }
        }
    }

    global_property_mappings.erase(source_key);
}

void DestructuringAssignment::handle_nested_object_destructuring_enhanced(Object* nested_obj, const std::vector<std::string>& var_names, Context& ctx, const std::string& property_key) {


    static std::map<std::string, std::string> runtime_property_mappings;


    for (const std::string& var_name : var_names) {
        if (var_name.length() > 9 && var_name.substr(0, 9) == "__nested:") {
            std::string deeper_vars_string = var_name.substr(9);
            std::vector<std::string> deeper_var_names;
            std::string current_var = "";
            int nested_depth = 0;

            for (size_t i = 0; i < deeper_vars_string.length(); ++i) {
                char c = deeper_vars_string[i];
                if (i + 9 <= deeper_vars_string.length() &&
                    deeper_vars_string.substr(i, 9) == "__nested:") {
                    nested_depth++;
                    current_var += "__nested:";
                    i += 8;
                } else if (c == ',' && nested_depth == 0) {
                    if (!current_var.empty()) {
                        deeper_var_names.push_back(current_var);
                        current_var = "";
                    }
                } else {
                    current_var += c;
                    if (nested_depth > 0 && i == deeper_vars_string.length() - 1) {
                        nested_depth = 0;
                    }
                }
            }
            if (!current_var.empty()) {
                deeper_var_names.push_back(current_var);
            }


            for (const auto& prop_name : nested_obj->get_own_property_keys()) {
                Value property_value = nested_obj->get_property(prop_name);
                if (property_value.is_object()) {
                    Object* deeper_obj = property_value.as_object();
                    handle_nested_object_destructuring_enhanced(deeper_obj, deeper_var_names, ctx, prop_name);
                    break;
                }
            }
        } else {
            size_t colon_pos = var_name.find(':');
            if (colon_pos != std::string::npos && colon_pos > 0 && colon_pos < var_name.length() - 1) {
                std::string property_name = var_name.substr(0, colon_pos);
                std::string variable_name = var_name.substr(colon_pos + 1);

                Value prop_value = nested_obj->get_property(property_name);
                if (!ctx.has_binding(variable_name)) {
                    ctx.create_binding(variable_name, prop_value, true);
                } else {
                    ctx.set_binding(variable_name, prop_value);
                }
            } else {
                std::string target_variable = var_name;
                bool found_mapping = false;

                static std::map<std::string, std::vector<std::pair<std::string, std::string>>> global_nested_mappings;

                for (const std::string& check_var : var_names) {
                    if (check_var.find("REGISTRY:") == 0) {
                        size_t first_colon = check_var.find(':', 9);
                        if (first_colon != std::string::npos) {
                            size_t second_colon = check_var.find(':', first_colon + 1);
                            if (second_colon != std::string::npos) {
                                std::string registry_key = check_var.substr(9, first_colon - 9);

                                if (global_nested_mappings.find(registry_key) != global_nested_mappings.end()) {
                                    auto& mappings = global_nested_mappings[registry_key];
                                    for (const auto& mapping_pair : mappings) {
                                        if (mapping_pair.first == var_name) {
                                            target_variable = mapping_pair.second;
                                            found_mapping = true;
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                        break;
                    }
                }

                Value prop_value = nested_obj->get_property(var_name);
                if (!ctx.has_binding(target_variable)) {
                    ctx.create_binding(target_variable, prop_value, true);
                } else {
                    ctx.set_binding(target_variable, prop_value);
                }
            }
        }
    }
}

std::string DestructuringAssignment::to_string() const {
    std::string targets_str;
    if (type_ == Type::ARRAY) {
        targets_str = "[";
        for (size_t i = 0; i < targets_.size(); i++) {
            if (i > 0) targets_str += ", ";
            targets_str += targets_[i]->get_name();
        }
        targets_str += "]";
    } else {
        targets_str = "{";
        for (size_t i = 0; i < targets_.size(); i++) {
            if (i > 0) targets_str += ", ";
            targets_str += targets_[i]->get_name();
        }
        targets_str += "}";
    }
    return targets_str + " = " + source_->to_string();
}

std::unique_ptr<ASTNode> DestructuringAssignment::clone() const {
    std::vector<std::unique_ptr<Identifier>> cloned_targets;
    for (const auto& target : targets_) {
        cloned_targets.push_back(
            std::unique_ptr<Identifier>(static_cast<Identifier*>(target->clone().release()))
        );
    }

    auto cloned = std::make_unique<DestructuringAssignment>(
        std::move(cloned_targets), source_ ? source_->clone() : nullptr, type_, start_, end_
    );

    for (const auto& mapping : property_mappings_) {
        if (mapping.computed_key) {
            cloned->add_computed_property_mapping(mapping.property_name, mapping.variable_name, mapping.computed_key);
        } else {
            cloned->add_property_mapping(mapping.property_name, mapping.variable_name);
        }
    }

    for (const auto& default_val : default_values_) {
        cloned->add_default_value(default_val.index, default_val.expr->clone());
    }

    if (nested_rest_pattern_) {
        cloned->set_nested_rest_pattern(nested_rest_pattern_->clone());
    }

    return std::move(cloned);
}


void DestructuringAssignment::handle_infinite_depth_destructuring(Object* obj, const std::string& nested_pattern, Context& ctx) {


    std::string pattern = nested_pattern;
    Object* current_obj = obj;

    while (!pattern.empty()) {

        if (pattern.length() > 9 && pattern.substr(0, 9) == "__nested:") {
            pattern = pattern.substr(9);
            continue;
        }

        size_t colon_pos = pattern.find(':');

        if (colon_pos == std::string::npos) {
            Value final_value = current_obj->get_property(pattern);
            if (!ctx.has_binding(pattern)) {
                ctx.create_binding(pattern, final_value, true);
            } else {
                ctx.set_binding(pattern, final_value);
            }
            return;
        }

        std::string prop_name = pattern.substr(0, colon_pos);
        std::string remaining = pattern.substr(colon_pos + 1);


        bool is_renaming = (remaining.find(':') == std::string::npos &&
                           remaining.find("__nested:") == std::string::npos);

        if (is_renaming) {
            Value prop_value = current_obj->get_property(prop_name);
            if (!ctx.has_binding(remaining)) {
                ctx.create_binding(remaining, prop_value, true);
            } else {
                ctx.set_binding(remaining, prop_value);
            }
            return;
        }


        Value prop_value = current_obj->get_property(prop_name);
        if (!prop_value.is_object()) {
            return;
        }

        current_obj = prop_value.as_object();
        pattern = remaining;

    }
}

} // namespace Quanta
