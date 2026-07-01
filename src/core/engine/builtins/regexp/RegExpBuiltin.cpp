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
#include <limits>

namespace Quanta {

static std::string regexp_get_substitution(const std::string& replacement, const std::string& str,
        size_t match_pos, const std::string& matched, const std::vector<std::string>& captures,
        Object* named_groups = nullptr) {
    std::string result;
    for (size_t i = 0; i < replacement.size(); i++) {
        if (replacement[i] != '$' || i + 1 >= replacement.size()) { result += replacement[i]; continue; }
        char next = replacement[i + 1];
        if (next == '$') { result += '$'; i++; }
        else if (next == '&') { result += matched; i++; }
        else if (next == '`') { result += str.substr(0, match_pos); i++; }
        else if (next == '\'') { result += str.substr(match_pos + matched.size()); i++; }
        else if (next == '<' && named_groups) {
            size_t close = replacement.find('>', i + 2);
            if (close != std::string::npos) {
                std::string name = replacement.substr(i + 2, close - i - 2);
                Value val = named_groups->get_property(name);
                if (!val.is_undefined()) result += val.to_string();
                i = close;
            } else { result += replacement[i]; }
        }
        else if (next >= '0' && next <= '9') {
            size_t n = next - '0';
            if (i + 2 < replacement.size() && replacement[i+2] >= '0' && replacement[i+2] <= '9') {
                size_t n2 = n * 10 + (replacement[i+2] - '0');
                if (n2 >= 1 && n2 <= captures.size()) { result += captures[n2-1]; i += 2; continue; }
            }
            if (n >= 1 && n <= captures.size()) { result += captures[n-1]; i++; }
            else result += replacement[i];
        } else { result += replacement[i]; }
    }
    return result;
}

void register_regexp_builtins(Context& ctx) {
    auto regexp_prototype = ObjectFactory::create_object();

    auto compile_fn = ObjectFactory::create_native_function("compile",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_type_error("RegExp.prototype.compile called on null or undefined");
                return Value();
            }

            std::string pattern = "";
            std::string flags = "";

            if (args.size() > 0) {
                pattern = args[0].to_string();
            }
            if (args.size() > 1 && !args[1].is_undefined() && !args[1].is_null()) {
                flags = args[1].to_string();
            }

            this_obj->set_property_descriptor("[[source]]",     PropertyDescriptor(Value(pattern), PropertyAttributes::None));
            this_obj->set_property_descriptor("[[global]]",     PropertyDescriptor(Value(flags.find('g') != std::string::npos), PropertyAttributes::None));
            this_obj->set_property_descriptor("[[ignoreCase]]", PropertyDescriptor(Value(flags.find('i') != std::string::npos), PropertyAttributes::None));
            this_obj->set_property_descriptor("[[multiline]]",  PropertyDescriptor(Value(flags.find('m') != std::string::npos), PropertyAttributes::None));
            this_obj->set_property_descriptor("[[unicode]]",    PropertyDescriptor(Value(flags.find('u') != std::string::npos), PropertyAttributes::None));
            this_obj->set_property_descriptor("[[sticky]]",     PropertyDescriptor(Value(flags.find('y') != std::string::npos), PropertyAttributes::None));
            this_obj->set_property_descriptor("[[dotAll]]",     PropertyDescriptor(Value(flags.find('s') != std::string::npos), PropertyAttributes::None));
            this_obj->set_property_descriptor("[[hasIndices]]", PropertyDescriptor(Value(flags.find('d') != std::string::npos), PropertyAttributes::None));
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
            std::string flags = (args.size() > 1 && !args[1].is_undefined() && !args[1].is_null()) ? args[1].to_string() : "";

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
                    // Non-IsRegExp object: ToString via JS-level toString/valueOf (not C++ to_string).
                    Value ts = pat_obj->get_property("toString");
                    if (!ctx.has_exception() && ts.is_function()) {
                        Value r = ts.as_function()->call(ctx, {}, args[0]);
                        if (!ctx.has_exception() && !r.is_object() && !r.is_function()) {
                            pattern = r.to_string();
                        } else if (!ctx.has_exception()) {
                            Value vof = pat_obj->get_property("valueOf");
                            if (!ctx.has_exception() && vof.is_function()) {
                                Value r2 = vof.as_function()->call(ctx, {}, args[0]);
                                if (!ctx.has_exception() && !r2.is_object() && !r2.is_function()) pattern = r2.to_string();
                            }
                        }
                    }
                    if (ctx.has_exception()) return Value();
                }
            } else if (!args.empty() && !args[0].is_undefined()) {
                pattern = args[0].to_string();
            }

            if (flags.find('u') != std::string::npos && flags.find('v') != std::string::npos) {
                ctx.throw_syntax_error("Regex flags 'u' and 'v' are mutually exclusive");
                return Value();
            }

            try {
                // ObjectType::RegExp so toString's internal-slot tag and String.prototype's
                // is_native_regexp checks (match/replace/split) see this the same as regex literals.
                auto regex_obj = std::make_unique<Object>(Object::ObjectType::RegExp);

                auto regexp_impl = std::make_shared<RegExp>(pattern, flags);

                regex_obj->set_property("_isRegExp", Value(true), PropertyAttributes::Writable);
                // Internal flag slots: stored under [[name]] keys so they don't shadow the prototype accessor getters (the correct public interface per ES2015+).
                regex_obj->set_property_descriptor("[[source]]",     PropertyDescriptor(Value(regexp_impl->get_source()), PropertyAttributes::None));
                regex_obj->set_property_descriptor("[[global]]",     PropertyDescriptor(Value(regexp_impl->get_global()), PropertyAttributes::None));
                regex_obj->set_property_descriptor("[[ignoreCase]]", PropertyDescriptor(Value(regexp_impl->get_ignore_case()), PropertyAttributes::None));
                regex_obj->set_property_descriptor("[[multiline]]",  PropertyDescriptor(Value(regexp_impl->get_multiline()), PropertyAttributes::None));
                regex_obj->set_property_descriptor("[[unicode]]",    PropertyDescriptor(Value(regexp_impl->get_unicode() && !regexp_impl->get_unicode_sets()), PropertyAttributes::None));
                regex_obj->set_property_descriptor("[[sticky]]",     PropertyDescriptor(Value(regexp_impl->get_sticky()), PropertyAttributes::None));
                regex_obj->set_property_descriptor("[[dotAll]]",     PropertyDescriptor(Value(regexp_impl->get_dotall()), PropertyAttributes::None));
                regex_obj->set_property_descriptor("[[hasIndices]]", PropertyDescriptor(Value(regexp_impl->get_flags().find('d') != std::string::npos), PropertyAttributes::None));
                {
                    PropertyDescriptor us_desc(Value(regexp_impl->get_unicode_sets()), PropertyAttributes::BuiltinFunction);
                    regex_obj->set_property_descriptor("unicodeSets", us_desc);
                }
                regex_obj->set_property("lastIndex", Value(static_cast<double>(regexp_impl->get_last_index())), PropertyAttributes::Writable);
                
                Object* regex_obj_ptr = regex_obj.get();

                auto test_fn = ObjectFactory::create_native_function("test",
                    [regexp_impl, regex_obj_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
                        Value arg0 = args.empty() ? Value() : args[0];
                        std::string str;
                        if (arg0.is_symbol()) { ctx.throw_type_error("Cannot convert Symbol to string"); return Value(); }
                        else if (arg0.is_object() || arg0.is_function()) { str = arg0.to_property_key(); if (ctx.has_exception()) return Value(); }
                        else { str = arg0.to_string(); }

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
                        // ToString(string): spec treats no-arg as exec("undefined"), not early null.
                        Value arg0 = args.empty() ? Value() : args[0];
                        std::string str;
                        if (arg0.is_symbol()) {
                            ctx.throw_type_error("Cannot convert Symbol to string");
                            return Value();
                        } else if (arg0.is_object() || arg0.is_function()) {
                            // ToPrimitive(arg, "string"): go through JS-level toString/valueOf.
                            str = arg0.to_property_key();
                            if (ctx.has_exception()) return Value();
                        } else {
                            str = arg0.to_string();
                        }

                        Value lastIndex_val = regex_obj_ptr->get_property("lastIndex");
                        double li = lastIndex_val.to_number();
                        if (ctx.has_exception()) return Value();
                        if (std::isnan(li) || li < 0) li = 0;
                        regexp_impl->set_last_index(li > static_cast<double>(std::numeric_limits<int>::max()) ? std::numeric_limits<int>::max() : static_cast<int>(li));

                        Value result = regexp_impl->exec(str);

                        int new_last = regexp_impl->get_last_index();
                        // Global/sticky: advance past zero-length matches to avoid infinite loops.
                        if ((regexp_impl->get_global() || regexp_impl->get_sticky()) &&
                            !result.is_null() && result.is_object()) {
                            Value matched = result.as_object()->get_element(0);
                            if (!matched.is_undefined() && matched.to_string().empty()) {
                                new_last = static_cast<int>(li) + 1;
                            }
                        }
                        regex_obj_ptr->set_property("lastIndex", Value(static_cast<double>(new_last)));

                        return result;
                    });
                regex_obj->set_property("exec", Value(exec_fn.release()), PropertyAttributes::BuiltinFunction);

                auto toString_fn = ObjectFactory::create_native_function("toString",
                    [regexp_impl, regex_obj_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
                        (void)args;
                        Value flags_v = regex_obj_ptr->get_property("flags");
                        std::string flags_str = flags_v.is_string() ? flags_v.to_string() : regexp_impl->get_flags();
                        return Value("/" + regexp_impl->get_source() + "/" + flags_str);
                    });
                regex_obj->set_property("toString", Value(toString_fn.release()), PropertyAttributes::BuiltinFunction);

                auto compile_inst_fn = ObjectFactory::create_native_function("compile",
                    [regexp_impl, regex_obj_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
                        std::string pattern = "";
                        std::string flags = "";
                        if (args.size() > 0) pattern = args[0].to_string();
                        if (args.size() > 1) flags = args[1].to_string();

                        regexp_impl->compile(pattern, flags);

                        regex_obj_ptr->set_property_descriptor("[[source]]",     PropertyDescriptor(Value(regexp_impl->get_source()), PropertyAttributes::None));
                        regex_obj_ptr->set_property_descriptor("[[global]]",     PropertyDescriptor(Value(regexp_impl->get_global()), PropertyAttributes::None));
                        regex_obj_ptr->set_property_descriptor("[[ignoreCase]]", PropertyDescriptor(Value(regexp_impl->get_ignore_case()), PropertyAttributes::None));
                        regex_obj_ptr->set_property_descriptor("[[multiline]]",  PropertyDescriptor(Value(regexp_impl->get_multiline()), PropertyAttributes::None));
                        regex_obj_ptr->set_property_descriptor("[[unicode]]",    PropertyDescriptor(Value(regexp_impl->get_unicode() && !regexp_impl->get_unicode_sets()), PropertyAttributes::None));
                        regex_obj_ptr->set_property_descriptor("[[sticky]]",     PropertyDescriptor(Value(regexp_impl->get_sticky()), PropertyAttributes::None));
                        regex_obj_ptr->set_property_descriptor("[[dotAll]]",     PropertyDescriptor(Value(regexp_impl->get_dotall()), PropertyAttributes::None));
                        regex_obj_ptr->set_property_descriptor("[[hasIndices]]", PropertyDescriptor(Value(regexp_impl->get_flags().find('d') != std::string::npos), PropertyAttributes::None));
                        regex_obj_ptr->set_property("lastIndex", Value(0.0));

                        return Value(regex_obj_ptr);
                    }, 2);
                regex_obj->set_property("compile", Value(compile_inst_fn.release()), PropertyAttributes::BuiltinFunction);

                regex_obj->set_property_descriptor("[[source]]",     PropertyDescriptor(Value(regexp_impl->get_source()), PropertyAttributes::None));
                regex_obj->set_property_descriptor("[[global]]",     PropertyDescriptor(Value(regexp_impl->get_global()), PropertyAttributes::None));
                regex_obj->set_property_descriptor("[[ignoreCase]]", PropertyDescriptor(Value(regexp_impl->get_ignore_case()), PropertyAttributes::None));
                regex_obj->set_property_descriptor("[[multiline]]",  PropertyDescriptor(Value(regexp_impl->get_multiline()), PropertyAttributes::None));
                regex_obj->set_property_descriptor("[[unicode]]",    PropertyDescriptor(Value(regexp_impl->get_unicode() && !regexp_impl->get_unicode_sets()), PropertyAttributes::None));
                regex_obj->set_property_descriptor("[[sticky]]",     PropertyDescriptor(Value(regexp_impl->get_sticky()), PropertyAttributes::None));
                regex_obj->set_property_descriptor("[[dotAll]]",     PropertyDescriptor(Value(regexp_impl->get_dotall()), PropertyAttributes::None));
                regex_obj->set_property_descriptor("[[hasIndices]]", PropertyDescriptor(Value(regexp_impl->get_flags().find('d') != std::string::npos), PropertyAttributes::None));
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
                ctx.throw_syntax_error(std::string(e.what()));
                return Value();
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

    // ES2015+: flag properties are ACCESSOR getters on RegExp.prototype (not own data on instances). Each getter throws TypeError for non-RegExp this (including RegExp.prototype itself), else reads flag from [[name]] internal slot on the instance.
    {
        auto make_flag_getter = [regexp_proto_ptr](const char* getter_name, const char* flag_key) {
            return ObjectFactory::create_native_function(getter_name,
                [regexp_proto_ptr, flag_key](Context& ctx, const std::vector<Value>&) -> Value {
                    Object* self = ctx.get_this_binding();
                    if (!self) { ctx.throw_type_error("RegExp flag getter requires a RegExp"); return Value(); }
                    if (self == regexp_proto_ptr) return Value(); // spec: return undefined for RegExp.prototype
                    Value is_re = self->get_own_property("_isRegExp");
                    if (!is_re.is_boolean() || !is_re.to_boolean()) { ctx.throw_type_error("RegExp flag getter requires a RegExp"); return Value(); }
                    return Value(self->get_own_property(flag_key).to_boolean());
                }, 0);
        };
        auto make_flag_desc = [](std::unique_ptr<Function> getter_fn) {
            PropertyDescriptor d;
            d.set_getter(getter_fn.release());
            d.set_enumerable(false);
            d.set_configurable(true);
            return d;
        };
        regexp_prototype->set_property_descriptor("hasIndices", make_flag_desc(make_flag_getter("get hasIndices", "[[hasIndices]]")));
        regexp_prototype->set_property_descriptor("global",     make_flag_desc(make_flag_getter("get global",     "[[global]]")));
        regexp_prototype->set_property_descriptor("ignoreCase", make_flag_desc(make_flag_getter("get ignoreCase", "[[ignoreCase]]")));
        regexp_prototype->set_property_descriptor("multiline",  make_flag_desc(make_flag_getter("get multiline",  "[[multiline]]")));
        regexp_prototype->set_property_descriptor("dotAll",     make_flag_desc(make_flag_getter("get dotAll",     "[[dotAll]]")));
        regexp_prototype->set_property_descriptor("unicode",    make_flag_desc(make_flag_getter("get unicode",    "[[unicode]]")));
        regexp_prototype->set_property_descriptor("sticky",     make_flag_desc(make_flag_getter("get sticky",     "[[sticky]]")));
        // source accessor: empty pattern renders as "(?:)" and slashes in pattern are escaped.
        regexp_prototype->set_property_descriptor("source", make_flag_desc(
            ObjectFactory::create_native_function("get source",
                [regexp_proto_ptr](Context& ctx, const std::vector<Value>&) -> Value {
                    Object* self = ctx.get_this_binding();
                    if (!self) { ctx.throw_type_error("get source requires a RegExp"); return Value(); }
                    if (self == regexp_proto_ptr) return Value(std::string("(?:)")); // spec: return "(?:)" for RegExp.prototype
                    Value is_re = self->get_own_property("_isRegExp");
                    if (!is_re.is_boolean() || !is_re.to_boolean()) { ctx.throw_type_error("get source requires a RegExp"); return Value(); }
                    Value src = self->get_own_property("[[source]]");
                    std::string s = src.to_string();
                    if (s.empty()) return Value(std::string("(?:)"));
                    // Ensure no unescaped slashes (pcre2 source already has them unescaped)
                    std::string escaped;
                    for (size_t i = 0; i < s.size(); i++) {
                        if (s[i] == '/' && (i == 0 || s[i-1] != '\\')) escaped += '\\';
                        escaped += s[i];
                    }
                    return Value(escaped);
                }, 0)));
    }

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
                // Check flags in ES2022 canonical order: d, g, i, m, s, u, v, y. Each flag is an accessor on the prototype that throws TypeError for non-regex `this` -- propagate that if any getter throws.
                std::string result;
                auto add_flag = [&](const char* prop, char ch) -> bool {
                    Value v = this_obj->get_property(prop);
                    if (ctx.has_exception()) return false;
                    if (v.to_boolean()) result += ch;
                    return !ctx.has_exception();
                };
                if (!add_flag("hasIndices", 'd')) return Value();
                if (!add_flag("global",     'g')) return Value();
                if (!add_flag("ignoreCase", 'i')) return Value();
                if (!add_flag("multiline",  'm')) return Value();
                if (!add_flag("dotAll",     's')) return Value();
                if (!add_flag("unicode",    'u')) return Value();
                if (!add_flag("unicodeSets",'v')) return Value();
                if (!add_flag("sticky",     'y')) return Value();
                return Value(result);
            });
        PropertyDescriptor flags_desc;
        flags_desc.set_getter(flags_getter_fn.release());
        flags_desc.set_enumerable(false);
        flags_desc.set_configurable(true);
        regexp_prototype->set_property_descriptor("flags", flags_desc);
    }

    // ES2024: RegExp.prototype.unicodeSets accessor (regexp-v-flag)
    {
        auto unicode_sets_getter_fn = ObjectFactory::create_native_function("get unicodeSets",
            [regexp_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)args;
                Object* this_obj = ctx.get_this_binding();
                if (!this_obj) {
                    ctx.throw_type_error("RegExp.prototype.unicodeSets getter called on incompatible receiver");
                    return Value();
                }
                if (this_obj == regexp_proto_ptr) return Value();
                Value is_regexp = this_obj->get_property("_isRegExp");
                if (!(is_regexp.is_boolean() && is_regexp.to_boolean())) {
                    ctx.throw_type_error("RegExp.prototype.unicodeSets getter called on incompatible receiver");
                    return Value();
                }
                return Value(this_obj->get_property("unicodeSets").to_boolean());
            });
        PropertyDescriptor unicode_sets_desc;
        unicode_sets_desc.set_getter(unicode_sets_getter_fn.release());
        unicode_sets_desc.set_enumerable(false);
        unicode_sets_desc.set_configurable(true);
        regexp_prototype->set_property_descriptor("unicodeSets", unicode_sets_desc);
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
            if (!this_obj || !this_obj->get_own_property("_isRegExp").to_boolean()) {
                ctx.throw_type_error("RegExp.prototype.test called on incompatible receiver");
                return Value();
            }
            Value arg0_t = args.empty() ? Value() : args[0];
            std::string str;
            if (arg0_t.is_object() || arg0_t.is_function()) { str = arg0_t.to_property_key(); if (ctx.has_exception()) return Value(); }
            else { str = arg0_t.to_string(); }
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
            Value arg0_m = args.empty() ? Value() : args[0];
            std::string str;
            if (arg0_m.is_object() || arg0_m.is_function()) { str = arg0_m.to_property_key(); if (ctx.has_exception()) return Value(); }
            else { str = arg0_m.to_string(); }
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
            Value arg0_r = args.size() > 0 ? args[0] : Value();
            std::string str;
            if (arg0_r.is_object() || arg0_r.is_function()) { str = arg0_r.to_property_key(); if (ctx.has_exception()) return Value(); }
            else { str = arg0_r.to_string(); }
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
                        std::string replacement;
                        if (replace_val.is_function()) {
                            std::vector<Value> fn_a = {matched, Value(static_cast<double>(index)), Value(str)};
                            Value mlen_v = match.as_object()->get_property("length");
                            int ml = mlen_v.is_number() ? static_cast<int>(mlen_v.to_number()) : 1;
                            for (int ci = 1; ci < ml; ci++) fn_a.insert(fn_a.begin()+1+ci-1, match.as_object()->get_element(ci));
                            Value r = replace_val.as_function()->call(ctx, fn_a, Value());
                            if (ctx.has_exception()) return Value();
                            replacement = r.to_string();
                        } else {
                            std::vector<std::string> caps;
                            Value mlen_v = match.as_object()->get_property("length");
                            int ml = mlen_v.is_number() ? static_cast<int>(mlen_v.to_number()) : 1;
                            for (int ci = 1; ci < ml; ci++) {
                                Value cv = match.as_object()->get_element(ci);
                                caps.push_back(cv.is_undefined() ? "" : cv.to_string());
                            }
                            Value grps = match.as_object()->get_property("groups");
                            Object* ng = (!grps.is_undefined() && !grps.is_null() && grps.is_object()) ? grps.as_object() : nullptr;
                            std::string rv_str;
                            if (replace_val.is_object() || replace_val.is_function()) { rv_str = replace_val.to_property_key(); if (ctx.has_exception()) return Value(); }
                            else { rv_str = replace_val.to_string(); }
                            replacement = regexp_get_substitution(rv_str, str, index, matched_str, caps, ng);
                        }
                        return Value(str.substr(0, index) + replacement + str.substr(index + matched_str.length()));
                    }
                }
                return Value(str);
            }
            // Global: collect all matches, then build result.
            this_obj->set_property("lastIndex", Value(0.0));
            Value exec_fn = this_obj->get_property("exec");
            if (!exec_fn.is_function()) return Value(str);
            Function* exec_func = exec_fn.as_function();
            struct MatchRecord { int index; std::string matched; std::vector<std::string> captures; Object* groups = nullptr; };
            std::vector<MatchRecord> matches;
            size_t safety = 0;
            const size_t max_iter = str.length() + 2;
            bool is_fn_replace = replace_val.is_function();
            std::string replace_str;
            if (!is_fn_replace) {
                if (replace_val.is_object() || replace_val.is_function()) { replace_str = replace_val.to_property_key(); if (ctx.has_exception()) return Value(); }
                else { replace_str = replace_val.to_string(); }
            }
            while (safety++ < max_iter) {
                Value match = exec_func->call(ctx, {Value(str)}, Value(this_obj));
                if (ctx.has_exception()) return Value();
                if (match.is_null()) break;
                if (!match.is_object()) break;
                Object* m = match.as_object();
                Value idx_v = m->get_property("index");
                if (ctx.has_exception()) return Value();
                double idx_d = idx_v.to_number();
                if (ctx.has_exception()) return Value();
                int idx = (std::isnan(idx_d) || idx_d < 0) ? 0 : static_cast<int>(idx_d);
                Value el0 = m->get_element(0);
                std::string matched_s;
                if (el0.is_object() || el0.is_function()) { matched_s = el0.to_property_key(); if (ctx.has_exception()) return Value(); }
                else { matched_s = el0.is_undefined() ? "" : el0.to_string(); }
                std::vector<std::string> caps;
                Value len_v = m->get_property("length");
                if (ctx.has_exception()) return Value();
                double len_d = len_v.to_number();
                if (ctx.has_exception()) return Value();
                int mlen = (std::isnan(len_d) || len_d < 1) ? 1 : static_cast<int>(len_d);
                for (int ci = 1; ci < mlen; ci++) {
                    Value cv = m->get_element(ci);
                    if (ctx.has_exception()) return Value();
                    if (cv.is_undefined()) { caps.push_back(""); continue; }
                    std::string cs;
                    if (cv.is_object() || cv.is_function()) { cs = cv.to_property_key(); if (ctx.has_exception()) return Value(); }
                    else { cs = cv.to_string(); }
                    caps.push_back(cs);
                }
                if (ctx.has_exception()) return Value();
                Value grps_v = m->get_property("groups");
                Object* grps_obj = (!grps_v.is_undefined() && !grps_v.is_null() && grps_v.is_object()) ? grps_v.as_object() : nullptr;
                matches.push_back({idx, matched_s, caps, grps_obj});
                // Per spec step 11.c.iii.2: after zero-length match, ToLength(lastIndex) and
                // advance if exec didn't already move past the match position.
                // Real exec advances internally; overridden exec may not.
                if (matched_s.empty()) {
                    Value cur_li = this_obj->get_property("lastIndex");
                    if (ctx.has_exception()) return Value();
                    double thisIndex = cur_li.to_number();
                    if (ctx.has_exception()) return Value();
                    if (std::isnan(thisIndex) || thisIndex < 0) thisIndex = 0;
                    if (static_cast<size_t>(thisIndex) <= static_cast<size_t>(idx)) {
                        this_obj->set_property("lastIndex", Value(static_cast<double>(idx + 1)));
                        if (ctx.has_exception()) return Value();
                    }
                }
            }
            if (matches.empty()) return Value(str);
            std::string result;
            size_t last_end = 0;
            for (auto& mr : matches) {
                size_t mi = (mr.index >= 0 && static_cast<size_t>(mr.index) <= str.size()) ? static_cast<size_t>(mr.index) : str.size();
                if (mi > last_end) result += str.substr(last_end, mi - last_end);
                else if (mi < last_end) { /* overlapping match: skip */ }
                std::string repl;
                if (is_fn_replace) {
                    std::vector<Value> fn_a = {Value(mr.matched)};
                    for (auto& c : mr.captures) fn_a.push_back(Value(c));
                    fn_a.push_back(Value(static_cast<double>(mr.index)));
                    fn_a.push_back(Value(str));
                    Value r = replace_val.as_function()->call(ctx, fn_a, Value());
                    if (ctx.has_exception()) return Value();
                    repl = r.to_string();
                } else {
                    repl = regexp_get_substitution(replace_str, str, mi, mr.matched, mr.captures, mr.groups);
                }
                result += repl;
                last_end = mi + mr.matched.length();
                if (last_end > str.size()) last_end = str.size();
            }
            if (last_end <= str.size()) result += str.substr(last_end);
            return Value(result);
        }, 2);
    regexp_prototype->set_property("Symbol.replace", Value(regexp_sym_replace.release()), PropertyAttributes::BuiltinFunction);

    auto regexp_sym_search = ObjectFactory::create_native_function("[Symbol.search]",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value(-1.0);
            Value arg0_s = args.empty() ? Value() : args[0];
            std::string str;
            if (arg0_s.is_object() || arg0_s.is_function()) { str = arg0_s.to_property_key(); if (ctx.has_exception()) return Value(); }
            else { str = arg0_s.to_string(); }
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
            Value arg0_sp = args.size() > 0 ? args[0] : Value();
            std::string str;
            if (arg0_sp.is_object() || arg0_sp.is_function()) { str = arg0_sp.to_property_key(); if (ctx.has_exception()) return Value(); }
            else { str = arg0_sp.to_string(); }
            // ES6: SpeciesConstructor -- Construct(C, «rx, newFlags»)
            Value ctor_val = this_obj->get_property("constructor");
            if (ctor_val.is_object() || ctor_val.is_function()) {
                Object* ctor_obj = ctor_val.is_function() ? static_cast<Object*>(ctor_val.as_function()) : ctor_val.as_object();
                Symbol* species_sym = Symbol::get_well_known(Symbol::SPECIES);
                if (species_sym) {
                    Value species_val = ctor_obj->get_property(species_sym->to_property_key());
                    if (ctx.has_exception()) return Value();
                    if (!species_val.is_null() && !species_val.is_undefined()) {
                        if (!species_val.is_function()) { ctx.throw_type_error("Species constructor is not a constructor"); return Value(); }
                        Value flags_val = this_obj->get_property("flags");
                        if (ctx.has_exception()) return Value();
                        std::vector<Value> species_args = { Value(this_obj), flags_val };
                        Value splitter = species_val.as_function()->construct(ctx, species_args);
                        if (ctx.has_exception()) return Value();
                        if (splitter.is_object() || splitter.is_function()) {
                            this_obj = splitter.is_function() ? static_cast<Object*>(splitter.as_function()) : splitter.as_object();
                        }
                    }
                }
            }
            // Get lim (ToUint32) and exec, then loop.
            Value limit_v = args.size() > 1 ? args[1] : Value();
            uint32_t lim = limit_v.is_undefined() ? 0xFFFFFFFFu : static_cast<uint32_t>(limit_v.to_number());
            if (lim == 0) return Value(ObjectFactory::create_array(0).release());
            Value exec_fn = this_obj->get_property("exec");
            auto result = ObjectFactory::create_array();
            if (!exec_fn.is_function()) {
                result->set_element(0, Value(str));
                result->set_length(1);
                return Value(result.release());
            }
            Function* exec_func = exec_fn.as_function();
            // Spec: if S is empty, try exec once; if match found return empty array, else [""].
            if (str.empty()) {
                Value z = exec_func->call(ctx, {Value(str)}, Value(this_obj));
                if (ctx.has_exception()) return Value();
                if (!z.is_null() && !z.is_undefined()) return Value(ObjectFactory::create_array(0).release());
                auto r = ObjectFactory::create_array(); r->set_element(0, Value(std::string(""))); r->set_length(1);
                return Value(r.release());
            }
            uint32_t idx = 0;
            size_t last_end = 0;
            bool any_match = false;
            bool last_was_zero_length_advance = false;
            // Reset lastIndex so exec searches from the start.
            this_obj->set_property("lastIndex", Value(0.0));
            size_t safety = 0;
            while (safety++ <= str.length() + 1 && idx < lim) {
                // Search from last_end by passing the substring; returned index is relative.
                if (last_end >= str.size()) break;
                std::string sub = str.substr(last_end);
                if (sub.empty()) break;
                Value match = exec_func->call(ctx, {Value(sub)}, Value(this_obj));
                if (ctx.has_exception()) return Value();
                if (match.is_null()) break;
                if (!match.is_object()) break;
                any_match = true;
                Object* m = match.as_object();
                Value idx_v = m->get_property("index");
                int rel_mi = idx_v.is_number() ? static_cast<int>(idx_v.to_number()) : 0;
                size_t mi = last_end + static_cast<size_t>(rel_mi);
                std::string matched_str = m->get_element(0).is_undefined() ? "" : m->get_element(0).to_string();
                if (matched_str.empty() && mi == last_end) {
                    // Zero-length match at current position: emit the next code unit, like "" string split.
                    if (last_end >= str.length()) break;
                    size_t char_len = 1;
                    unsigned char c = static_cast<unsigned char>(str[last_end]);
                    if ((c & 0xF8) == 0xF0) char_len = 4;
                    else if ((c & 0xF0) == 0xE0) char_len = 3;
                    else if ((c & 0xE0) == 0xC0) char_len = 2;
                    result->set_element(idx++, Value(str.substr(last_end, char_len)));
                    last_end += char_len;
                    last_was_zero_length_advance = true;
                    continue;
                }
                result->set_element(idx++, Value(str.substr(last_end, mi - last_end)));
                if (idx >= lim) { last_end = mi + matched_str.length(); break; }
                Value len_v = m->get_property("length");
                int mlen = len_v.is_number() ? static_cast<int>(len_v.to_number()) : 1;
                for (int ci = 1; ci < mlen && idx < lim; ci++) {
                    Value cv = m->get_element(ci);
                    result->set_element(idx++, cv);
                }
                last_end = mi + (matched_str.empty() ? 1 : matched_str.length());
                last_was_zero_length_advance = false;
            }
            // Add trailing segment when: (a) remaining text, (b) no match found (whole string),
            // or (c) last real (non-zero-length) match ended at the end of the string.
            // Don't add for zero-length-advance paths (character-by-character iteration).
            if (idx < lim && (last_end < str.length() || !any_match || !last_was_zero_length_advance)) {
                // Don't add empty trailing when we've consumed past the end (e.g. /$/split("x")).
                if (last_end <= str.length()) {
                    result->set_element(idx++, Value(str.substr(last_end)));
                }
            }
            result->set_length(idx);
            return Value(result.release());
        }, 2);
    regexp_prototype->set_property("Symbol.split", Value(regexp_sym_split.release()), PropertyAttributes::BuiltinFunction);

    // RegExp.prototype[Symbol.matchAll]: the per-spec built-in that matchAll delegates to.
    {
        auto regexp_sym_matchAll = ObjectFactory::create_native_function("[Symbol.matchAll]",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                Object* this_obj = ctx.get_this_binding();
                if (!this_obj) { ctx.throw_type_error("RegExp.prototype[Symbol.matchAll] called on null"); return Value(); }
                Value arg0_ma = args.empty() ? Value() : args[0];
                std::string str;
                if (arg0_ma.is_object() || arg0_ma.is_function()) { str = arg0_ma.to_property_key(); if (ctx.has_exception()) return Value(); }
                else { str = arg0_ma.to_string(); }
                Value flags_val = this_obj->get_property("flags");
                if (ctx.has_exception()) return Value();
                std::string flags = flags_val.is_string() ? flags_val.to_string() :
                    (flags_val.is_undefined() || flags_val.is_null() ? "" : flags_val.to_string());
                if (flags.find('g') == std::string::npos) {
                    ctx.throw_type_error("String.prototype.matchAll requires global flag");
                    return Value();
                }

                struct MatchAllState { std::string str; Value regex; bool done = false; };
                auto state = std::make_shared<MatchAllState>(MatchAllState{str, Value(this_obj), false});
                auto iterator = ObjectFactory::create_object();
                Object* iter_ptr = iterator.get();
                auto next_fn = ObjectFactory::create_native_function("next",
                    [state](Context& ctx, const std::vector<Value>&) -> Value {
                        auto result = ObjectFactory::create_object();
                        if (state->done) { result->set_property("done", Value(true)); result->set_property("value", Value()); return Value(result.release()); }
                        Object* rx = state->regex.as_object();
                        Value exec_fn = rx ? rx->get_property("exec") : Value();
                        if (!exec_fn.is_function()) { state->done = true; result->set_property("done", Value(true)); result->set_property("value", Value()); return Value(result.release()); }
                        Value match = exec_fn.as_function()->call(ctx, {Value(state->str)}, state->regex);
                        if (ctx.has_exception()) return Value();
                        if (match.is_null() || match.is_undefined()) { state->done = true; result->set_property("done", Value(true)); result->set_property("value", Value()); }
                        else { result->set_property("done", Value(false)); result->set_property("value", match); }
                        return Value(result.release());
                    }, 0);
                iterator->set_property("next", Value(next_fn.release()));
                Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
                if (iter_sym) {
                    auto sym_iter_fn = ObjectFactory::create_native_function("[Symbol.iterator]",
                        [iter_ptr](Context& ctx, const std::vector<Value>&) -> Value { (void)ctx; return Value(iter_ptr); }, 0);
                    iterator->set_property(iter_sym->to_property_key(), Value(sym_iter_fn.release()));
                }
                return Value(iterator.release());
            }, 1);
        Symbol* matchAll_sym = Symbol::get_well_known(Symbol::MATCH_ALL);
        regexp_prototype->set_property(matchAll_sym ? matchAll_sym->to_property_key() : std::string("Symbol.matchAll"),
            Value(regexp_sym_matchAll.release()), PropertyAttributes::BuiltinFunction);
    }

    regexp_constructor->set_property("prototype", Value(regexp_prototype.release()), PropertyAttributes::None);

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
