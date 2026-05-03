/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "quanta/core/engine/builtins/RegExpBuiltin.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/RegExp.h"
#include "quanta/core/runtime/Symbol.h"
#include <sstream>
#include "quanta/parser/AST.h"
#include <algorithm>

namespace Quanta {

void register_regexp_builtins(Context& ctx) {
    auto regexp_prototype = ObjectFactory::create_object();

    auto compile_fn = ObjectFactory::create_native_function("compile",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_exception(Value(std::string("TypeError: RegExp.prototype.compile called on null or undefined")));
                return Value();
            }

            std::string pattern = "";
            std::string flags = "";

            if (args.size() > 0) {
                pattern = args[0].to_string();
            }
            if (args.size() > 1) {
                flags = args[1].to_string();
            }

            this_obj->set_property("source", Value(pattern));
            this_obj->set_property("global", Value(flags.find('g') != std::string::npos));
            this_obj->set_property("ignoreCase", Value(flags.find('i') != std::string::npos));
            this_obj->set_property("multiline", Value(flags.find('m') != std::string::npos));
            this_obj->set_property("lastIndex", Value(0.0));

            return Value(this_obj);
        }, 2);
    regexp_prototype->set_property("compile", Value(compile_fn.release()), PropertyAttributes::BuiltinFunction);

    Object* regexp_proto_ptr = regexp_prototype.get();

    auto regexp_constructor = ObjectFactory::create_native_constructor("RegExp",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // ES6: Check IsRegExp(pattern) via Symbol.match
            bool pattern_is_regexp = false;
            std::string pattern = "";
            std::string flags = args.size() > 1 ? args[1].to_string() : "";

            if (!args.empty() && (args[0].is_object() || args[0].is_function())) {
                Object* pat_obj = args[0].is_object() ? args[0].as_object() : args[0].as_function();
                // IsRegExp: check Symbol.match (fires get trap on Proxy)
                Value sym_match = pat_obj->get_property("Symbol.match");
                if (!sym_match.is_undefined()) {
                    pattern_is_regexp = sym_match.to_boolean();
                } else {
                    // Fallback: check internal _isRegExp flag
                    Value is_regexp = pat_obj->get_property("_isRegExp");
                    pattern_is_regexp = is_regexp.is_boolean() && is_regexp.to_boolean();
                }

                if (pattern_is_regexp) {
                    // Get constructor to check if it matches current RegExp
                    Value ctor = pat_obj->get_property("constructor");  // fires get("constructor") on Proxy
                    Value current_regexp = ctx.get_binding("RegExp");
                    bool ctor_matches = false;
                    if (ctor.is_function() && current_regexp.is_function()) {
                        ctor_matches = (ctor.as_function() == current_regexp.as_function());
                    }
                    if (ctor_matches && args.size() < 2) {
                        // Same constructor and no flags override: return pattern as-is
                        return args[0];
                    }
                    // Different constructor or flags provided: get source and flags
                    Value src = pat_obj->get_property("source");  // fires get("source") on Proxy
                    pattern = src.is_undefined() ? "" : src.to_string();
                    if (args.size() < 2) {
                        Value fl = pat_obj->get_property("flags");  // fires get("flags") on Proxy
                        flags = fl.is_undefined() ? "" : fl.to_string();
                    }
                } else {
                    pattern = pat_obj->get_property("_isRegExp").is_undefined() ? args[0].to_string() : "";
                    // For ordinary objects without IsRegExp, convert to string
                    pattern = args[0].to_string();
                }
            } else if (!args.empty()) {
                pattern = args[0].to_string();
            }

            try {
                auto regex_obj = ObjectFactory::create_object();

                auto regexp_impl = std::make_shared<RegExp>(pattern, flags);

                regex_obj->set_property("_isRegExp", Value(true));
                regex_obj->set_property("source", Value(regexp_impl->get_source()));
                // ES6: flags must be in alphabetical order
                std::string sorted_flags = regexp_impl->get_flags();
                std::sort(sorted_flags.begin(), sorted_flags.end());
                regex_obj->set_property("flags", Value(sorted_flags));
                regex_obj->set_property("global", Value(regexp_impl->get_global()));
                regex_obj->set_property("ignoreCase", Value(regexp_impl->get_ignore_case()));
                regex_obj->set_property("multiline", Value(regexp_impl->get_multiline()));
                regex_obj->set_property("unicode", Value(regexp_impl->get_unicode()));
                regex_obj->set_property("sticky", Value(regexp_impl->get_sticky()));
                regex_obj->set_property("dotAll", Value(regexp_impl->get_dotall()));
                regex_obj->set_property("lastIndex", Value(static_cast<double>(regexp_impl->get_last_index())));
                
                Object* regex_obj_ptr = regex_obj.get();

                auto test_fn = ObjectFactory::create_native_function("test",
                    [regexp_impl, regex_obj_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
                        if (args.empty()) return Value(false);
                        std::string str = args[0].to_string();

                        if (regexp_impl->get_global()) {
                            Value lastIndex_val = regex_obj_ptr->get_property("lastIndex");
                            if (lastIndex_val.is_number()) {
                                regexp_impl->set_last_index(static_cast<int>(lastIndex_val.to_number()));
                            }
                        }

                        bool result = regexp_impl->test(str);

                        if (regexp_impl->get_global()) {
                            regex_obj_ptr->set_property("lastIndex", Value(static_cast<double>(regexp_impl->get_last_index())));
                        }

                        return Value(result);
                    });
                regex_obj->set_property("test", Value(test_fn.release()), PropertyAttributes::BuiltinFunction);

                auto exec_fn = ObjectFactory::create_native_function("exec",
                    [regexp_impl, regex_obj_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
                        if (args.empty()) return Value::null();
                        std::string str = args[0].to_string();

                        Value lastIndex_val = regex_obj_ptr->get_property("lastIndex");
                        if (lastIndex_val.is_number()) {
                            regexp_impl->set_last_index(static_cast<int>(lastIndex_val.to_number()));
                        }

                        Value result = regexp_impl->exec(str);

                        regex_obj_ptr->set_property("lastIndex", Value(static_cast<double>(regexp_impl->get_last_index())));

                        return result;
                    });
                regex_obj->set_property("exec", Value(exec_fn.release()), PropertyAttributes::BuiltinFunction);

                auto toString_fn = ObjectFactory::create_native_function("toString",
                    [regexp_impl](Context& ctx, const std::vector<Value>& args) -> Value {
                        (void)ctx;
                        (void)args;
                        return Value(regexp_impl->to_string());
                    });
                regex_obj->set_property("toString", Value(toString_fn.release()), PropertyAttributes::BuiltinFunction);

                auto compile_inst_fn = ObjectFactory::create_native_function("compile",
                    [regexp_impl, regex_obj_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
                        std::string pattern = "";
                        std::string flags = "";
                        if (args.size() > 0) pattern = args[0].to_string();
                        if (args.size() > 1) flags = args[1].to_string();

                        regexp_impl->compile(pattern, flags);

                        regex_obj_ptr->set_property("source", Value(regexp_impl->get_source()));
                        std::string sf = regexp_impl->get_flags();
                        std::sort(sf.begin(), sf.end());
                        regex_obj_ptr->set_property("flags", Value(sf));
                        regex_obj_ptr->set_property("global", Value(regexp_impl->get_global()));
                        regex_obj_ptr->set_property("ignoreCase", Value(regexp_impl->get_ignore_case()));
                        regex_obj_ptr->set_property("multiline", Value(regexp_impl->get_multiline()));
                        regex_obj_ptr->set_property("lastIndex", Value(0.0));

                        return Value(regex_obj_ptr);
                    }, 2);
                regex_obj->set_property("compile", Value(compile_inst_fn.release()), PropertyAttributes::BuiltinFunction);

                regex_obj->set_property("source", Value(regexp_impl->get_source()));
                regex_obj->set_property("flags", Value(sorted_flags));
                regex_obj->set_property("global", Value(regexp_impl->get_global()));
                regex_obj->set_property("ignoreCase", Value(regexp_impl->get_ignore_case()));
                regex_obj->set_property("multiline", Value(regexp_impl->get_multiline()));
                regex_obj->set_property("lastIndex", Value(static_cast<double>(regexp_impl->get_last_index())));

                Object* regex_raw = regex_obj.release();
                Value new_target = ctx.get_new_target();
                if (!new_target.is_undefined()) {
                    Object* nt_obj = new_target.is_function()
                        ? static_cast<Object*>(new_target.as_function())
                        : new_target.is_object() ? new_target.as_object() : nullptr;
                    if (nt_obj) {
                        Value nt_proto = nt_obj->get_property("prototype");
                        if (nt_proto.is_object()) regex_raw->set_prototype(nt_proto.as_object());
                    }
                } else {
                    Value regexp_ctor = ctx.get_binding("RegExp");
                    if (regexp_ctor.is_function()) {
                        Value proto = regexp_ctor.as_function()->get_property("prototype");
                        if (proto.is_object()) regex_raw->set_prototype(proto.as_object());
                    }
                }
                return Value(regex_raw);

            } catch (const std::exception& e) {
                ctx.throw_error("Invalid RegExp: " + std::string(e.what()));
                return Value::null();
            }
        });

    // ES6: RegExp.prototype.toString is generic - works on any object with source/flags
    auto regexp_toString = ObjectFactory::create_native_function("toString",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_type_error("RegExp.prototype.toString called on incompatible receiver");
                return Value();
            }
            Value source_val = this_obj->get_property("source");
            Value flags_val = this_obj->get_property("flags");
            std::string source = source_val.is_undefined() ? "(?:)" : source_val.to_string();
            std::string flags = flags_val.is_undefined() ? "" : flags_val.to_string();
            return Value("/" + source + "/" + flags);
        }, 0);
    regexp_prototype->set_property("toString", Value(regexp_toString.release()), PropertyAttributes::BuiltinFunction);

    PropertyDescriptor regexp_constructor_desc(Value(regexp_constructor.get()),
        PropertyAttributes::BuiltinFunction);
    regexp_prototype->set_property_descriptor("constructor", regexp_constructor_desc);

    // ES2022: RegExp.prototype flag data properties (false by default, shadowed by instance props)
    regexp_prototype->set_property("hasIndices", Value(false));
    regexp_prototype->set_property("global", Value(false));
    regexp_prototype->set_property("ignoreCase", Value(false));
    regexp_prototype->set_property("multiline", Value(false));
    regexp_prototype->set_property("dotAll", Value(false));
    regexp_prototype->set_property("unicode", Value(false));
    regexp_prototype->set_property("sticky", Value(false));

    // ES2022: RegExp.prototype.flags accessor (reads flag props via get_property for Proxy support)
    {
        auto flags_getter_fn = ObjectFactory::create_native_function("get flags",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)args;
                Object* this_obj = ctx.get_this_binding();
                if (!this_obj) {
                    ctx.throw_type_error("RegExp.prototype.flags getter called on incompatible receiver");
                    return Value();
                }
                // Check flags in ES2022 canonical order: d, g, i, m, s, u, y
                std::string result;
                if (this_obj->get_property("hasIndices").to_boolean()) result += "d";
                if (this_obj->get_property("global").to_boolean()) result += "g";
                if (this_obj->get_property("ignoreCase").to_boolean()) result += "i";
                if (this_obj->get_property("multiline").to_boolean()) result += "m";
                if (this_obj->get_property("dotAll").to_boolean()) result += "s";
                if (this_obj->get_property("unicode").to_boolean()) result += "u";
                if (this_obj->get_property("sticky").to_boolean()) result += "y";
                return Value(result);
            });
        PropertyDescriptor flags_desc;
        flags_desc.set_getter(flags_getter_fn.release());
        flags_desc.set_enumerable(false);
        flags_desc.set_configurable(true);
        regexp_prototype->set_property_descriptor("flags", flags_desc);
    }

    // RegExp.prototype.exec - generic, delegates to own exec on instance
    auto regexp_exec_proto_fn = ObjectFactory::create_native_function("exec",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->get_own_property("_isRegExp").to_boolean()) {
                ctx.throw_type_error("RegExp.prototype.exec called on incompatible receiver");
                return Value();
            }
            Value own_exec = this_obj->get_own_property("exec");
            if (own_exec.is_function()) {
                std::string str = args.empty() ? "undefined" : args[0].to_string();
                return own_exec.as_function()->call(ctx, {Value(str)}, Value(this_obj));
            }
            ctx.throw_type_error("RegExp.prototype.exec called on incompatible receiver");
            return Value();
        }, 1);
    regexp_prototype->set_property("exec", Value(regexp_exec_proto_fn.release()), PropertyAttributes::BuiltinFunction);

    // ES6: RegExp.prototype.test - generic function that calls this.exec
    auto regexp_test_fn = ObjectFactory::create_native_function("test",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_type_error("RegExp.prototype.test called on incompatible receiver");
                return Value();
            }
            std::string str = args.empty() ? "" : args[0].to_string();
            // Call this.exec via get_property (fires get("exec") trap on Proxy)
            Value exec_fn = this_obj->get_property("exec");
            if (exec_fn.is_function()) {
                Value result = exec_fn.as_function()->call(ctx, {Value(str)}, Value(this_obj));
                if (ctx.has_exception()) return Value();
                return Value(!result.is_null() && !result.is_undefined());
            }
            return Value(false);
        }, 1);
    regexp_prototype->set_property("test", Value(regexp_test_fn.release()), PropertyAttributes::BuiltinFunction);

    // ES6: RegExp.prototype[Symbol.match/replace/search/split]
    auto regexp_sym_match = ObjectFactory::create_native_function("[Symbol.match]",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value();
            std::string str = args.empty() ? "" : args[0].to_string();
            // Check global flag via get_property (fires get("global") trap on Proxy)
            Value global_val = this_obj->get_property("global");
            bool is_global = global_val.to_boolean();
            if (!is_global) {
                // Non-global: call exec once
                Value exec_fn = this_obj->get_property("exec");
                if (exec_fn.is_function()) {
                    return exec_fn.as_function()->call(ctx, {Value(str)}, Value(this_obj));
                }
                return Value::null();
            }
            // Global: also check unicode, then loop exec
            Value unicode_val = this_obj->get_property("unicode");
            (void)unicode_val;
            this_obj->set_property("lastIndex", Value(0.0));
            auto result_array = ObjectFactory::create_array();
            size_t match_count = 0;
            Value exec_fn = this_obj->get_property("exec");
            if (!exec_fn.is_function()) return Value::null();
            Function* exec_func = exec_fn.as_function();
            size_t safety = 0;
            const size_t max_iter = str.length() + 2;
            while (safety++ < max_iter) {
                Value match = exec_func->call(ctx, {Value(str)}, Value(this_obj));
                if (ctx.has_exception()) return Value();
                if (match.is_null()) break;
                if (match.is_object()) {
                    Value matched = match.as_object()->get_element(0);
                    result_array->set_element(match_count++, matched);
                }
            }
            if (match_count == 0) return Value::null();
            result_array->set_length(static_cast<uint32_t>(match_count));
            return Value(result_array.release());
        }, 1);
    regexp_prototype->set_property("Symbol.match", Value(regexp_sym_match.release()), PropertyAttributes::BuiltinFunction);

    auto regexp_sym_replace = ObjectFactory::create_native_function("[Symbol.replace]",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value();
            std::string str = args.size() > 0 ? args[0].to_string() : "";
            Value replace_val = args.size() > 1 ? args[1] : Value();
            // Check global flag via get_property (fires get("global") trap on Proxy)
            Value global_val = this_obj->get_property("global");
            bool is_global = global_val.to_boolean();
            if (!is_global) {
                // Non-global: call exec once
                Value exec_fn = this_obj->get_property("exec");
                if (exec_fn.is_function()) {
                    Value match = exec_fn.as_function()->call(ctx, {Value(str)}, Value(this_obj));
                    if (match.is_null() || match.is_undefined()) return Value(str);
                    if (match.is_object()) {
                        Value matched = match.as_object()->get_property("0");
                        Value index_val = match.as_object()->get_property("index");
                        int index = static_cast<int>(index_val.to_number());
                        std::string matched_str = matched.to_string();
                        std::string replacement = replace_val.is_function() ? "" : replace_val.to_string();
                        if (replace_val.is_function()) {
                            Value r = replace_val.as_function()->call(ctx, {matched, Value(static_cast<double>(index)), Value(str)}, Value());
                            replacement = r.to_string();
                        }
                        return Value(str.substr(0, index) + replacement + str.substr(index + matched_str.length()));
                    }
                }
                return Value(str);
            }
            // Global: also check unicode, then loop exec
            Value unicode_val = this_obj->get_property("unicode");
            (void)unicode_val;
            this_obj->set_property("lastIndex", Value(0.0));
            Value exec_fn = this_obj->get_property("exec");
            if (!exec_fn.is_function()) return Value(str);
            Function* exec_func = exec_fn.as_function();
            std::string result = str;
            size_t safety = 0;
            const size_t max_iter = str.length() + 2;
            while (safety++ < max_iter) {
                Value match = exec_func->call(ctx, {Value(str)}, Value(this_obj));
                if (ctx.has_exception()) return Value();
                if (match.is_null()) break;
            }
            return Value(result);
        }, 2);
    regexp_prototype->set_property("Symbol.replace", Value(regexp_sym_replace.release()), PropertyAttributes::BuiltinFunction);

    auto regexp_sym_search = ObjectFactory::create_native_function("[Symbol.search]",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value(-1.0);
            std::string str = args.empty() ? "" : args[0].to_string();
            // Save previousLastIndex via get_property (fires get("lastIndex") trap on Proxy)
            Value prev_last_index = this_obj->get_property("lastIndex");
            // Set lastIndex to 0
            this_obj->set_property("lastIndex", Value(0.0));
            // Call exec via get_property (fires get("exec") trap on Proxy)
            Value exec_fn = this_obj->get_property("exec");
            Value result_val;
            if (exec_fn.is_function()) {
                result_val = exec_fn.as_function()->call(ctx, {Value(str)}, Value(this_obj));
                if (ctx.has_exception()) return Value();
            }
            // Read current lastIndex (fires get("lastIndex") again)
            Value cur_last_index = this_obj->get_property("lastIndex");
            // Restore previousLastIndex if changed
            if (cur_last_index.to_string() != prev_last_index.to_string()) {
                this_obj->set_property("lastIndex", prev_last_index);
            }
            if (result_val.is_null() || result_val.is_undefined()) return Value(-1.0);
            if (result_val.is_object()) {
                return result_val.as_object()->get_property("index");
            }
            return Value(-1.0);
        }, 1);
    regexp_prototype->set_property("Symbol.search", Value(regexp_sym_search.release()), PropertyAttributes::BuiltinFunction);

    auto regexp_sym_split = ObjectFactory::create_native_function("[Symbol.split]",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value();
            std::string str = args.size() > 0 ? args[0].to_string() : "";
            // ES6: SpeciesConstructor - check constructor[Symbol.species]
            Value ctor_val = this_obj->get_property("constructor");
            if (ctor_val.is_object() || ctor_val.is_function()) {
                Object* ctor_obj = ctor_val.is_function() ? static_cast<Object*>(ctor_val.as_function()) : ctor_val.as_object();
                Symbol* species_sym = Symbol::get_well_known(Symbol::SPECIES);
                if (species_sym) {
                    Value species_val = ctor_obj->get_property(species_sym->to_property_key());
                    if (species_val.is_function()) {
                        Value flags_val = this_obj->get_property("flags");
                        std::vector<Value> species_args = { Value(this_obj), flags_val };
                        Value splitter = species_val.as_function()->call(ctx, species_args, Value(ctor_obj));
                        if (ctx.has_exception()) return Value();
                        if (splitter.is_object() || splitter.is_function()) {
                            this_obj = splitter.is_function() ? static_cast<Object*>(splitter.as_function()) : splitter.as_object();
                        }
                    }
                }
            }
            // Get exec and use it
            auto result = ObjectFactory::create_array();
            Value exec_fn = this_obj->get_property("exec");
            if (!exec_fn.is_function()) return Value(result.release());
            uint32_t idx = 0;
            std::string remaining = str;
            while (!remaining.empty()) {
                Value match = exec_fn.as_function()->call(ctx, {Value(remaining)}, Value(this_obj));
                if (match.is_null() || match.is_undefined()) break;
                if (!match.is_object()) break;
                Value index_val = match.as_object()->get_property("index");
                Value matched_val = match.as_object()->get_property("0");
                int index = static_cast<int>(index_val.to_number());
                std::string matched_str = matched_val.to_string();
                if (matched_str.empty() && index == 0) {
                    result->set_element(idx++, Value(std::string(1, remaining[0])));
                    remaining = remaining.substr(1);
                } else {
                    result->set_element(idx++, Value(remaining.substr(0, index)));
                    remaining = remaining.substr(index + matched_str.length());
                }
            }
            if (!remaining.empty() || idx > 0) {
                result->set_element(idx++, Value(remaining));
            }
            result->set_property("length", Value(static_cast<double>(idx)));
            return Value(result.release());
        }, 2);
    regexp_prototype->set_property("Symbol.split", Value(regexp_sym_split.release()), PropertyAttributes::BuiltinFunction);

    regexp_constructor->set_property("prototype", Value(regexp_prototype.release()));

    {
        Symbol* species_sym = Symbol::get_well_known(Symbol::SPECIES);
        if (species_sym) {
            auto regexp_species_getter = ObjectFactory::create_native_function("get [Symbol.species]",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)args;
                    Object* self = ctx.get_this_binding();
                    return self ? Value(self) : Value();
                });
            PropertyDescriptor regexp_species_desc;
            regexp_species_desc.set_getter(regexp_species_getter.release());
            regexp_species_desc.set_enumerable(false);
            regexp_species_desc.set_configurable(true);
            regexp_constructor->set_property_descriptor(species_sym->to_property_key(), regexp_species_desc);
        }
    }

    ctx.register_built_in_object("RegExp", regexp_constructor.release());
}

} // namespace Quanta
