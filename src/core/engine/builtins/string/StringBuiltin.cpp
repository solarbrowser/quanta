/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "StringBuiltin.h"
#include "quanta/core/engine/Context.h"
#include "quanta/parser/Parser.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/core/runtime/Iterator.h"
#include "quanta/core/runtime/RegExp.h"
#include <cmath>
#include <sstream>
#include <algorithm>
#include "utf8proc.h"

namespace Quanta {

void register_string_builtins(Context& ctx) {
    auto string_constructor = ObjectFactory::create_native_constructor("String",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string str_value = args.empty() ? "" : args[0].to_string();

            Object* this_obj = ctx.get_this_binding();
            if (this_obj) {
                this_obj->set_property("value", Value(str_value));
                this_obj->set_property("[[PrimitiveValue]]", Value(str_value));
                size_t str_utf16_len = 0;
                for (size_t i = 0; i < str_value.size(); ) {
                    unsigned char c = (unsigned char)str_value[i];
                    if (c >= 0xF0) { str_utf16_len += 2; i += 4; }
                    else if (c >= 0xE0) { str_utf16_len += 1; i += 3; }
                    else if (c >= 0xC0) { str_utf16_len += 1; i += 2; }
                    else { str_utf16_len += 1; i += 1; }
                }
                PropertyDescriptor length_desc(Value(static_cast<double>(str_utf16_len)),
                    static_cast<PropertyAttributes>(PropertyAttributes::None));
                this_obj->set_property_descriptor("length", length_desc);

                // Set indexed character properties: c[0] === "g"
                for (size_t i = 0; i < str_value.size(); i++) {
                    this_obj->set_property(std::to_string(i), Value(std::string(1, str_value[i])));
                }

                auto toString_fn = ObjectFactory::create_native_function("toString",
                    [](Context& ctx, const std::vector<Value>& args) -> Value {
                        (void)args;
                        Object* this_binding = ctx.get_this_binding();
                        if (this_binding && this_binding->has_property("value")) {
                            return this_binding->get_property("value");
                        }
                        return Value(std::string(""));
                    });
                this_obj->set_property("toString", Value(toString_fn.release()), PropertyAttributes::BuiltinFunction);
            }

            return Value(str_value);
        });
    
    auto string_prototype = ObjectFactory::create_object();
    
    auto padStart_fn = ObjectFactory::create_native_function("padStart",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();
            
            if (args.empty()) return Value(str);
            
            uint32_t target_length = static_cast<uint32_t>(args[0].to_number());
            std::string pad_string = args.size() > 1 ? args[1].to_string() : " ";
            
            if (target_length <= str.length()) {
                return Value(str);
            }
            
            uint32_t pad_length = target_length - str.length();
            std::string padding = "";
            
            if (!pad_string.empty()) {
                while (padding.length() < pad_length) {
                    padding += pad_string;
                }
                padding = padding.substr(0, pad_length);
            }
            
            return Value(padding + str);
        });
    PropertyDescriptor padStart_desc(Value(padStart_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("padStart", padStart_desc);
    
    auto padEnd_fn = ObjectFactory::create_native_function("padEnd",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();
            
            if (args.empty()) return Value(str);
            
            uint32_t target_length = static_cast<uint32_t>(args[0].to_number());
            std::string pad_string = args.size() > 1 ? args[1].to_string() : " ";
            
            if (target_length <= str.length()) {
                return Value(str);
            }
            
            uint32_t pad_length = target_length - str.length();
            std::string padding = "";
            
            if (!pad_string.empty()) {
                while (padding.length() < pad_length) {
                    padding += pad_string;
                }
                padding = padding.substr(0, pad_length);
            }
            
            return Value(str + padding);
        });
    PropertyDescriptor padEnd_desc(Value(padEnd_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("padEnd", padEnd_desc);

    // Helper: convert value to string, calling toString() on objects
    auto obj_to_string = [](Context& ctx, const Value& val) -> std::string {
        if (val.is_object() || val.is_function()) {
            Object* obj = val.is_function()
                ? static_cast<Object*>(val.as_function())
                : val.as_object();
            Value toString_method = obj->get_property("toString");
            if (toString_method.is_function()) {
                Value result = toString_method.as_function()->call(ctx, {}, val);
                if (!ctx.has_exception() && result.is_string()) {
                    return result.to_string();
                }
            }
        }
        return val.to_string();
    };

    auto str_includes_fn = ObjectFactory::create_native_function("includes",
        [obj_to_string](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();

            if (args.empty()) return Value(false);

            // ES6: throw TypeError if argument is a regexp (has Symbol.match truthy)
            if (args[0].is_object() || args[0].is_function()) {
                Object* arg_obj = args[0].is_function()
                    ? static_cast<Object*>(args[0].as_function())
                    : args[0].as_object();
                Value sym_match = arg_obj->get_property("Symbol.match");
                if (sym_match.is_undefined()) {
                    // No Symbol.match property - check if it's a RegExp
                    if (arg_obj->get_type() == Object::ObjectType::RegExp) {
                        ctx.throw_exception(Value(std::string("TypeError: First argument to String.prototype.includes must not be a regular expression")));
                        return Value();
                    }
                } else if (sym_match.to_boolean()) {
                    ctx.throw_exception(Value(std::string("TypeError: First argument to String.prototype.includes must not be a regular expression")));
                    return Value();
                }
            }

            if (args[0].is_symbol()) {
                ctx.throw_exception(Value(std::string("TypeError: Cannot convert a Symbol value to a string")));
                return Value();
            }

            std::string search_string = obj_to_string(ctx, args[0]);
            size_t position = 0;
            if (args.size() > 1) {
                if (args[1].is_symbol()) {
                    ctx.throw_exception(Value(std::string("TypeError: Cannot convert a Symbol value to a number")));
                    return Value();
                }
                position = static_cast<size_t>(std::max(0.0, args[1].to_number()));
            }

            if (position >= str.length()) {
                return Value(search_string.empty());
            }

            size_t found = str.find(search_string, position);
            return Value(found != std::string::npos);
        });
    PropertyDescriptor str_includes_length_desc(Value(1.0), PropertyAttributes::Configurable);
    str_includes_length_desc.set_enumerable(false);
    str_includes_length_desc.set_writable(false);
    str_includes_fn->set_property_descriptor("length", str_includes_length_desc);
    PropertyDescriptor string_includes_desc(Value(str_includes_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("includes", string_includes_desc);

    auto startsWith_fn = ObjectFactory::create_native_function("startsWith",
        [obj_to_string](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();

            if (args.empty()) return Value(false);

            // ES6: throw TypeError if argument is a regexp (has Symbol.match truthy)
            if (args[0].is_object() || args[0].is_function()) {
                Object* arg_obj = args[0].is_function()
                    ? static_cast<Object*>(args[0].as_function())
                    : args[0].as_object();
                Value sym_match = arg_obj->get_property("Symbol.match");
                if (sym_match.is_undefined()) {
                    if (arg_obj->get_type() == Object::ObjectType::RegExp) {
                        ctx.throw_exception(Value(std::string("TypeError: First argument to String.prototype.startsWith must not be a regular expression")));
                        return Value();
                    }
                } else if (sym_match.to_boolean()) {
                    ctx.throw_exception(Value(std::string("TypeError: First argument to String.prototype.startsWith must not be a regular expression")));
                    return Value();
                }
            }

            if (args[0].is_symbol()) {
                ctx.throw_exception(Value(std::string("TypeError: Cannot convert a Symbol value to a string")));
                return Value();
            }

            std::string search_string = obj_to_string(ctx, args[0]);
            size_t position = 0;
            if (args.size() > 1) {
                if (args[1].is_symbol()) {
                    ctx.throw_exception(Value(std::string("TypeError: Cannot convert a Symbol value to a number")));
                    return Value();
                }
                position = static_cast<size_t>(std::max(0.0, args[1].to_number()));
            }

            if (position >= str.length()) {
                return Value(search_string.empty());
            }

            return Value(str.substr(position, search_string.length()) == search_string);
        });
    PropertyDescriptor startsWith_length_desc(Value(1.0), PropertyAttributes::Configurable);
    startsWith_length_desc.set_enumerable(false);
    startsWith_length_desc.set_writable(false);
    startsWith_fn->set_property_descriptor("length", startsWith_length_desc);
    PropertyDescriptor startsWith_desc(Value(startsWith_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("startsWith", startsWith_desc);

    auto endsWith_fn = ObjectFactory::create_native_function("endsWith",
        [obj_to_string](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();

            if (args.empty()) return Value(false);

            // ES6: throw TypeError if argument is a regexp (has Symbol.match truthy)
            if (args[0].is_object() || args[0].is_function()) {
                Object* arg_obj = args[0].is_function()
                    ? static_cast<Object*>(args[0].as_function())
                    : args[0].as_object();
                Value sym_match = arg_obj->get_property("Symbol.match");
                if (sym_match.is_undefined()) {
                    if (arg_obj->get_type() == Object::ObjectType::RegExp) {
                        ctx.throw_exception(Value(std::string("TypeError: First argument to String.prototype.endsWith must not be a regular expression")));
                        return Value();
                    }
                } else if (sym_match.to_boolean()) {
                    ctx.throw_exception(Value(std::string("TypeError: First argument to String.prototype.endsWith must not be a regular expression")));
                    return Value();
                }
            }

            if (args[0].is_symbol()) {
                ctx.throw_exception(Value(std::string("TypeError: Cannot convert a Symbol value to a string")));
                return Value();
            }

            std::string search_string = obj_to_string(ctx, args[0]);
            size_t length = args.size() > 1 ?
                static_cast<size_t>(std::max(0.0, args[1].to_number())) : str.length();

            if (length > str.length()) length = str.length();
            if (search_string.length() > length) return Value(false);

            return Value(str.substr(length - search_string.length(), search_string.length()) == search_string);
        });
    PropertyDescriptor endsWith_length_desc(Value(1.0), PropertyAttributes::Configurable);
    endsWith_length_desc.set_enumerable(false);
    endsWith_length_desc.set_writable(false);
    endsWith_fn->set_property_descriptor("length", endsWith_length_desc);
    PropertyDescriptor endsWith_desc(Value(endsWith_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("endsWith", endsWith_desc);

    // Helper to convert this value to string for String.prototype methods
    auto toString_helper = [](Context& ctx, const Value& this_value) -> std::string {
        if (this_value.is_object() || this_value.is_function()) {
            Object* obj = this_value.is_object() ? this_value.as_object() : this_value.as_function();
            Value toString_method = obj->get_property("toString");
            if (!toString_method.is_undefined() && toString_method.is_function()) {
                Function* toString_fn = toString_method.as_function();
                std::vector<Value> empty_args;
                Value result = toString_fn->call(ctx, empty_args, this_value);
                if (ctx.has_exception()) {
                    return "";
                }
                return result.to_string();
            }
        }
        return this_value.to_string();
    };

    auto match_fn = ObjectFactory::create_native_function("match",
        [toString_helper](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string str = "";
            try {
                Value this_value = ctx.get_binding("this");
                str = toString_helper(ctx, this_value);
            } catch (...) {
                return Value();
            }

            if (args.empty()) return Value();

            Value pattern = args[0];

            // ES6: Check Symbol.match on the argument (skip for native RegExp - use built-in logic)
            if (pattern.is_object() || pattern.is_function()) {
                Object* pat_obj = pattern.is_function()
                    ? static_cast<Object*>(pattern.as_function())
                    : pattern.as_object();
                bool is_native_regexp = pat_obj->get_type() == Object::ObjectType::RegExp;
                if (!is_native_regexp) {
                    Value sym_match = pat_obj->get_property("Symbol.match");
                    if (sym_match.is_function()) {
                        return sym_match.as_function()->call(ctx, {Value(str)}, pattern);
                    }
                    // Not a function: ToPrimitive(pattern, "string") fires get(Symbol.toPrimitive)
                    Value sym_to_prim = pat_obj->get_property("Symbol.toPrimitive");
                    if (sym_to_prim.is_function()) {
                        Value prim = sym_to_prim.as_function()->call(ctx, {Value(std::string("string"))}, pattern);
                        if (ctx.has_exception()) return Value();
                        pattern = prim;
                    } else {
                        pattern = Value(pattern.to_string());
                    }
                }
            }

            if (pattern.is_object()) {
                Object* regex_obj = pattern.as_object();

                Value global_val = regex_obj->get_property("global");
                bool is_global = global_val.is_boolean() && global_val.to_boolean();

                Value exec_method = regex_obj->get_property("exec");
                if (exec_method.is_function()) {
                    Function* exec_func = exec_method.as_function();

                    if (is_global) {
                        regex_obj->set_property("lastIndex", Value(0.0));

                        auto result_array = ObjectFactory::create_array();
                        size_t match_count = 0;
                        size_t safety_counter = 0;
                        const size_t max_iterations = str.length() + 1;

                        while (safety_counter++ < max_iterations) {
                            Value last_index_before = regex_obj->get_property("lastIndex");
                            int index_before = last_index_before.is_number() ? static_cast<int>(last_index_before.to_number()) : 0;

                            std::vector<Value> exec_args = { Value(str) };
                            Value match_result = exec_func->call(ctx, exec_args, pattern);

                            if (match_result.is_null() || !match_result.is_object()) {
                                break;
                            }

                            Object* match_obj = match_result.as_object();
                            Value matched_str = match_obj->get_element(0);
                            result_array->set_element(match_count++, matched_str);

                            Value last_index_after = regex_obj->get_property("lastIndex");
                            int index_after = last_index_after.is_number() ? static_cast<int>(last_index_after.to_number()) : 0;

                            if (index_after == index_before) {
                                regex_obj->set_property("lastIndex", Value(static_cast<double>(index_after + 1)));
                            }
                        }

                        if (match_count == 0) {
                            return Value::null();
                        }

                        return Value(result_array.release());
                    } else {
                        std::vector<Value> exec_args = { Value(str) };
                        return exec_func->call(ctx, exec_args, pattern);
                    }
                }
            }

            std::string search = pattern.to_string();
            size_t pos = str.find(search);

            if (pos != std::string::npos) {
                auto result = ObjectFactory::create_array();
                result->set_element(0, Value(search));
                result->set_property("index", Value(static_cast<double>(pos)));
                result->set_property("input", Value(str));
                return Value(result.release());
            }

            return Value::null();
        });
    PropertyDescriptor match_desc(Value(match_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("match", match_desc);

    auto matchAll_fn = ObjectFactory::create_native_function("matchAll",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string str = "";
            try {
                Value this_value = ctx.get_binding("this");
                str = this_value.to_string();
            } catch (...) {
                return Value();
            }

            if (args.empty()) {
                ctx.throw_type_error("matchAll requires a regexp argument");
                return Value();
            }

            Value regex_val = args[0];
            if (!regex_val.is_object()) {
                ctx.throw_type_error("matchAll argument must be a RegExp");
                return Value();
            }
            Object* regex_obj = regex_val.as_object();

            Value global_val = regex_obj->get_property("global");
            if (!global_val.is_boolean() || !global_val.as_boolean()) {
                ctx.throw_type_error("String.prototype.matchAll called with a non-global RegExp argument");
                return Value();
            }

            auto shared_str = std::make_shared<std::string>(str);
            auto shared_regex = std::make_shared<Value>(regex_val);
            auto done_flag = std::make_shared<bool>(false);

            auto iterator = ObjectFactory::create_object();
            Object* iter_ptr = iterator.get();

            auto next_fn = ObjectFactory::create_native_function("next",
                [shared_str, shared_regex, done_flag](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)args;
                    auto result = ObjectFactory::create_object();
                    if (*done_flag) {
                        result->set_property("done", Value(true));
                        result->set_property("value", Value());
                        return Value(result.release());
                    }
                    Object* rx = shared_regex->as_object();
                    Value exec_method = rx->get_property("exec");
                    if (!exec_method.is_function()) {
                        *done_flag = true;
                        result->set_property("done", Value(true));
                        result->set_property("value", Value());
                        return Value(result.release());
                    }
                    Value match = exec_method.as_function()->call(ctx, {Value(*shared_str)}, *shared_regex);
                    if (ctx.has_exception()) return Value();
                    if (match.is_null() || match.is_undefined()) {
                        *done_flag = true;
                        result->set_property("done", Value(true));
                        result->set_property("value", Value());
                    } else {
                        result->set_property("done", Value(false));
                        result->set_property("value", match);
                    }
                    return Value(result.release());
                }, 0);
            iterator->set_property("next", Value(next_fn.release()));

            auto sym_iter_fn = ObjectFactory::create_native_function("[Symbol.iterator]",
                [iter_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx; (void)args;
                    return Value(iter_ptr);
                }, 0);
            iterator->set_property("Symbol.iterator", Value(sym_iter_fn.release()));

            return Value(iterator.release());
        }, 1);
    PropertyDescriptor matchAll_desc(Value(matchAll_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("matchAll", matchAll_desc);

    auto search_fn = ObjectFactory::create_native_function("search",
        [toString_helper](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string str = "";
            try {
                Value this_value = ctx.get_binding("this");
                str = toString_helper(ctx, this_value);
            } catch (...) {
                return Value(-1.0);
            }

            if (args.empty()) return Value(-1.0);

            Value pattern = args[0];

            // ES6: Check Symbol.search on the argument (skip for native RegExp)
            if (pattern.is_object() || pattern.is_function()) {
                Object* pat_obj = pattern.is_function()
                    ? static_cast<Object*>(pattern.as_function())
                    : pattern.as_object();
                bool is_native_regexp = pat_obj->get_type() == Object::ObjectType::RegExp;
                if (!is_native_regexp) {
                    Value sym_search = pat_obj->get_property("Symbol.search");
                    if (sym_search.is_function()) {
                        return sym_search.as_function()->call(ctx, {Value(str)}, pattern);
                    }
                    // Not a function: ToPrimitive(pattern, "string") fires get(Symbol.toPrimitive)
                    Value sym_to_prim = pat_obj->get_property("Symbol.toPrimitive");
                    if (sym_to_prim.is_function()) {
                        Value prim = sym_to_prim.as_function()->call(ctx, {Value(std::string("string"))}, pattern);
                        if (ctx.has_exception()) return Value(-1.0);
                        pattern = prim;
                    } else {
                        pattern = Value(pattern.to_string());
                    }
                }
            }

            Object* regex_obj = nullptr;

            if (pattern.is_object()) {
                regex_obj = pattern.as_object();
            } else {
                std::string pattern_str = pattern.to_string();
                try {
                    auto regexp_impl = std::make_shared<RegExp>(pattern_str, "");
                    auto temp_regex = ObjectFactory::create_object();
                    Object* temp_ptr = temp_regex.get();

                    auto temp_exec = ObjectFactory::create_native_function("exec",
                        [regexp_impl, temp_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
                            (void)ctx;
                            if (args.empty()) return Value::null();
                            Value lastIndex_val = temp_ptr->get_property("lastIndex");
                            if (lastIndex_val.is_number()) {
                                regexp_impl->set_last_index(static_cast<int>(lastIndex_val.to_number()));
                            }
                            std::string str = args[0].to_string();
                            Value result = regexp_impl->exec(str);
                            temp_ptr->set_property("lastIndex", Value(static_cast<double>(regexp_impl->get_last_index())));
                            return result;
                        });

                    temp_regex->set_property("exec", Value(temp_exec.release()));
                    temp_regex->set_property("lastIndex", Value(0.0));
                    regex_obj = temp_regex.release();
                    pattern = Value(regex_obj);
                } catch (...) {
                    return Value(-1.0);
                }
            }

            if (regex_obj) {
                Value exec_method = regex_obj->get_property("exec");
                if (exec_method.is_function()) {
                    Function* exec_func = exec_method.as_function();

                    Value saved_lastIndex = regex_obj->get_property("lastIndex");
                    regex_obj->set_property("lastIndex", Value(0.0));

                    std::vector<Value> exec_args = { Value(str) };
                    Value match_result = exec_func->call(ctx, exec_args, pattern);

                    regex_obj->set_property("lastIndex", saved_lastIndex);

                    if (match_result.is_null() || !match_result.is_object()) {
                        return Value(-1.0);
                    }

                    Object* match_obj = match_result.as_object();
                    Value index_val = match_obj->get_property("index");
                    if (index_val.is_number()) {
                        return index_val;
                    }
                }
            }

            return Value(-1.0);
        });
    PropertyDescriptor search_desc(Value(search_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("search", search_desc);

    auto replace_fn = ObjectFactory::create_native_function("replace",
        [toString_helper](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string str = "";
            try {
                Value this_value = ctx.get_binding("this");
                str = toString_helper(ctx, this_value);
            } catch (...) {
                return Value(std::string(""));
            }

            // ES6: Check Symbol.replace on the first argument (skip for native RegExp)
            if (!args.empty() && (args[0].is_object() || args[0].is_function())) {
                Object* pat_obj = args[0].is_function()
                    ? static_cast<Object*>(args[0].as_function())
                    : args[0].as_object();
                bool is_native_regexp = pat_obj->get_type() == Object::ObjectType::RegExp;
                if (!is_native_regexp) {
                    Value sym_replace = pat_obj->get_property("Symbol.replace");
                    if (sym_replace.is_function()) {
                        std::vector<Value> call_args = {Value(str)};
                        if (args.size() >= 2) call_args.push_back(args[1]);
                        return sym_replace.as_function()->call(ctx, call_args, args[0]);
                    }
                    // Not a function: ToPrimitive(pattern, "string") fires get(Symbol.toPrimitive)
                    Value sym_to_prim = pat_obj->get_property("Symbol.toPrimitive");
                    std::string pat_str;
                    if (sym_to_prim.is_function()) {
                        Value prim = sym_to_prim.as_function()->call(ctx, {Value(std::string("string"))}, args[0]);
                        if (ctx.has_exception()) return Value(str);
                        pat_str = prim.is_undefined() ? "" : prim.to_string();
                    } else {
                        pat_str = pat_obj->to_string();
                    }
                    std::string replacement = args.size() >= 2 ? args[1].to_string() : "";
                    size_t pos = str.find(pat_str);
                    if (pos != std::string::npos) {
                        return Value(str.substr(0, pos) + replacement + str.substr(pos + pat_str.length()));
                    }
                    return Value(str);
                }
            }

            if (args.size() < 2) return Value(str);

            Value search_val = args[0];

            std::string replacement = args[1].to_string();

            auto process_replacement = [](const std::string& repl, Object* match_obj, const std::string& orig_str, int match_pos) -> std::string {
                std::string result;
                for (size_t i = 0; i < repl.length(); ++i) {
                    if (repl[i] == '$' && i + 1 < repl.length()) {
                        if (repl[i + 1] == '$') {
                            result += '$';
                            ++i;
                        } else if (repl[i + 1] == '&') {
                            Value matched = match_obj->get_element(0);
                            if (!matched.is_undefined()) {
                                result += matched.to_string();
                            }
                            ++i;
                        } else if (repl[i + 1] == '`') {
                            result += orig_str.substr(0, match_pos);
                            ++i;
                        } else if (repl[i + 1] == '\'') {
                            Value matched = match_obj->get_element(0);
                            if (!matched.is_undefined()) {
                                std::string matched_str = matched.to_string();
                                int after_pos = match_pos + matched_str.length();
                                if (after_pos < static_cast<int>(orig_str.length())) {
                                    result += orig_str.substr(after_pos);
                                }
                            }
                            ++i;
                        } else if (isdigit(repl[i + 1])) {
                            size_t j = i + 1;
                            while (j < repl.length() && isdigit(repl[j])) {
                                ++j;
                            }
                            std::string num_str = repl.substr(i + 1, j - i - 1);
                            int capture_num = std::stoi(num_str);

                            Value capture_val = match_obj->get_element(capture_num);
                            if (!capture_val.is_undefined()) {
                                result += capture_val.to_string();
                            }

                            i = j - 1;
                        } else {
                            result += repl[i];
                        }
                    } else {
                        result += repl[i];
                    }
                }
                return result;
            };

            if (search_val.is_object()) {
                Object* regex_obj = search_val.as_object();

                Value global_val = regex_obj->get_property("global");
                bool is_global = global_val.is_boolean() && global_val.to_boolean();

                Value exec_method = regex_obj->get_property("exec");
                if (exec_method.is_function()) {
                    Function* exec_func = exec_method.as_function();

                    if (is_global) {
                        regex_obj->set_property("lastIndex", Value(0.0));

                        std::string result = str;
                        int offset = 0;

                        while (true) {
                            std::vector<Value> exec_args = { Value(str) };
                            Value match_result = exec_func->call(ctx, exec_args, search_val);

                            if (match_result.is_null() || !match_result.is_object()) {
                                break;
                            }

                            Object* match_obj = match_result.as_object();
                            Value index_val = match_obj->get_property("index");
                            Value matched_str = match_obj->get_element(0);

                            if (index_val.is_number()) {
                                int match_index = static_cast<int>(index_val.to_number());
                                std::string matched = matched_str.to_string();

                                std::string processed_repl = process_replacement(replacement, match_obj, str, match_index);

                                result.replace(match_index + offset, matched.length(), processed_repl);
                                offset += processed_repl.length() - matched.length();
                            }
                        }

                        return Value(result);
                    } else {
                        std::vector<Value> exec_args = { Value(str) };
                        Value match_result = exec_func->call(ctx, exec_args, search_val);

                        if (match_result.is_object()) {
                            Object* match_arr = match_result.as_object();
                            Value index_val = match_arr->get_property("index");
                            Value match_str = match_arr->get_element(0);

                            if (index_val.is_number() && !match_str.is_undefined()) {
                                size_t pos = static_cast<size_t>(index_val.to_number());
                                std::string matched = match_str.to_string();

                                std::string processed_repl = process_replacement(replacement, match_arr, str, static_cast<int>(pos));

                                str.replace(pos, matched.length(), processed_repl);
                                return Value(str);
                            }
                        }
                    }
                }
            }

            std::string search = search_val.to_string();
            size_t pos = str.find(search);

            if (pos != std::string::npos) {
                str.replace(pos, search.length(), replacement);
            }

            return Value(str);
        });
    PropertyDescriptor replace_desc(Value(replace_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("replace", replace_desc);

    auto replaceAll_fn = ObjectFactory::create_native_function("replaceAll",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();

            if (args.size() < 2) return Value(str);

            std::string search = args[0].to_string();
            bool is_function = args[1].is_function();

            if (search.empty()) return Value(str);

            std::vector<size_t> positions;
            size_t pos = 0;
            while ((pos = str.find(search, pos)) != std::string::npos) {
                positions.push_back(pos);
                pos += search.length();
            }

            for (auto it = positions.rbegin(); it != positions.rend(); ++it) {
                std::string replacement;
                if (is_function) {
                    Function* replacer = args[1].as_function();
                    std::vector<Value> fn_args = {
                        Value(search),
                        Value(static_cast<double>(*it)),
                        Value(this_value.to_string())
                    };
                    Value result = replacer->call(ctx, fn_args);
                    if (ctx.has_exception()) return Value();
                    replacement = result.to_string();
                } else {
                    replacement = args[1].to_string();
                }
                str.replace(*it, search.length(), replacement);
            }

            return Value(str);
        }, 2);
    PropertyDescriptor replaceAll_desc(Value(replaceAll_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("replaceAll", replaceAll_desc);

    auto trim_fn = ObjectFactory::create_native_function("trim",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();

            size_t start = str.find_first_not_of(" \t\n\r\f\v");
            if (start == std::string::npos) return Value(std::string(""));

            size_t end = str.find_last_not_of(" \t\n\r\f\v");
            return Value(str.substr(start, end - start + 1));
        }, 0);
    PropertyDescriptor trim_desc(Value(trim_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("trim", trim_desc);

    auto trimStart_fn = ObjectFactory::create_native_function("trimStart",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();

            size_t start = str.find_first_not_of(" \t\n\r\f\v");
            if (start == std::string::npos) return Value(std::string(""));

            return Value(str.substr(start));
        }, 0);
    PropertyDescriptor trimStart_desc(Value(trimStart_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("trimStart", trimStart_desc);
    string_prototype->set_property_descriptor("trimLeft", trimStart_desc);

    auto trimEnd_fn = ObjectFactory::create_native_function("trimEnd",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();

            size_t end = str.find_last_not_of(" \t\n\r\f\v");
            if (end == std::string::npos) return Value(std::string(""));

            return Value(str.substr(0, end + 1));
        }, 0);
    PropertyDescriptor trimEnd_desc(Value(trimEnd_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("trimEnd", trimEnd_desc);
    string_prototype->set_property_descriptor("trimRight", trimEnd_desc);

    auto codePointAt_fn = ObjectFactory::create_native_function("codePointAt",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();

            if (args.size() == 0 || str.empty()) return Value();

            int32_t pos = static_cast<int32_t>(args[0].to_number());
            if (pos < 0 || pos >= static_cast<int32_t>(str.length())) {
                return Value();
            }

            unsigned char ch = str[pos];

            if ((ch & 0x80) == 0) {
                return Value(static_cast<double>(ch));
            } else if ((ch & 0xE0) == 0xC0) {
                if (pos + 1 < static_cast<int32_t>(str.length())) {
                    uint32_t codePoint = ((ch & 0x1F) << 6) | (str[pos + 1] & 0x3F);
                    return Value(static_cast<double>(codePoint));
                }
            } else if ((ch & 0xF0) == 0xE0) {
                if (pos + 2 < static_cast<int32_t>(str.length())) {
                    uint32_t codePoint = ((ch & 0x0F) << 12) |
                                        ((str[pos + 1] & 0x3F) << 6) |
                                        (str[pos + 2] & 0x3F);
                    return Value(static_cast<double>(codePoint));
                }
            } else if ((ch & 0xF8) == 0xF0) {
                if (pos + 3 < static_cast<int32_t>(str.length())) {
                    uint32_t codePoint = ((ch & 0x07) << 18) |
                                        ((str[pos + 1] & 0x3F) << 12) |
                                        ((str[pos + 2] & 0x3F) << 6) |
                                        (str[pos + 3] & 0x3F);
                    return Value(static_cast<double>(codePoint));
                }
            }

            return Value(static_cast<double>(ch));
        }, 1);
    PropertyDescriptor codePointAt_desc(Value(codePointAt_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("codePointAt", codePointAt_desc);

    auto localeCompare_fn = ObjectFactory::create_native_function("localeCompare",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();

            if (args.size() == 0) return Value(0.0);

            std::string that = args[0].to_string();

            if (str < that) return Value(-1.0);
            if (str > that) return Value(1.0);
            return Value(0.0);
        }, 1);
    PropertyDescriptor localeCompare_desc(Value(localeCompare_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("localeCompare", localeCompare_desc);

    auto charAt_fn = ObjectFactory::create_native_function("charAt",
        [toString_helper](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            std::string str = toString_helper(ctx, this_value);

            uint32_t index = 0;
            if (args.size() > 0) {
                index = static_cast<uint32_t>(args[0].to_number());
            }

            if (index >= str.length()) {
                return Value(std::string(""));
            }

            return Value(std::string(1, str[index]));
        });
    PropertyDescriptor charAt_desc(Value(charAt_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("charAt", charAt_desc);

    auto string_at_fn = ObjectFactory::create_native_function("at",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();

            if (args.empty()) {
                return Value();
            }

            int64_t index = static_cast<int64_t>(args[0].to_number());
            int64_t len = static_cast<int64_t>(str.length());

            if (index < 0) {
                index = len + index;
            }

            if (index < 0 || index >= len) {
                return Value();
            }

            return Value(std::string(1, str[static_cast<size_t>(index)]));
        }, 1);
    PropertyDescriptor string_at_desc(Value(string_at_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("at", string_at_desc);

    auto charCodeAt_fn = ObjectFactory::create_native_function("charCodeAt",
        [toString_helper](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            std::string str = toString_helper(ctx, this_value);

            uint32_t index = 0;
            if (args.size() > 0) {
                index = static_cast<uint32_t>(args[0].to_number());
            }

            if (index >= str.length()) {
                return Value(std::numeric_limits<double>::quiet_NaN());
            }

            return Value(static_cast<double>(static_cast<unsigned char>(str[index])));
        });
    PropertyDescriptor charCodeAt_desc(Value(charCodeAt_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("charCodeAt", charCodeAt_desc);

    auto str_indexOf_fn = ObjectFactory::create_native_function("indexOf",
        [toString_helper](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            std::string str = toString_helper(ctx, this_value);

            if (args.empty()) {
                return Value(-1.0);
            }

            std::string search = args[0].to_string();
            size_t start = 0;
            if (args.size() > 1) {
                double pos = args[1].to_number();
                // ES1: If position is NaN, treat as 0; if negative, treat as 0
                if (std::isnan(pos) || pos < 0) {
                    start = 0;
                } else {
                    start = static_cast<size_t>(pos);
                }
            }

            size_t found_pos = str.find(search, start);
            return Value(found_pos == std::string::npos ? -1.0 : static_cast<double>(found_pos));
        }, 1);
    PropertyDescriptor string_indexOf_desc(Value(str_indexOf_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("indexOf", string_indexOf_desc);

    auto str_split_fn = ObjectFactory::create_native_function("split",
        [toString_helper](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            std::string str = toString_helper(ctx, this_value);

            // ES6: Check Symbol.split on the separator argument (skip for native RegExp)
            if (!args.empty() && (args[0].is_object() || args[0].is_function())) {
                Object* sep_obj = args[0].is_function()
                    ? static_cast<Object*>(args[0].as_function())
                    : args[0].as_object();
                bool is_native_regexp = sep_obj->get_type() == Object::ObjectType::RegExp;
                if (!is_native_regexp) {
                    Value sym_split = sep_obj->get_property("Symbol.split");
                    if (sym_split.is_function()) {
                        std::vector<Value> call_args = {Value(str)};
                        if (args.size() >= 2) call_args.push_back(args[1]);
                        return sym_split.as_function()->call(ctx, call_args, args[0]);
                    }
                    // Not a function: ToPrimitive(sep, "string") fires get(Symbol.toPrimitive)
                    Value sym_to_prim = sep_obj->get_property("Symbol.toPrimitive");
                    std::string sep_str;
                    if (sym_to_prim.is_function()) {
                        Value prim = sym_to_prim.as_function()->call(ctx, {Value(std::string("string"))}, args[0]);
                        if (ctx.has_exception()) return Value(ObjectFactory::create_array(0).release());
                        sep_str = (prim.is_undefined() || prim.is_null()) ? "" : prim.to_string();
                    } else {
                        sep_str = sep_obj->to_string();
                    }
                    // Do string split with sep_str and return
                    auto split_result = ObjectFactory::create_array(0);
                    uint32_t split_idx = 0;
                    if (sep_str.empty()) {
                        for (size_t ci = 0; ci < str.size(); ci++) {
                            split_result->set_element(split_idx++, Value(std::string(1, str[ci])));
                        }
                    } else {
                        size_t spos = 0, sfound;
                        while ((sfound = str.find(sep_str, spos)) != std::string::npos) {
                            split_result->set_element(split_idx++, Value(str.substr(spos, sfound - spos)));
                            spos = sfound + sep_str.length();
                        }
                        split_result->set_element(split_idx++, Value(str.substr(spos)));
                    }
                    split_result->set_length(split_idx);
                    return Value(split_result.release());
                }
            }

            auto result_array = ObjectFactory::create_array(0);

            int limit = -1;
            if (args.size() >= 2 && args[1].is_number()) {
                limit = static_cast<int>(args[1].to_number());
                if (limit == 0) {
                    return Value(result_array.release());
                }
            }

            if (args.empty() || args[0].is_undefined()) {
                result_array->set_element(0, Value(str));
                return Value(result_array.release());
            }

            Value sep_val = args[0];

            if (sep_val.is_object()) {
                Object* regex_obj = sep_val.as_object();
                Value exec_method = regex_obj->get_property("exec");

                if (exec_method.is_function()) {
                    Function* exec_func = exec_method.as_function();

                    if (str.empty()) {
                        std::vector<Value> exec_args = { Value(str) };
                        Value match_result = exec_func->call(ctx, exec_args, sep_val);

                        if (!match_result.is_null() && match_result.is_object()) {
                            Object* match_obj = match_result.as_object();
                            Value matched_val = match_obj->get_element(0);
                            if (matched_val.to_string().empty()) {
                                return Value(result_array.release());
                            }
                        }
                        result_array->set_element(0, Value(str));
                        return Value(result_array.release());
                    }

                    uint32_t arr_index = 0;
                    size_t search_pos = 0;
                    bool last_was_empty_at_start = false;

                    while (search_pos <= str.length()) {
                        if (limit >= 0 && static_cast<int>(arr_index) >= limit) break;

                        std::string remaining = str.substr(search_pos);
                        if (remaining.empty()) break;

                        std::vector<Value> exec_args = { Value(remaining) };
                        Value match_result = exec_func->call(ctx, exec_args, sep_val);

                        if (match_result.is_null() || !match_result.is_object()) {
                            break;
                        }

                        Object* match_obj = match_result.as_object();
                        Value index_val = match_obj->get_property("index");
                        Value matched_val = match_obj->get_element(0);

                        if (!index_val.is_number()) break;

                        size_t match_pos_in_remaining = static_cast<size_t>(index_val.to_number());
                        size_t actual_match_pos = search_pos + match_pos_in_remaining;
                        std::string matched = matched_val.to_string();

                        if (matched.empty() && match_pos_in_remaining == 0) {
                            if (search_pos < str.length()) {
                                result_array->set_element(arr_index++, Value(std::string(1, str[search_pos])));
                                search_pos++;
                                last_was_empty_at_start = true;
                            } else {
                                break;
                            }
                        } else {
                            result_array->set_element(arr_index++, Value(str.substr(search_pos, actual_match_pos - search_pos)));

                            if (limit >= 0 && static_cast<int>(arr_index) >= limit) break;

                            Value length_val = match_obj->get_property("length");
                            if (length_val.is_number()) {
                                int num_captures = static_cast<int>(length_val.to_number());
                                for (int i = 1; i < num_captures; ++i) {
                                    if (limit >= 0 && static_cast<int>(arr_index) >= limit) break;
                                    Value capture = match_obj->get_element(i);
                                    result_array->set_element(arr_index++, capture);
                                }
                            }

                            if (matched.empty()) {
                                search_pos = actual_match_pos + 1;
                            } else {
                                search_pos = actual_match_pos + matched.length();
                            }
                            last_was_empty_at_start = false;
                        }
                    }

                    if (limit < 0 || static_cast<int>(arr_index) < limit) {
                        if (search_pos < str.length() || (search_pos == str.length() && !last_was_empty_at_start)) {
                            result_array->set_element(arr_index, Value(str.substr(search_pos)));
                        }
                    }

                    return Value(result_array.release());
                }
            }

            std::string separator = sep_val.to_string();

            if (separator.empty()) {
                for (size_t i = 0; i < str.length(); ++i) {
                    if (limit >= 0 && static_cast<int>(i) >= limit) break;
                    result_array->set_element(i, Value(std::string(1, str[i])));
                }
            } else {
                size_t start = 0;
                size_t end = 0;
                uint32_t index = 0;

                while ((end = str.find(separator, start)) != std::string::npos) {
                    if (limit >= 0 && static_cast<int>(index) >= limit) break;
                    result_array->set_element(index++, Value(str.substr(start, end - start)));
                    start = end + separator.length();
                }
                if (limit < 0 || static_cast<int>(index) < limit) {
                    result_array->set_element(index, Value(str.substr(start)));
                }
            }

            return Value(result_array.release());
        }, 1);
    PropertyDescriptor string_split_desc(Value(str_split_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("split", string_split_desc);

    auto toLowerCase_fn = ObjectFactory::create_native_function("toLowerCase",
        [toString_helper](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            std::string str = toString_helper(ctx, this_value);

            std::transform(str.begin(), str.end(), str.begin(),
                [](unsigned char c) { return std::tolower(c); });

            return Value(str);
        });
    PropertyDescriptor toLowerCase_desc(Value(toLowerCase_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("toLowerCase", toLowerCase_desc);

    auto str_concat_fn = ObjectFactory::create_native_function("concat",
        [toString_helper](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            std::string result = toString_helper(ctx, this_value);

            for (const auto& arg : args) {
                result += arg.to_string();
            }

            return Value(result);
        });
    PropertyDescriptor str_concat_desc(Value(str_concat_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("concat", str_concat_desc);

    auto toUpperCase_fn = ObjectFactory::create_native_function("toUpperCase",
        [toString_helper](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            std::string str = toString_helper(ctx, this_value);

            std::transform(str.begin(), str.end(), str.begin(),
                [](unsigned char c) { return std::toupper(c); });

            return Value(str);
        });
    PropertyDescriptor toUpperCase_desc(Value(toUpperCase_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("toUpperCase", toUpperCase_desc);

    auto toLocaleLowerCase_fn = ObjectFactory::create_native_function("toLocaleLowerCase",
        [toString_helper](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            std::string str = toString_helper(ctx, this_value);

            std::transform(str.begin(), str.end(), str.begin(),
                [](unsigned char c) { return std::tolower(c); });

            return Value(str);
        });
    PropertyDescriptor toLocaleLowerCase_desc(Value(toLocaleLowerCase_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("toLocaleLowerCase", toLocaleLowerCase_desc);

    auto toLocaleUpperCase_fn = ObjectFactory::create_native_function("toLocaleUpperCase",
        [toString_helper](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            std::string str = toString_helper(ctx, this_value);

            std::transform(str.begin(), str.end(), str.begin(),
                [](unsigned char c) { return std::toupper(c); });

            return Value(str);
        });
    PropertyDescriptor toLocaleUpperCase_desc(Value(toLocaleUpperCase_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("toLocaleUpperCase", toLocaleUpperCase_desc);

    auto str_slice_fn = ObjectFactory::create_native_function("slice",
        [toString_helper](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            std::string str = toString_helper(ctx, this_value);
            int len = static_cast<int>(str.length());

            if (args.empty()) return Value(str);

            int start = static_cast<int>(args[0].to_number());
            int end = (args.size() > 1 && !args[1].is_undefined())
                ? static_cast<int>(args[1].to_number()) : len;

            if (start < 0) start = std::max(len + start, 0);
            else start = std::min(start, len);

            if (end < 0) end = std::max(len + end, 0);
            else end = std::min(end, len);

            if (start >= end) return Value(std::string(""));
            return Value(str.substr(start, end - start));
        }, 2);
    PropertyDescriptor str_slice_desc(Value(str_slice_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("slice", str_slice_desc);

    // ES1: 15.5.4.7 String.prototype.lastIndexOf(searchString, position)
    auto str_lastIndexOf_fn = ObjectFactory::create_native_function("lastIndexOf",
        [toString_helper](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            std::string str = toString_helper(ctx, this_value);

            if (args.empty()) {
                return Value(-1.0);
            }

            std::string search = args[0].to_string();
            size_t start = str.length();

            if (args.size() > 1) {
                double pos = args[1].to_number();
                if (std::isnan(pos) || pos >= static_cast<double>(str.length())) {
                    start = str.length();
                } else if (pos < 0) {
                    start = 0;
                } else {
                    start = static_cast<size_t>(pos) + search.length();
                    if (start > str.length()) {
                        start = str.length();
                    }
                }
            }

            // Search backwards from start position
            if (search.empty()) {
                return Value(static_cast<double>(std::min(start, str.length())));
            }

            size_t found_pos = str.rfind(search, start);
            return Value(found_pos == std::string::npos ? -1.0 : static_cast<double>(found_pos));
        }, 1);
    PropertyDescriptor str_lastIndexOf_desc(Value(str_lastIndexOf_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("lastIndexOf", str_lastIndexOf_desc);

    // ES1: 15.5.4.10 String.prototype.substring(start, end)
    auto str_substring_fn = ObjectFactory::create_native_function("substring",
        [toString_helper](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            std::string str = toString_helper(ctx, this_value);

            size_t len = str.length();
            size_t start = 0;
            size_t end = len;

            if (args.size() > 0) {
                double start_num = args[0].to_number();
                if (std::isnan(start_num) || start_num < 0) {
                    start = 0;
                } else if (start_num > static_cast<double>(len)) {
                    start = len;
                } else {
                    start = static_cast<size_t>(start_num);
                }
            }

            if (args.size() > 1) {
                double end_num = args[1].to_number();
                if (std::isnan(end_num) || end_num < 0) {
                    end = 0;
                } else if (end_num > static_cast<double>(len)) {
                    end = len;
                } else {
                    end = static_cast<size_t>(end_num);
                }
            }

            // ES1: If start > end, swap them
            if (start > end) {
                std::swap(start, end);
            }

            if (start >= len) {
                return Value(std::string(""));
            }

            return Value(str.substr(start, end - start));
        }, 2);
    PropertyDescriptor str_substring_desc(Value(str_substring_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("substring", str_substring_desc);

    auto string_concat_static = ObjectFactory::create_native_function("concat",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string result = "";
            for (const auto& arg : args) {
                result += arg.to_string();
            }
            return Value(result);
        });
    string_constructor->set_property("concat", Value(string_concat_static.release()));


    // Helper lambda for HTML escaping attribute values (per ES6 spec: only escape ")
    auto html_escape_attr = [](const std::string& s) -> std::string {
        std::string result;
        for (char c : s) {
            if (c == '"') result += "&quot;";
            else result += c;
        }
        return result;
    };

    auto anchor_fn = ObjectFactory::create_native_function("anchor",
        [html_escape_attr](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");

            // RequireObjectCoercible
            if (this_value.is_null() || this_value.is_undefined()) {
                ctx.throw_type_error("Cannot call String.prototype.anchor on null or undefined");
                return Value();
            }

            std::string str = this_value.to_string();
            std::string name = args.size() > 0 ? html_escape_attr(args[0].to_string()) : "";
            return Value(std::string("<a name=\"") + name + "\">" + str + "</a>");
        }, 1);
    PropertyDescriptor anchor_desc(Value(anchor_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("anchor", anchor_desc);

    auto big_fn = ObjectFactory::create_native_function("big",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            if (this_value.is_null() || this_value.is_undefined()) {
                ctx.throw_type_error("Cannot call method on null or undefined");
                return Value();
            }
            std::string str = this_value.to_string();
            return Value(std::string("<big>") + str + "</big>");
        }, 0);
    PropertyDescriptor big_desc(Value(big_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("big", big_desc);

    auto blink_fn = ObjectFactory::create_native_function("blink",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            if (this_value.is_null() || this_value.is_undefined()) {
                ctx.throw_type_error("Cannot call method on null or undefined");
                return Value();
            }
            std::string str = this_value.to_string();
            return Value(std::string("<blink>") + str + "</blink>");
        }, 0);
    PropertyDescriptor blink_desc(Value(blink_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("blink", blink_desc);

    auto bold_fn = ObjectFactory::create_native_function("bold",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            if (this_value.is_null() || this_value.is_undefined()) {
                ctx.throw_type_error("Cannot call method on null or undefined");
                return Value();
            }
            std::string str = this_value.to_string();
            return Value(std::string("<b>") + str + "</b>");
        }, 0);
    PropertyDescriptor bold_desc(Value(bold_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("bold", bold_desc);

    auto fixed_fn = ObjectFactory::create_native_function("fixed",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            if (this_value.is_null() || this_value.is_undefined()) {
                ctx.throw_type_error("Cannot call method on null or undefined");
                return Value();
            }
            std::string str = this_value.to_string();
            return Value(std::string("<tt>") + str + "</tt>");
        }, 0);
    PropertyDescriptor fixed_desc(Value(fixed_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("fixed", fixed_desc);

    auto fontcolor_fn = ObjectFactory::create_native_function("fontcolor",
        [html_escape_attr](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            if (this_value.is_null() || this_value.is_undefined()) {
                ctx.throw_type_error("Cannot call method on null or undefined");
                return Value();
            }
            std::string str = this_value.to_string();
            std::string color = args.size() > 0 ? html_escape_attr(args[0].to_string()) : "";
            return Value(std::string("<font color=\"") + color + "\">" + str + "</font>");
        }, 1);
    PropertyDescriptor fontcolor_desc(Value(fontcolor_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("fontcolor", fontcolor_desc);

    auto fontsize_fn = ObjectFactory::create_native_function("fontsize",
        [html_escape_attr](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            if (this_value.is_null() || this_value.is_undefined()) {
                ctx.throw_type_error("Cannot call method on null or undefined");
                return Value();
            }
            std::string str = this_value.to_string();
            std::string size = args.size() > 0 ? html_escape_attr(args[0].to_string()) : "";
            return Value(std::string("<font size=\"") + size + "\">" + str + "</font>");
        }, 1);
    PropertyDescriptor fontsize_desc(Value(fontsize_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("fontsize", fontsize_desc);

    auto italics_fn = ObjectFactory::create_native_function("italics",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            if (this_value.is_null() || this_value.is_undefined()) {
                ctx.throw_type_error("Cannot call method on null or undefined");
                return Value();
            }
            std::string str = this_value.to_string();
            return Value(std::string("<i>") + str + "</i>");
        }, 0);
    PropertyDescriptor italics_desc(Value(italics_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("italics", italics_desc);

    auto link_fn = ObjectFactory::create_native_function("link",
        [html_escape_attr](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            if (this_value.is_null() || this_value.is_undefined()) {
                ctx.throw_type_error("Cannot call method on null or undefined");
                return Value();
            }
            std::string str = this_value.to_string();
            std::string url = args.size() > 0 ? html_escape_attr(args[0].to_string()) : "";
            return Value(std::string("<a href=\"") + url + "\">" + str + "</a>");
        }, 1);
    PropertyDescriptor link_desc(Value(link_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("link", link_desc);

    auto small_fn = ObjectFactory::create_native_function("small",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            if (this_value.is_null() || this_value.is_undefined()) {
                ctx.throw_type_error("Cannot call method on null or undefined");
                return Value();
            }
            std::string str = this_value.to_string();
            return Value(std::string("<small>") + str + "</small>");
        }, 0);
    PropertyDescriptor small_desc(Value(small_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("small", small_desc);

    auto strike_fn = ObjectFactory::create_native_function("strike",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            if (this_value.is_null() || this_value.is_undefined()) {
                ctx.throw_type_error("Cannot call method on null or undefined");
                return Value();
            }
            std::string str = this_value.to_string();
            return Value(std::string("<strike>") + str + "</strike>");
        }, 0);
    PropertyDescriptor strike_desc(Value(strike_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("strike", strike_desc);

    auto sub_fn = ObjectFactory::create_native_function("sub",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            if (this_value.is_null() || this_value.is_undefined()) {
                ctx.throw_type_error("Cannot call method on null or undefined");
                return Value();
            }
            std::string str = this_value.to_string();
            return Value(std::string("<sub>") + str + "</sub>");
        }, 0);
    PropertyDescriptor sub_desc(Value(sub_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("sub", sub_desc);

    auto sup_fn = ObjectFactory::create_native_function("sup",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            if (this_value.is_null() || this_value.is_undefined()) {
                ctx.throw_type_error("Cannot call method on null or undefined");
                return Value();
            }
            std::string str = this_value.to_string();
            return Value(std::string("<sup>") + str + "</sup>");
        }, 0);
    PropertyDescriptor sup_desc(Value(sup_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("sup", sup_desc);

    // AnnexB: String.prototype.substr(start, length)
    auto substr_fn = ObjectFactory::create_native_function("substr",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();
            int64_t size = static_cast<int64_t>(str.length());

            double start_val = 0;
            if (!args.empty()) {
                start_val = args[0].to_number();
            }

            int64_t intStart;
            if (std::isnan(start_val)) {
                intStart = 0;
            } else if (std::isinf(start_val)) {
                if (start_val < 0) {
                    intStart = 0; 
                } else {
                    intStart = size; 
                }
            } else {
                intStart = static_cast<int64_t>(std::trunc(start_val));
            }

            if (intStart < 0) {
                intStart = std::max(static_cast<int64_t>(0), size + intStart);
            }
            intStart = std::min(intStart, size);

            int64_t intLength;
            if (args.size() > 1) {
                double length_val = args[1].to_number();
                // ToIntegerOrInfinity
                if (std::isnan(length_val)) {
                    intLength = 0;
                } else if (std::isinf(length_val)) {
                    if (length_val < 0) {
                        intLength = 0;
                    } else {
                        intLength = size;
                    }
                } else {
                    intLength = static_cast<int64_t>(std::trunc(length_val));
                }
            } else {
                intLength = size;
            }

            intLength = std::min(std::max(intLength, static_cast<int64_t>(0)), size);

            int64_t intEnd = std::min(intStart + intLength, size);

            if (intEnd <= intStart) {
                return Value(std::string(""));
            }

            return Value(str.substr(static_cast<size_t>(intStart), static_cast<size_t>(intEnd - intStart)));
        }, 2);
    PropertyDescriptor substr_desc(Value(substr_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("substr", substr_desc);

    auto isWellFormed_fn = ObjectFactory::create_native_function("isWellFormed",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            std::string str = "";
            try {
                Value this_value = ctx.get_binding("this");
                str = this_value.to_string();
            } catch (...) {
                return Value(true);
            }
            return Value(true);
        }, 0);
    PropertyDescriptor isWellFormed_desc(Value(isWellFormed_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("isWellFormed", isWellFormed_desc);

    auto toWellFormed_fn = ObjectFactory::create_native_function("toWellFormed",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            std::string str = "";
            try {
                Value this_value = ctx.get_binding("this");
                str = this_value.to_string();
            } catch (...) {
                return Value(std::string(""));
            }
            return Value(str);
        }, 0);
    PropertyDescriptor toWellFormed_desc(Value(toWellFormed_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("toWellFormed", toWellFormed_desc);

    auto repeat_fn = ObjectFactory::create_native_function("repeat",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string str = "";
            try {
                Value this_value = ctx.get_binding("this");
                str = this_value.to_string();
            } catch (...) {
                return Value(std::string(""));
            }

            if (args.empty()) return Value(std::string(""));

            int count = static_cast<int>(args[0].to_number());
            if (count < 0 || std::isinf(args[0].to_number())) {
                throw std::runtime_error("RangeError: Invalid count value");
            }

            if (count == 0) return Value(std::string(""));

            std::string result;
            result.reserve(str.length() * count);
            for (int i = 0; i < count; i++) {
                result += str;
            }
            return Value(result);
        }, 1);
    PropertyDescriptor repeat_desc(Value(repeat_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("repeat", repeat_desc);

    auto normalize_fn = ObjectFactory::create_native_function("normalize",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();
            std::string form = (args.size() > 0 && !args[0].is_undefined()) ? args[0].to_string() : "NFC";

            const utf8proc_uint8_t* input = reinterpret_cast<const utf8proc_uint8_t*>(str.c_str());
            utf8proc_uint8_t* output = nullptr;

            if (form == "NFC") {
                output = utf8proc_NFC(input);
            } else if (form == "NFD") {
                output = utf8proc_NFD(input);
            } else if (form == "NFKC") {
                output = utf8proc_NFKC(input);
            } else if (form == "NFKD") {
                output = utf8proc_NFKD(input);
            } else {
                throw std::runtime_error("RangeError: The normalization form should be one of NFC, NFD, NFKC, NFKD");
            }

            if (!output) {
                return Value(str);
            }
            std::string result(reinterpret_cast<const char*>(output));
            free(output);
            return Value(result);
        }, 0);
    PropertyDescriptor normalize_desc(Value(normalize_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("normalize", normalize_desc);

    // ES6: String.prototype[Symbol.iterator] - iterates by Unicode codepoints
    auto string_iterator_fn = ObjectFactory::create_native_function("[Symbol.iterator]",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_val = ctx.get_binding("this");
            std::string str = this_val.to_string();
            auto iterator = ObjectFactory::create_object();
            if (Iterator::s_string_iterator_prototype_) {
                iterator->set_prototype(Iterator::s_string_iterator_prototype_);
            }
            auto index = std::make_shared<size_t>(0);
            auto str_copy = std::make_shared<std::string>(str);
            auto next_fn = ObjectFactory::create_native_function("next",
                [str_copy, index](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx; (void)args;
                    auto result = ObjectFactory::create_object();
                    if (*index >= str_copy->length()) {
                        result->set_property("done", Value(true));
                        result->set_property("value", Value());
                    } else {
                        // UTF-8 codepoint-aware: read full multi-byte character
                        unsigned char ch = static_cast<unsigned char>((*str_copy)[*index]);
                        size_t char_len = 1;
                        if (ch >= 0xF0) char_len = 4;
                        else if (ch >= 0xE0) char_len = 3;
                        else if (ch >= 0xC0) char_len = 2;
                        if (*index + char_len > str_copy->length()) char_len = 1;
                        std::string codepoint = str_copy->substr(*index, char_len);
                        result->set_property("done", Value(false));
                        result->set_property("value", Value(codepoint));
                        *index += char_len;
                    }
                    return Value(result.release());
                }, 0);
            iterator->set_property("next", Value(next_fn.release()));
            return Value(iterator.release());
        }, 0);
    string_prototype->set_property("Symbol.iterator", Value(string_iterator_fn.release()), PropertyAttributes::BuiltinFunction);

    Object* proto_ptr = string_prototype.get();
    string_constructor->set_property("prototype", Value(string_prototype.release()), PropertyAttributes::None);
    proto_ptr->set_property("constructor", Value(string_constructor.get()));

    auto string_raw_fn = ObjectFactory::create_native_function("raw",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                ctx.throw_exception(Value(std::string("TypeError: String.raw requires at least 1 argument")));
                return Value();
            }

            if (args.size() > 0 && (args[0].is_object() || args[0].is_function())) {
                Object* template_obj = args[0].is_object() ? args[0].as_object()
                    : static_cast<Object*>(args[0].as_function());
                // Get raw array via get_property (fires get("raw") trap on Proxy)
                Value raw_val = template_obj->get_property("raw");
                if (raw_val.is_object()) {
                    Object* raw_obj = raw_val.as_object();
                    // Get length via get_property (fires get("length") trap on Proxy)
                    Value len_val = raw_obj->get_property("length");
                    uint32_t length = len_val.is_number() ? static_cast<uint32_t>(len_val.to_number()) : 0;
                    std::string result;
                    for (uint32_t i = 0; i < length; i++) {
                        if (i > 0 && i < static_cast<uint32_t>(args.size())) {
                            result += args[i].to_string();
                        }
                        // Get element via get_property (fires get("0"), get("1"), ... on Proxy)
                        Value chunk = raw_obj->get_property(std::to_string(i));
                        result += chunk.is_undefined() ? "" : chunk.to_string();
                    }
                    return Value(result);
                }
            }

            return Value(std::string(""));
        }, 1);

    string_constructor->set_property("raw", Value(string_raw_fn.release()), PropertyAttributes::BuiltinFunction);

    auto fromCharCode_fn = ObjectFactory::create_native_function("fromCharCode",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            std::string result;
            for (const auto& arg : args) {
                uint32_t code = static_cast<uint32_t>(arg.to_number()) & 0xFFFF;
                if (code <= 0x7F) {
                    result += static_cast<char>(code);
                } else if (code <= 0x7FF) {
                    result += static_cast<char>(0xC0 | (code >> 6));
                    result += static_cast<char>(0x80 | (code & 0x3F));
                } else {
                    result += static_cast<char>(0xE0 | (code >> 12));
                    result += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                    result += static_cast<char>(0x80 | (code & 0x3F));
                }
            }
            return Value(result);
        }, 1);
    string_constructor->set_property("fromCharCode", Value(fromCharCode_fn.release()), PropertyAttributes::BuiltinFunction);

    auto fromCodePoint_fn = ObjectFactory::create_native_function("fromCodePoint",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string result;
            for (const auto& arg : args) {
                double num = arg.to_number();
                if (num < 0 || num > 0x10FFFF || num != std::floor(num)) {
                    ctx.throw_exception(Value(std::string("RangeError: Invalid code point")));
                    return Value();
                }
                uint32_t code = static_cast<uint32_t>(num);
                if (code <= 0x7F) {
                    result += static_cast<char>(code);
                } else if (code <= 0x7FF) {
                    result += static_cast<char>(0xC0 | (code >> 6));
                    result += static_cast<char>(0x80 | (code & 0x3F));
                } else if (code <= 0xFFFF) {
                    result += static_cast<char>(0xE0 | (code >> 12));
                    result += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                    result += static_cast<char>(0x80 | (code & 0x3F));
                } else {
                    result += static_cast<char>(0xF0 | (code >> 18));
                    result += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
                    result += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                    result += static_cast<char>(0x80 | (code & 0x3F));
                }
            }
            return Value(result);
        }, 1);
    string_constructor->set_property("fromCodePoint", Value(fromCodePoint_fn.release()), PropertyAttributes::BuiltinFunction);

    ctx.register_built_in_object("String", string_constructor.release());
}

} // namespace Quanta
