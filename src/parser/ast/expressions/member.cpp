/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/parser/AST.h"
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
    if (val.is_symbol()) return val.as_symbol()->to_property_key();
    if (!val.is_object() && !val.is_function()) return val.to_string();

    Object* obj = val.is_function() ? static_cast<Object*>(val.as_function()) : val.as_object();

    Symbol* tp_sym = Symbol::get_well_known(Symbol::TO_PRIMITIVE);
    if (tp_sym) {
        Value tp = obj->get_property(tp_sym->to_property_key());
        if (ctx.has_exception()) return "";
        if (!tp.is_undefined()) {
            if (!tp.is_function()) {
                ctx.throw_type_error("Cannot convert object to primitive value");
                return "";
            }
            Value result = tp.as_function()->call(ctx, {Value(std::string("string"))}, val);
            if (ctx.has_exception()) return "";
            if (result.is_symbol()) return result.as_symbol()->to_property_key();
            if (result.is_object() || result.is_function()) {
                ctx.throw_type_error("Cannot convert object to primitive value");
                return "";
            }
            return result.to_string();
        }
    }

    Value ts = obj->get_property("toString");
    if (!ctx.has_exception() && ts.is_function()) {
        Value r = ts.as_function()->call(ctx, {}, val);
        if (ctx.has_exception()) return "";
        if (!r.is_object() && !r.is_function()) return r.to_string();
    }
    if (ctx.has_exception()) return "";

    Value vof = obj->get_property("valueOf");
    if (!ctx.has_exception() && vof.is_function()) {
        Value r = vof.as_function()->call(ctx, {}, val);
        if (ctx.has_exception()) return "";
        if (!r.is_object() && !r.is_function()) return r.to_string();
    }
    if (ctx.has_exception()) return "";

    ctx.throw_type_error("Cannot convert object to primitive value");
    return "";
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
    if (!cs.is_empty() && cs.top().function_ptr) {
        Function* fn = cs.top().function_ptr;

        Value brands_val = fn->get_property("__private_brands__");
        if (brands_val.is_object()) {
            Object* brands = brands_val.as_object();
            Value name_brand = brands->get_property(prop_name);
            if (name_brand.is_object() || name_brand.is_function()) {
                Object* expected = name_brand.is_function()
                    ? static_cast<Object*>(name_brand.as_function())
                    : name_brand.as_object();
                if (!do_brand_check(obj, expected)) return false;
                if (!require_exists) return true;
                bool found = obj->has_private_slot(prop_name);
                if (!found) {
                    Object* p = obj->get_prototype();
                    while (p && !found) { if (p->has_private_slot(prop_name)) found = true; p = p->get_prototype(); }
                }
                return found;
            }
        }

        Value brand_val = fn->get_property("__private_class_brand__");
        if (brand_val.is_object() || brand_val.is_function()) {
            Object* expected = brand_val.is_function()
                ? static_cast<Object*>(brand_val.as_function())
                : brand_val.as_object();
            if (!do_brand_check(obj, expected)) return false;
            if (!require_exists) return true;
            bool found = obj->has_private_slot(prop_name);
            if (!found) {
                Object* p = obj->get_prototype();
                while (p && !found) { if (p->has_private_slot(prop_name)) found = true; p = p->get_prototype(); }
            }
            return found;
        }
    }
    bool found = obj->has_private_slot(prop_name);
    if (!found) {
        Object* p = obj->get_prototype();
        while (p && !found) { if (p->has_private_slot(prop_name)) found = true; p = p->get_prototype(); }
    }
    return found;
}

std::vector<Value> process_arguments_with_spread(const std::vector<std::unique_ptr<ASTNode>>& arguments, Context& ctx);

Value MemberExpression::evaluate(Context& ctx) {
    // ES6: super.prop / super[expr] looks up on parent prototype, not the constructor itself
    if (object_->get_type() == ASTNode::Type::IDENTIFIER &&
        static_cast<Identifier*>(object_.get())->get_name() == "super") {
        // ES2024 13.3.7.1: GetThisBinding() must succeed before evaluating the property expr.
        // In a derived constructor before super(), this throws ReferenceError.
        if (ctx.this_needs_super()) {
            ctx.throw_reference_error("Must call super constructor before accessing 'this' in derived class constructor");
            return Value();
        }
        Object* lookup_proto = nullptr;
        Value super_ctor = ctx.get_binding("__super__");
        if (super_ctor.is_function()) {
            // Class method: super is the parent class, lookup on its prototype
            Value proto_val = super_ctor.as_function()->get_property("prototype");
            if (proto_val.is_object()) lookup_proto = proto_val.as_object();
        } else {
            // Object literal method: super = Object.getPrototypeOf([[HomeObject]])
            // Use __home_object__ if set, otherwise fall back to current 'this'
            Value home = ctx.get_binding("__home_object__");
            if (!home.is_undefined() && !home.is_null()) {
                Object* home_obj = home.is_function() ? static_cast<Object*>(home.as_function())
                                                      : home.as_object();
                if (home_obj) lookup_proto = home_obj->get_prototype();
            } else {
                Value this_val = ctx.get_binding("this");
                if (this_val.is_object_like()) {
                    Object* this_obj = this_val.is_function()
                        ? static_cast<Object*>(this_val.as_function()) : this_val.as_object();
                    if (this_obj) lookup_proto = this_obj->get_prototype();
                }
            }
        }
        if (lookup_proto) {
            std::string prop_name;
            if (computed_) {
                Value key_val = property_->evaluate(ctx);
                if (ctx.has_exception()) return Value();
                prop_name = to_js_property_key(ctx, key_val);
                if (ctx.has_exception()) return Value();
            } else if (property_->get_type() == ASTNode::Type::IDENTIFIER) {
                prop_name = static_cast<Identifier*>(property_.get())->get_name();
            }
            // For getter properties, invoke with current 'this' as receiver (spec super[[Get]])
            PropertyDescriptor desc = lookup_proto->get_property_descriptor(prop_name);
            if (desc.is_accessor_descriptor() && desc.has_getter()) {
                Function* getter = dynamic_cast<Function*>(desc.get_getter());
                if (getter) {
                    Value this_val = ctx.get_binding("this");
                    return getter->call(ctx, {}, this_val);
                }
            }
            return lookup_proto->get_property(prop_name);
        }
        // RequireObjectCoercible: null prototype base throws TypeError (spec 13.3.7.3 step 5)
        ctx.throw_type_error("Cannot read properties of null (reading super property)");
        return Value();
    }

    Value object_value = object_->evaluate(ctx);
    if (ctx.has_exception()) return Value();
    
    if (object_value.is_null() || object_value.is_undefined()) {
        if (object_->get_type() == ASTNode::Type::OPTIONAL_CHAINING_EXPRESSION) {
            return Value();
        }
        ctx.throw_type_error("Cannot read property of null or undefined");
        return Value();
    }

    // ES6: Property access on symbols - look up Symbol.prototype
    if (object_value.is_symbol() && !computed_) {
        if (property_->get_type() == ASTNode::Type::IDENTIFIER) {
            std::string prop_name = static_cast<Identifier*>(property_.get())->get_name();
            Value ctor = ctx.get_binding("Symbol");
            if (ctor.is_function()) {
                Value proto = ctor.as_function()->get_property("prototype");
                if (proto.is_object()) {
                    Object* proto_obj = proto.as_object();
                    PropertyDescriptor desc = proto_obj->get_property_descriptor(prop_name);
                    if (desc.is_accessor_descriptor() && desc.has_getter()) {
                        Function* getter = dynamic_cast<Function*>(desc.get_getter());
                        if (getter) return getter->call(ctx, {}, object_value);
                        return Value();
                    }
                    Value val = proto_obj->get_property(prop_name);
                    if (!val.is_undefined()) return val;
                }
            }
        }
        return Value();
    }

    // ES5: Property access on primitives - check prototype for accessors
    if ((object_value.is_string() || object_value.is_number() || object_value.is_boolean()) && !computed_) {
        if (property_->get_type() == ASTNode::Type::IDENTIFIER) {
            Identifier* prop = static_cast<Identifier*>(property_.get());
            std::string prop_name = prop->get_name();

            if (object_value.is_string() && prop_name == "length") {
                std::string str_value = object_value.to_string();
                return Value(static_cast<double>(utf16_length(str_value)));
            }

            std::string ctor_name = object_value.is_string() ? "String" :
                (object_value.is_number() ? "Number" : "Boolean");
            Value ctor = ctx.get_binding(ctor_name);
            if (ctor.is_object() || ctor.is_function()) {
                Object* ctor_obj = ctor.is_object() ? ctor.as_object() : ctor.as_function();
                Value prototype = ctor_obj->get_property("prototype");
                if (prototype.is_object()) {
                    Object* proto_obj = prototype.as_object();

                    // Check for accessor getter on prototype
                    PropertyDescriptor desc = proto_obj->get_property_descriptor(prop_name);
                    if (desc.is_accessor_descriptor() && desc.has_getter()) {
                        Function* getter = dynamic_cast<Function*>(desc.get_getter());
                        if (getter) {
                            return getter->call(ctx, {}, object_value);
                        }
                    }

                    Value method = proto_obj->get_property(prop_name);
                    if (!method.is_undefined()) {
                        return method;
                    }
                }
            }
        }
    }

    if ((object_value.is_object() || object_value.is_function()) && !computed_) {
        Object* obj = object_value.is_object() ? object_value.as_object() : object_value.as_function();
        if (property_->get_type() == ASTNode::Type::IDENTIFIER) {
            Identifier* prop = static_cast<Identifier*>(property_.get());
            std::string prop_name = prop->get_name();

            // fast path: Array length access
            if (__builtin_expect(prop_name == "length" && obj->is_array(), 1)) {
                return Value(static_cast<double>(obj->get_length()));
            }

            if (!prop_name.empty() && prop_name[0] == '#') {
                if (!private_brand_check(ctx, obj, prop_name)) {
                    ctx.throw_type_error("Cannot read private member " + prop_name + " from an object whose class did not declare it");
                    return Value();
                }
                Object* lookup = obj;
                while (lookup) {
                    PropertyDescriptor d = lookup->get_property_descriptor(prop_name);
                    if (d.is_accessor_descriptor()) {
                        if (!d.has_getter()) {
                            ctx.throw_type_error("'" + prop_name + "' accessor has no getter");
                            return Value();
                        }
                        break;
                    }
                    if (d.has_value()) break;
                    lookup = lookup->get_prototype();
                }
            }

            Shape* shape = obj->get_shape();


            if (__builtin_expect(!obj->is_function() && (prop_name.empty() || prop_name[0] != '#'), 1)) {
                for (uint8_t i = 0; i < ic_size_; i++) {
                    if (__builtin_expect(ic_cache_[i].shape_ptr == shape, 1)) {
                        return obj->get_property_by_offset_unchecked(ic_cache_[i].offset);
                    }
                }
            }

            PropertyDescriptor desc = obj->get_property_descriptor(prop_name);
            if (desc.is_accessor_descriptor() && desc.has_getter()) {
                Object* getter = desc.get_getter();
                if (getter) {
                    Function* getter_fn = dynamic_cast<Function*>(getter);
                    if (getter_fn) {
                        std::vector<Value> args;
                        return getter_fn->call(ctx, args, object_value);
                    }
                }
                return Value();
            }

            // Check prototype chain for accessor descriptors (e.g. class get/set)
            {
                Object* proto = obj->get_prototype();
                while (proto) {
                    PropertyDescriptor proto_desc = proto->get_property_descriptor(prop_name);
                    if (proto_desc.is_accessor_descriptor() && proto_desc.has_getter()) {
                        Function* getter_fn = dynamic_cast<Function*>(proto_desc.get_getter());
                        if (getter_fn) {
                            std::vector<Value> args;
                            return getter_fn->call(ctx, args, object_value);
                        }
                        return Value();
                    }
                    if (proto_desc.has_value()) break;  // Found as data property, stop
                    proto = proto->get_prototype();
                }
            }

            if (__builtin_expect(shape != nullptr && (prop_name.empty() || prop_name[0] != '#'), 1)) {
                auto info = shape->get_property_info(prop_name);
                if (__builtin_expect(info.offset != UINT32_MAX, 1)) {
                    if (ic_size_ < 4) {
                        ic_cache_[ic_size_].shape_ptr = shape;
                        ic_cache_[ic_size_].offset = info.offset;
                        ic_size_++;
                    } else {
                        ic_cache_[3].shape_ptr = shape;
                        ic_cache_[3].offset = info.offset;
                    }
                }
            }

            return obj->get_property(prop_name);
        }
    }
    
    if ((object_value.is_object() || object_value.is_function()) && computed_) {
        Object* obj = object_value.is_object() ? object_value.as_object() : object_value.as_function();

        //  ufp: Constant array index
        if (__builtin_expect(property_->get_type() == ASTNode::Type::NUMBER_LITERAL, 0)) {
            NumberLiteral* num_lit = static_cast<NumberLiteral*>(property_.get());
            double index_double = num_lit->get_value();
            if (__builtin_expect(index_double >= 0 && index_double == static_cast<uint32_t>(index_double), 1)) {
                uint32_t index = static_cast<uint32_t>(index_double);
                Value element = obj->get_element(index);
                if (!element.is_undefined()) {
                    return element;
                }
            }
        }

        Value prop_value = property_->evaluate(ctx);
        if (ctx.has_exception()) return Value();
        // fp: Variable array index
        if (__builtin_expect(prop_value.is_number(), 1)) {
            double index_double = prop_value.as_number();
            if (__builtin_expect(index_double >= 0 && index_double == static_cast<uint32_t>(index_double), 1)) {
                uint32_t index = static_cast<uint32_t>(index_double);
                Value element = obj->get_element(index);
                if (!element.is_undefined()) {
                    return element;
                }
            }
        }

        std::string prop_name = to_js_property_key(ctx, prop_value);
        if (ctx.has_exception()) return Value();

        PropertyDescriptor desc = obj->get_property_descriptor(prop_name);
        if (desc.is_accessor_descriptor() && desc.has_getter()) {
            Object* getter = desc.get_getter();
            if (getter) {
                Function* getter_fn = dynamic_cast<Function*>(getter);
                if (getter_fn) {
                    std::vector<Value> args;
                    return getter_fn->call(ctx, args, object_value);
                }
            }
            return Value();
        }

        return obj->get_property(prop_name);
    }
    
    if (object_->get_type() == ASTNode::Type::IDENTIFIER &&
        property_->get_type() == ASTNode::Type::IDENTIFIER && !computed_) {
        
        Identifier* obj = static_cast<Identifier*>(object_.get());
        Identifier* prop = static_cast<Identifier*>(property_.get());
        
        if (obj->get_name() == "Math") {
            std::string prop_name = prop->get_name();
            
            if (prop_name == "PI") {
                return Value(Math::PI);
            } else if (prop_name == "E") {
                return Value(Math::E);
            } else if (prop_name == "LN2") {
                return Value(Math::LN2);
            } else if (prop_name == "LN10") {
                return Value(Math::LN10);
            } else if (prop_name == "LOG2E") {
                return Value(Math::LOG2E);
            } else if (prop_name == "LOG10E") {
                return Value(Math::LOG10E);
            } else if (prop_name == "SQRT1_2") {
                return Value(Math::SQRT1_2);
            } else if (prop_name == "SQRT2") {
                return Value(Math::SQRT2);
            }
            
        }
    }

    if (object_value.is_undefined() || object_value.is_null()) {
        std::string type_name = object_value.is_undefined() ? "undefined" : "null";
        ctx.throw_type_error("Cannot read property of " + type_name);
        return Value();
    }
    
    std::string prop_name;
    if (computed_) {
        Value prop_value = property_->evaluate(ctx);
        if (ctx.has_exception()) return Value();
        prop_name = to_js_property_key(ctx, prop_value);
        if (ctx.has_exception()) return Value();
    } else {
        if (property_->get_type() == ASTNode::Type::IDENTIFIER) {
            Identifier* prop = static_cast<Identifier*>(property_.get());
            prop_name = prop->get_name();
        }
    }
    

    if (object_value.is_string()) {
        std::string str_value = object_value.to_string();
        
        if (str_value.length() >= 6 && str_value.substr(0, 6) == "ARRAY:" && computed_) {
            Value prop_value = property_->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            if (prop_value.is_number()) {
                uint32_t index = static_cast<uint32_t>(prop_value.as_number());
                
                size_t start = str_value.find('[');
                size_t end = str_value.find(']');
                if (start != std::string::npos && end != std::string::npos) {
                    std::string content = str_value.substr(start + 1, end - start - 1);
                    if (content.empty()) return Value();
                    
                    std::vector<std::string> elements;
                    size_t pos = 0;
                    while (pos < content.length()) {
                        size_t comma = content.find(',', pos);
                        if (comma == std::string::npos) comma = content.length();
                        elements.push_back(content.substr(pos, comma - pos));
                        pos = comma + 1;
                    }
                    
                    if (index < elements.size()) {
                        std::string element = elements[index];
                        if (element == "true") {
                            return Value(true);
                        } else if (element == "false") {
                            return Value(false);
                        } else if (element == "null") {
                            return Value();
                        } else {
                            try {
                                double num = std::stod(element);
                                return Value(num);
                            } catch (...) {
                                return Value(element);
                            }
                        }
                    }
                }
            }
            return Value();
        }
        
        if (str_value.length() >= 6 && str_value.substr(0, 6) == "ARRAY:" && !computed_ && property_->get_type() == ASTNode::Type::IDENTIFIER) {
            Identifier* prop = static_cast<Identifier*>(property_.get());
            std::string prop_name = prop->get_name();
            
            if (prop_name == "length") {
                size_t start = str_value.find('[');
                size_t end = str_value.find(']');
                if (start != std::string::npos && end != std::string::npos) {
                    std::string content = str_value.substr(start + 1, end - start - 1);
                    if (content.empty()) return Value(0.0);
                    
                    uint32_t count = 1;
                    for (char c : content) {
                        if (c == ',') count++;
                    }
                    return Value(static_cast<double>(count));
                }
                return Value(0.0);
            }
            
            return Value();
        }
        
        if (str_value.length() >= 7 && str_value.substr(0, 7) == "OBJECT:" && computed_) {
            Value prop_value = property_->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            if (prop_value.is_string()) {
                std::string prop_name = prop_value.to_string();
                
                std::string search = prop_name + "=";
                size_t start = str_value.find(search);
                if (start != std::string::npos) {
                    start += search.length();
                    size_t end = str_value.find(",", start);
                    if (end == std::string::npos) {
                        end = str_value.find("}", start);
                    }
                    
                    if (end != std::string::npos) {
                        std::string value = str_value.substr(start, end - start);
                        if (value == "true") {
                            return Value(true);
                        } else if (value == "false") {
                            return Value(false);
                        } else if (value == "null") {
                            return Value();
                        } else if (value.substr(0, 9) == "FUNCTION:") {
                            std::string func_id = value.substr(9);
                                Value func_value = ctx.get_binding(func_id);
                                if (!func_value.is_undefined()) {
                                    return func_value;
                            } else {
                                    return Value();
                            }
                        } else {
                            try {
                                double num = std::stod(value);
                                return Value(num);
                            } catch (...) {
                                return Value(value);
                            }
                        }
                    }
                }
            }
            return Value();
        }
        
        if (str_value.length() >= 7 && str_value.substr(0, 7) == "OBJECT:" && !computed_ && property_->get_type() == ASTNode::Type::IDENTIFIER) {
            Identifier* prop = static_cast<Identifier*>(property_.get());
            std::string prop_name = prop->get_name();
            
            std::string search = prop_name + "=";
            size_t start = str_value.find(search);
            if (start != std::string::npos) {
                start += search.length();
                size_t end = start;
                
                if (start < str_value.length() && str_value.substr(start, 7) == "OBJECT:") {
                    int brace_count = 0;
                    bool in_object = false;
                    
                    for (size_t i = start; i < str_value.length(); i++) {
                        if (str_value[i] == '{') {
                            brace_count++;
                            in_object = true;
                        } else if (str_value[i] == '}') {
                            brace_count--;
                            if (in_object && brace_count == 0) {
                                end = i + 1;
                                break;
                            }
                        }
                    }
                } else {
                    end = str_value.find(",", start);
                    if (end == std::string::npos) {
                        end = str_value.find("}", start);
                    }
                }
                
                if (end > start) {
                    std::string value = str_value.substr(start, end - start);
                    if (value == "true") {
                        return Value(true);
                    } else if (value == "false") {
                        return Value(false);
                    } else if (value == "null") {
                        return Value();
                    } else if (value.substr(0, 9) == "FUNCTION:") {
                        std::string func_id = value.substr(9);
                        Value func_value = ctx.get_binding(func_id);
                        
                        if (func_value.is_undefined()) {
                            auto it = g_object_function_map.find(func_id);
                            if (it != g_object_function_map.end()) {
                                func_value = it->second;
                            }
                        }
                        
                        if (!func_value.is_undefined()) {
                            return func_value;
                        } else {
                            return Value();
                        }
                    } else {
                        try {
                            double num = std::stod(value);
                            return Value(num);
                        } catch (...) {
                            return Value(value);
                        }
                    }
                }
            }
            return Value();
        }
        
        std::string prop_name;
        if (!computed_ && property_->get_type() == ASTNode::Type::IDENTIFIER) {
            Identifier* prop = static_cast<Identifier*>(property_.get());
            prop_name = prop->get_name();
        }
        
        if (!computed_ && prop_name == "length") {
            return Value(static_cast<double>(utf16_length(str_value)));
        }

        if (!computed_ && prop_name == "charAt") {
            auto char_at_fn = ObjectFactory::create_native_function("charAt",
                [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    if (args.empty()) return Value(std::string(""));
                    int index = static_cast<int>(args[0].to_number());
                    if (index >= 0 && index < static_cast<int>(str_value.length())) {
                        return Value(std::string(1, str_value[index]));
                    }
                    return Value(std::string(""));
                });
            return Value(char_at_fn.release());
        }
        
        if (!computed_ && prop_name == "indexOf") {
            auto index_of_fn = ObjectFactory::create_native_function("indexOf",
                [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    if (args.empty()) return Value(-1.0);
                    std::string search = args[0].to_string();
                    size_t pos = str_value.find(search);
                    return Value(pos != std::string::npos ? static_cast<double>(pos) : -1.0);
                });
            return Value(index_of_fn.release());
        }
        
        if (prop_name == "toUpperCase") {
            auto upper_fn = ObjectFactory::create_native_function("toUpperCase",
                [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    std::string result = str_value;
                    std::transform(result.begin(), result.end(), result.begin(), ::toupper);
                    return Value(result);
                });
            return Value(upper_fn.release());
        }
        
        if (prop_name == "toLowerCase") {
            auto lower_fn = ObjectFactory::create_native_function("toLowerCase",
                [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    std::string result = str_value;
                    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
                    return Value(result);
                });
            return Value(lower_fn.release());
        }
        
        if (prop_name == "substring") {
            auto substring_fn = ObjectFactory::create_native_function("substring",
                [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    if (args.empty()) return Value(str_value);
                    int start = static_cast<int>(args[0].to_number());
                    int end = args.size() > 1 ? static_cast<int>(args[1].to_number()) : str_value.length();
                    start = std::max(0, std::min(start, static_cast<int>(str_value.length())));
                    end = std::max(0, std::min(end, static_cast<int>(str_value.length())));
                    if (start > end) std::swap(start, end);
                    return Value(str_value.substr(start, end - start));
                });
            return Value(substring_fn.release());
        }
        
        if (prop_name == "substr") {
            auto substr_fn = ObjectFactory::create_native_function("substr",
                [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    if (args.empty()) return Value(str_value);

                    int size = static_cast<int>(str_value.length());

                    // Convert start to integer (ToIntegerOrInfinity)
                    double start_num = args[0].to_number();
                    int start;
                    if (std::isnan(start_num)) {
                        start = 0;
                    } else if (std::isinf(start_num)) {
                        start = (start_num < 0) ? 0 : size;
                    } else {
                        start = static_cast<int>(std::trunc(start_num));
                    }

                    // Handle negative start
                    if (start < 0) {
                        start = std::max(0, size + start);
                    }
                    start = std::min(start, size);

                    // Convert length to integer (ToIntegerOrInfinity)
                    int length;
                    if (args.size() > 1) {
                        double length_num = args[1].to_number();
                        if (std::isnan(length_num)) {
                            length = 0;
                        } else if (std::isinf(length_num)) {
                            length = (length_num < 0) ? 0 : size;
                        } else {
                            length = static_cast<int>(std::trunc(length_num));
                        }
                    } else {
                        length = size;
                    }

                    // Clamp length to [0, size]
                    length = std::min(std::max(length, 0), size);

                    // Calculate end position
                    int end = std::min(start + length, size);

                    // Return substring
                    if (end <= start) {
                        return Value(std::string(""));
                    }

                    return Value(str_value.substr(start, end - start));
                });
            return Value(substr_fn.release());
        }
        
        if (prop_name == "slice") {
            auto slice_fn = ObjectFactory::create_native_function("slice",
                [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    if (args.empty()) return Value(str_value);
                    int start = static_cast<int>(args[0].to_number());
                    int end = args.size() > 1 ? static_cast<int>(args[1].to_number()) : str_value.length();
                    if (start < 0) start = std::max(0, static_cast<int>(str_value.length()) + start);
                    if (end < 0) end = std::max(0, static_cast<int>(str_value.length()) + end);
                    start = std::min(start, static_cast<int>(str_value.length()));
                    end = std::min(end, static_cast<int>(str_value.length()));
                    if (start >= end) return Value(std::string(""));
                    return Value(str_value.substr(start, end - start));
                });
            return Value(slice_fn.release());
        }
        
        if (!computed_ && prop_name == "split") {
            auto split_fn = ObjectFactory::create_native_function("split",
                [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    std::string separator = args.empty() ? "" : args[0].to_string();
                    
                    auto array = ObjectFactory::create_array();
                    
                    if (separator.empty()) {
                        for (size_t i = 0; i < str_value.length(); ++i) {
                            array->set_element(static_cast<uint32_t>(i), Value(std::string(1, str_value[i])));
                        }
                        array->set_length(static_cast<uint32_t>(str_value.length()));
                    } else {
                        std::vector<std::string> parts;
                        size_t start = 0;
                        size_t pos = 0;
                        
                        while ((pos = str_value.find(separator, start)) != std::string::npos) {
                            parts.push_back(str_value.substr(start, pos - start));
                            start = pos + separator.length();
                        }
                        parts.push_back(str_value.substr(start));
                        
                        for (size_t i = 0; i < parts.size(); ++i) {
                            array->set_element(static_cast<uint32_t>(i), Value(parts[i]));
                        }
                        array->set_length(static_cast<uint32_t>(parts.size()));
                    }
                    
                    return Value(array.release());
                });
            return Value(split_fn.release());
        }
        
        if (prop_name == "replace") {
            auto replace_fn = ObjectFactory::create_native_function("replace",
                [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    if (args.size() < 2) return Value(str_value);
                    
                    std::string search = args[0].to_string();
                    std::string replacement = args[1].to_string();
                    
                    std::string result = str_value;
                    size_t pos = result.find(search);
                    if (pos != std::string::npos) {
                        result.replace(pos, search.length(), replacement);
                    }
                    
                    return Value(result);
                });
            return Value(replace_fn.release());
        }
        
        if (!computed_ && prop_name == "startsWith") {
            auto starts_with_fn = ObjectFactory::create_native_function("startsWith",
                [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    if (args.empty()) return Value(false);

                    if (args[0].is_symbol()) {
                        ctx.throw_exception(Value(std::string("TypeError: Cannot convert a Symbol value to a string")));
                        return Value();
                    }

                    std::string search = args[0].to_string();
                    int start = 0;
                    if (args.size() > 1) {
                        if (args[1].is_symbol()) {
                            ctx.throw_exception(Value(std::string("TypeError: Cannot convert a Symbol value to a number")));
                            return Value();
                        }
                        start = static_cast<int>(args[1].to_number());
                    }
                    if (start < 0) start = 0;
                    size_t position = static_cast<size_t>(start);

                    if (position >= str_value.length()) {
                        return Value(search.empty());
                    }

                    if (position + search.length() > str_value.length()) {
                        return Value(false);
                    }

                    return Value(str_value.substr(position, search.length()) == search);
                });
            return Value(starts_with_fn.release());
        }
        
        if (!computed_ && prop_name == "endsWith") {
            auto ends_with_fn = ObjectFactory::create_native_function("endsWith",
                [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    if (args.empty()) return Value(false);

                    if (args[0].is_symbol()) {
                        ctx.throw_exception(Value(std::string("TypeError: Cannot convert a Symbol value to a string")));
                        return Value();
                    }

                    std::string search = args[0].to_string();
                    size_t length = str_value.length();
                    if (args.size() > 1) {
                        if (args[1].is_symbol()) {
                            ctx.throw_exception(Value(std::string("TypeError: Cannot convert a Symbol value to a number")));
                            return Value();
                        }
                        if (!std::isnan(args[1].to_number())) {
                            length = static_cast<size_t>(std::max(0.0, args[1].to_number()));
                        }
                    }

                    if (length > str_value.length()) length = str_value.length();
                    if (search.length() > length) return Value(false);

                    size_t start = length - search.length();
                    return Value(str_value.substr(start, search.length()) == search);
                });
            return Value(ends_with_fn.release());
        }
        
        if (prop_name == "includes") {
            auto includes_fn = ObjectFactory::create_native_function("includes",
                [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    if (args.empty()) return Value(false);

                    if (args[0].is_symbol()) {
                        ctx.throw_exception(Value(std::string("TypeError: Cannot convert a Symbol value to a string")));
                        return Value();
                    }

                    std::string search = args[0].to_string();

                    int start = 0;
                    if (args.size() > 1) {
                        if (args[1].is_symbol()) {
                            ctx.throw_exception(Value(std::string("TypeError: Cannot convert a Symbol value to a number")));
                            return Value();
                        }
                        start = static_cast<int>(args[1].to_number());
                    }
                    if (start < 0) start = 0;
                    size_t position = static_cast<size_t>(start);

                    if (position >= str_value.length()) {
                        return Value(search.empty());
                    }

                    size_t found = str_value.find(search, position);
                    return Value(found != std::string::npos);
                });
            return Value(includes_fn.release());
        }
        
        if (prop_name == "repeat") {
            auto repeat_fn = ObjectFactory::create_native_function("repeat",
                [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    if (args.empty()) {
                        return Value(std::string(""));
                    }
                    int count = static_cast<int>(args[0].to_number());
                    if (count < 0) {
                        ctx.throw_range_error("Invalid count value");
                        return Value();
                    }
                    if (count == 0) {
                        return Value(std::string(""));
                    }

                    std::string result;
                    result.reserve(str_value.length() * count);
                    for (int i = 0; i < count; ++i) {
                        result += str_value;
                    }
                    return Value(result);
                });
            return Value(repeat_fn.release());
        }
        
        if (prop_name == "trim") {
            auto trim_fn = ObjectFactory::create_native_function("trim",
                [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    (void)args;
                    std::string result = str_value;
                    result.erase(0, result.find_first_not_of(" \t\n\r"));
                    result.erase(result.find_last_not_of(" \t\n\r") + 1);
                    return Value(result);
                });
            return Value(trim_fn.release());
        }

        if (prop_name == "concat") {
            auto concat_fn = ObjectFactory::create_native_function("concat",
                [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    std::string result = str_value;
                    for (const auto& arg : args) {
                        result += arg.to_string();
                    }
                    return Value(result);
                });
            return Value(concat_fn.release());
        }

        if (prop_name == "padStart") {
            auto pad_start_fn = ObjectFactory::create_native_function("padStart",
                [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    if (args.empty()) return Value(str_value);
                    
                    int target_length = static_cast<int>(args[0].to_number());
                    if (target_length <= static_cast<int>(str_value.length())) {
                        return Value(str_value);
                    }
                    
                    std::string pad_string = " ";
                    if (args.size() > 1 && !args[1].is_undefined()) {
                        pad_string = args[1].to_string();
                    }
                    if (pad_string.empty()) pad_string = " ";
                    
                    int pad_length = target_length - static_cast<int>(str_value.length());
                    std::string result;
                    
                    while (static_cast<int>(result.length()) < pad_length) {
                        if (static_cast<int>(result.length()) + static_cast<int>(pad_string.length()) <= pad_length) {
                            result += pad_string;
                        } else {
                            result += pad_string.substr(0, pad_length - static_cast<int>(result.length()));
                        }
                    }
                    
                    return Value(result + str_value);
                });
            return Value(pad_start_fn.release());
        }
        
        if (prop_name == "padEnd") {
            auto pad_end_fn = ObjectFactory::create_native_function("padEnd",
                [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    if (args.empty()) return Value(str_value);
                    
                    int target_length = static_cast<int>(args[0].to_number());
                    if (target_length <= static_cast<int>(str_value.length())) {
                        return Value(str_value);
                    }
                    
                    std::string pad_string = " ";
                    if (args.size() > 1 && !args[1].is_undefined()) {
                        pad_string = args[1].to_string();
                    }
                    if (pad_string.empty()) pad_string = " ";
                    
                    int pad_length = target_length - static_cast<int>(str_value.length());
                    std::string result;
                    
                    while (static_cast<int>(result.length()) < pad_length) {
                        if (static_cast<int>(result.length()) + static_cast<int>(pad_string.length()) <= pad_length) {
                            result += pad_string;
                        } else {
                            result += pad_string.substr(0, pad_length - static_cast<int>(result.length()));
                        }
                    }
                    
                    return Value(str_value + result);
                });
            return Value(pad_end_fn.release());
        }
        
        if (computed_) {
            Value prop_value = property_->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            if (prop_value.is_symbol()) {
                Symbol* prop_symbol = prop_value.as_symbol();
                Symbol* iterator_symbol = Symbol::get_well_known(Symbol::ITERATOR);
                
                if (iterator_symbol && prop_symbol->equals(iterator_symbol)) {
                    auto string_iterator_fn = ObjectFactory::create_native_function("@@iterator",
                        [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                            (void)ctx;
                            (void)args;
                            auto iterator = std::make_unique<StringIterator>(str_value);
                            return Value(iterator.release());
                        });
                    return Value(string_iterator_fn.release());
                }
            }
        }
        
        if (computed_) {
            Value prop_value = property_->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            if (prop_value.is_number()) {
                int index = static_cast<int>(prop_value.to_number());
                if (index >= 0 && index < static_cast<int>(str_value.length())) {
                    return Value(std::string(1, str_value[index]));
                }
            }
        }
        
        return Value();
    }
    
    else if (object_value.is_number()) {
        Value number_ctor = ctx.get_binding("Number");
        if (number_ctor.is_function()) {
            Function* number_fn = number_ctor.as_function();
            Value prototype = number_fn->get_property("prototype");
            if (prototype.is_object()) {
                Object* number_prototype = prototype.as_object();
                Value method = number_prototype->get_property(prop_name);
                if (!method.is_undefined()) {
                    return method;
                }
            }
        }

        return Value();
    }
    
    else if (object_value.is_boolean()) {
        bool bool_value = object_value.as_boolean();
        
        if (prop_name == "toString") {
            auto to_string_fn = ObjectFactory::create_native_function("toString",
                [bool_value](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    return Value(bool_value ? "true" : "false");
                });
            return Value(to_string_fn.release());
        }
        
        if (prop_name == "valueOf") {
            auto value_of_fn = ObjectFactory::create_native_function("valueOf",
                [bool_value](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    return Value(bool_value);
                });
            return Value(value_of_fn.release());
        }
        
        return Value();
    }
    
    else if (object_value.is_string()) {
        std::string str_val = object_value.to_string();
        
        if (str_val.length() >= 6 && str_val.substr(0, 6) == "ARRAY:") {
            if (computed_) {
                Value prop_value = property_->evaluate(ctx);
                if (ctx.has_exception()) return Value();
                
                if (prop_value.is_number()) {
                    uint32_t index = static_cast<uint32_t>(prop_value.as_number());
                    
                    size_t start = str_val.find('[');
                    size_t end = str_val.find(']');
                    if (start != std::string::npos && end != std::string::npos) {
                        std::string content = str_val.substr(start + 1, end - start - 1);
                        if (content.empty()) return Value();
                        
                        std::vector<std::string> elements;
                        size_t pos = 0;
                        while (pos < content.length()) {
                            size_t comma = content.find(',', pos);
                            if (comma == std::string::npos) comma = content.length();
                            elements.push_back(content.substr(pos, comma - pos));
                            pos = comma + 1;
                        }
                        
                        if (index < elements.size()) {
                            return Value(elements[index]);
                        }
                    }
                }
            }
            return Value();
        }
        
        if (str_val.substr(0, 7) == "OBJECT:") {
            std::string prop_name;
            if (property_->get_type() == ASTNode::Type::IDENTIFIER) {
                Identifier* prop = static_cast<Identifier*>(property_.get());
                prop_name = prop->get_name();
            }
            
            std::string search = prop_name + "=";
            size_t start = str_val.find(search);
            if (start != std::string::npos) {
                start += search.length();
                size_t end = str_val.find(",", start);
                if (end == std::string::npos) {
                    end = str_val.find("}", start);
                }
                
                if (end != std::string::npos) {
                    std::string value = str_val.substr(start, end - start);
                    return Value(value);
                }
            }
            
            return Value();
        }
        
        std::string str_value = object_value.to_string();
        
        if (!computed_ && property_->get_type() == ASTNode::Type::IDENTIFIER) {
            Identifier* prop = static_cast<Identifier*>(property_.get());
            std::string prop_name = prop->get_name();
            
            if (prop_name == "length") {
                return Value(static_cast<double>(utf16_length(str_value)));
            }

            if (prop_name == "charAt") {
                auto char_at_fn = ObjectFactory::create_native_function("charAt",
                    [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                        (void)ctx;
                        if (args.empty()) return Value(std::string(""));
                        int index = static_cast<int>(args[0].to_number());
                        if (index >= 0 && index < static_cast<int>(str_value.length())) {
                            return Value(std::string(1, str_value[index]));
                        }
                        return Value(std::string(""));
                    });
                return Value(char_at_fn.release());
            }
            
            if (prop_name == "indexOf") {
                auto index_of_fn = ObjectFactory::create_native_function("indexOf",
                    [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                        (void)ctx;
                        if (args.empty()) return Value(-1.0);
                        std::string search = args[0].to_string();
                        size_t pos = str_value.find(search);
                        return Value(pos == std::string::npos ? -1.0 : static_cast<double>(pos));
                    });
                return Value(index_of_fn.release());
            }
            
            if (!computed_ && prop_name == "split") {
                auto split_fn = ObjectFactory::create_native_function("split",
                    [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                        (void)ctx;
                        std::string separator = args.empty() ? "" : args[0].to_string();
                        
                        auto array = ObjectFactory::create_array();
                        
                        if (separator.empty()) {
                            for (size_t i = 0; i < str_value.length(); ++i) {
                                array->set_element(static_cast<uint32_t>(i), Value(std::string(1, str_value[i])));
                            }
                            array->set_length(static_cast<uint32_t>(str_value.length()));
                        } else {
                            std::vector<std::string> parts;
                            size_t start = 0;
                            size_t pos = 0;
                            while ((pos = str_value.find(separator, start)) != std::string::npos) {
                                parts.push_back(str_value.substr(start, pos - start));
                                start = pos + separator.length();
                            }
                            parts.push_back(str_value.substr(start));
                            
                            for (size_t i = 0; i < parts.size(); ++i) {
                                array->set_element(static_cast<uint32_t>(i), Value(parts[i]));
                            }
                            array->set_length(static_cast<uint32_t>(parts.size()));
                        }
                        
                        return Value(array.release());
                    });
                return Value(split_fn.release());
            }
            
            if (!computed_ && prop_name == "startsWith") {
                auto starts_with_fn = ObjectFactory::create_native_function("startsWith",
                    [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                        (void)ctx; (void)args;
                        return Value(true);
                    });
                return Value(starts_with_fn.release());
            }
            
            if (!computed_ && prop_name == "endsWith") {
                auto ends_with_fn = ObjectFactory::create_native_function("endsWith",
                    [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                        (void)ctx;
                        if (args.empty()) return Value(false);
                        std::string search = args[0].to_string();
                        size_t length = args.size() > 1 && !std::isnan(args[1].to_number()) ?
                            static_cast<size_t>(std::max(0.0, args[1].to_number())) : str_value.length();

                        if (length > str_value.length()) length = str_value.length();
                        if (search.length() > length) return Value(false);

                        size_t start = length - search.length();
                        return Value(str_value.substr(start, search.length()) == search);
                    });
                return Value(ends_with_fn.release());
            }
        }
        
        if (computed_) {
            Value prop_value = property_->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            if (prop_value.is_number()) {
                int index = static_cast<int>(prop_value.to_number());
                if (index >= 0 && index < static_cast<int>(str_value.length())) {
                    return Value(std::string(1, str_value[index]));
                }
            }
        }
        
        return Value();
    }
    
    else if (object_value.is_object() || object_value.is_function()) {
        Object* obj = object_value.is_object() ? object_value.as_object() : object_value.as_function();
        if (computed_) {
            Value prop_value = property_->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            if (obj->is_array() && prop_value.is_number()) {
                uint32_t index = static_cast<uint32_t>(prop_value.as_number());
                return obj->get_element(index);
            }
            
            return obj->get_property(prop_value.to_string());
        } else {
            if (property_->get_type() == ASTNode::Type::IDENTIFIER) {
                Identifier* prop = static_cast<Identifier*>(property_.get());
                std::string prop_name = prop->get_name();
                
                if (prop_name == "cookie") {
                    // Cookie handling removed, return empty string
                    return Value(std::string(""));
                }
                
                Value result = obj->get_property(prop_name);
                if (ctx.has_exception()) return Value();
                return result;
            }
        }
    }
    
    return Value();
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


Value NewExpression::evaluate(Context& ctx) {
    Value constructor_value = constructor_->evaluate(ctx);
    if (ctx.has_exception()) return Value();

    if (constructor_value.is_object() &&
        constructor_value.as_object()->get_type() == Object::ObjectType::Proxy) {
        Proxy* proxy = static_cast<Proxy*>(constructor_value.as_object());
        std::vector<Value> arg_values = process_arguments_with_spread(arguments_, ctx);
        if (ctx.has_exception()) return Value();
        return proxy->construct_trap(arg_values);
    }

    if (!constructor_value.is_function()) {
        ctx.throw_type_error(constructor_value.to_string() + " is not a constructor");
        return Value();
    }

    std::vector<Value> arg_values = process_arguments_with_spread(arguments_, ctx);
    if (ctx.has_exception()) return Value();

    Function* constructor_fn = constructor_value.as_function();
    return constructor_fn->construct(ctx, arg_values);
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

Value MetaProperty::evaluate(Context& ctx) {
    if (meta_ == "new" && property_ == "target") {
        return ctx.get_new_target();
    }

    ctx.throw_exception(Value("ReferenceError: Unknown meta property: " + meta_ + "." + property_));
    return Value();
}

std::string MetaProperty::to_string() const {
    return meta_ + "." + property_;
}

std::unique_ptr<ASTNode> MetaProperty::clone() const {
    return std::make_unique<MetaProperty>(meta_, property_, start_, end_);
}






} // namespace Quanta
