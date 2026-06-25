/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "quanta/core/engine/builtins/StringBuiltin.h"
#include "quanta/core/engine/Context.h"
#include "quanta/parser/Parser.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/core/runtime/Iterator.h"
#include "quanta/core/runtime/RegExp.h"
#include "quanta/core/runtime/String.h"
#include <cmath>
#include <limits>
#include <sstream>
#include <algorithm>
#include <cctype>
#include "utf8proc.h"
#include "quanta/parser/AST.h"

namespace Quanta {

// GetSubstitution: process $$ / $& / $` / $' / $n in a replacement string.
static std::string apply_substitution(const std::string& replacement, const std::string& str,
        size_t match_pos, const std::string& matched,
        const std::vector<std::string>& captures = {}) {
    std::string result;
    result.reserve(replacement.size());
    for (size_t i = 0; i < replacement.size(); i++) {
        if (replacement[i] != '$' || i + 1 >= replacement.size()) { result += replacement[i]; continue; }
        char next = replacement[i + 1];
        if (next == '$') { result += '$'; i++; }
        else if (next == '&') { result += matched; i++; }
        else if (next == '`') { result += str.substr(0, match_pos); i++; }
        else if (next == '\'') { result += str.substr(match_pos + matched.size()); i++; }
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

// Returns UTF-8 byte length of a Unicode WhiteSpace/LineTerminator at str[i], 0 if not whitespace.
static size_t is_unicode_whitespace(const std::string& str, size_t i) {
    unsigned char c = static_cast<unsigned char>(str[i]);
    if (c == 0x09||c == 0x0A||c == 0x0B||c == 0x0C||c == 0x0D||c == 0x20) return 1;
    if (i + 1 < str.size()) {
        unsigned char c1 = static_cast<unsigned char>(str[i+1]);
        if (c == 0xC2 && c1 == 0xA0) return 2;
        if (i + 2 < str.size()) {
            unsigned char c2 = static_cast<unsigned char>(str[i+2]);
            if (c == 0xEF && c1 == 0xBB && c2 == 0xBF) return 3;
            if (c == 0xE2 && c1 == 0x80 && c2 >= 0x80 && c2 <= 0xAB) return 3;
            if (c == 0xE2 && c1 == 0x80 && c2 == 0xAF) return 3;
            if (c == 0xE2 && c1 == 0x81 && c2 == 0x9F) return 3;
            if (c == 0xE1 && c1 == 0x9A && c2 == 0x80) return 3;
            if (c == 0xE3 && c1 == 0x80 && c2 == 0x80) return 3;
        }
    }
    return 0;
}

static std::string unicode_trim(const std::string& str) {
    size_t start = 0, end = str.size();
    while (start < end) { size_t n = is_unicode_whitespace(str, start); if (!n) break; start += n; }
    while (end > start) {
        size_t p = end - 1;
        while (p > start && (static_cast<unsigned char>(str[p]) & 0xC0) == 0x80) p--;
        size_t n = is_unicode_whitespace(str, p);
        if (!n || p + n != end) break;
        end = p;
    }
    return str.substr(start, end - start);
}

// RequireObjectCoercible + ToString: throws for null/undefined, calls JS toString() on objects.
static std::string string_this_coerce(Context& ctx, const Value& v, bool& ok) {
    ok = false;
    if (v.is_null() || v.is_undefined()) {
        ctx.throw_type_error("String method called on null or undefined");
        return {};
    }
    if (v.is_object() || v.is_function()) {
        Object* obj = v.is_function() ? static_cast<Object*>(v.as_function()) : v.as_object();
        // Fast path: primitive-wrapper objects expose their value via [[PrimitiveValue]] or valueOf().
        auto otype = obj->get_type();
        if (otype == Object::ObjectType::String || otype == Object::ObjectType::Boolean ||
                otype == Object::ObjectType::Number) {
            Value pv = obj->get_property("[[PrimitiveValue]]");
            if (!pv.is_undefined() && !pv.is_object() && !pv.is_function()) { ok = true; return pv.to_string(); }
            Value vof = obj->get_property("valueOf");
            if (!ctx.has_exception() && vof.is_function()) {
                Value r = vof.as_function()->call(ctx, {}, v);
                if (!ctx.has_exception() && !r.is_object() && !r.is_function()) { ok = true; return r.to_string(); }
                if (ctx.has_exception()) return {};
            }
        }
        // Check @@toPrimitive first (string hint).
        Symbol* toPrim_sym = Symbol::get_well_known("Symbol.toPrimitive");
        if (toPrim_sym) {
            Value toPrim = obj->get_property(toPrim_sym->to_property_key());
            if (ctx.has_exception()) return {};
            if (toPrim.is_function()) {
                Value r = toPrim.as_function()->call(ctx, {Value(std::string("string"))}, v);
                if (ctx.has_exception()) return {};
                if (!r.is_object() && !r.is_function()) { ok = true; return r.to_string(); }
                ctx.throw_type_error("Cannot convert object to string");
                return {};
            }
        }
        Value ts = obj->get_property("toString");
        if (ctx.has_exception()) return {};
        if (ts.is_function()) {
            Value r = ts.as_function()->call(ctx, {}, v);
            if (ctx.has_exception()) return {};
            if (!r.is_object() && !r.is_function()) {
                if (r.is_symbol()) { ctx.throw_type_error("Cannot convert a Symbol value to a string"); return {}; }
                ok = true; return r.to_string();
            }
        }
        Value vof = obj->get_property("valueOf");
        if (ctx.has_exception()) return {};
        if (vof.is_function()) {
            Value r = vof.as_function()->call(ctx, {}, v);
            if (ctx.has_exception()) return {};
            if (!r.is_object() && !r.is_function()) {
                if (r.is_symbol()) { ctx.throw_type_error("Cannot convert a Symbol value to a string"); return {}; }
                ok = true; return r.to_string();
            }
        }
        ctx.throw_type_error("Cannot convert object to string");
        return {};
    }
    if (v.is_symbol()) {
        ctx.throw_type_error("Cannot convert a Symbol value to a string");
        return {};
    }
    ok = true;
    return v.to_string();
}

static std::string get_string_this(Context& ctx, bool& ok) {
    if (ctx.original_this_was_nullish()) {
        ctx.throw_type_error("String method called on null or undefined");
        ok = false; return {};
    }
    return string_this_coerce(ctx, ctx.get_binding("this"), ok);
}

void register_string_builtins(Context& ctx) {
    auto string_constructor = ObjectFactory::create_native_constructor("String",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string str_value;
            if (args.empty()) {
                str_value = "";
            } else {
                const Value& arg = args[0];
                if (arg.is_symbol()) {
                    if (ctx.is_in_constructor_call()) {
                        ctx.throw_type_error("Cannot convert a Symbol value to a string");
                        return Value();
                    }
                    str_value = arg.as_symbol()->to_string();
                } else if (arg.is_object() || arg.is_function()) {
                    // Spec: ToString for objects: ToPrimitive("string") -> toString then valueOf.
                    bool _ok;
                    str_value = string_this_coerce(ctx, arg, _ok);
                    if (ctx.has_exception()) return Value();
                } else {
                    str_value = arg.to_string();
                }
            }

            // Return an ObjectType::String-tagged wrapper so Object.prototype.toString
            // sees the correct internal-slot tag (generic Function::construct gives Ordinary).
            Object* old_this = ctx.get_this_binding();
            if (old_this) {
                auto this_obj = std::make_unique<Object>(Object::ObjectType::String);
                this_obj->set_prototype(old_this->get_prototype());
                this_obj->set_property("[[PrimitiveValue]]", Value(str_value), PropertyAttributes::Writable);
                size_t str_utf16_len = utf16_length(str_value);
                PropertyDescriptor length_desc(Value(static_cast<double>(str_utf16_len)),
                    static_cast<PropertyAttributes>(PropertyAttributes::None));
                this_obj->set_property_descriptor("length", length_desc);

                // Indexed character properties: non-writable, enumerable, non-configurable per spec.
                for (size_t i = 0; i < str_utf16_len; i++) {
                    int32_t unit = utf16_code_unit_at(str_value, i);
                    if (unit < 0) break;
                    PropertyDescriptor char_desc(Value(encode_utf16_unit(static_cast<uint32_t>(unit))),
                        PropertyAttributes::Enumerable);
                    this_obj->set_property_descriptor(std::to_string(i), char_desc);
                }

                auto toString_fn = ObjectFactory::create_native_function("toString",
                    [](Context& ctx, const std::vector<Value>& args) -> Value {
                        (void)args;
                        Object* this_binding = ctx.get_this_binding();
                        if (this_binding && this_binding->has_property("[[PrimitiveValue]]")) {
                            return this_binding->get_property("[[PrimitiveValue]]");
                        }
                        return Value(std::string(""));
                    });
                // Don't add own toString: spec doesn't define one on wrapper instances;
                // it should be inherited from String.prototype.toString.
                (void)toString_fn;
                return Value(this_obj.release());
            }

            return Value(str_value);
        });
    
    auto string_prototype = ObjectFactory::create_object();

    // Spec 22.1.3: String.prototype is itself a String object with [[StringData]] = "".
    {
        PropertyDescriptor proto_length_desc(Value(0.0), static_cast<PropertyAttributes>(PropertyAttributes::None));
        string_prototype->set_property_descriptor("length", proto_length_desc);
        string_prototype->set_property("[[PrimitiveValue]]", Value(std::string("")));
    }

    auto padStart_fn = ObjectFactory::create_native_function("padStart",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            bool _ok; std::string str = get_string_this(ctx, _ok); if (!_ok) return Value();
            
            if (args.empty()) return Value(str);
            
            double tl = args[0].to_number();
            if (std::isnan(tl) || tl <= 0 || tl == std::numeric_limits<double>::infinity()) return Value(str);
            tl = std::floor(tl);
            std::string pad_string = ([&]() -> std::string {
                if (args.size() > 1 && !args[1].is_undefined()) {
                    if (args[1].is_symbol()) { ctx.throw_type_error("Cannot convert Symbol to string"); return {}; }
                    if (args[1].is_object() || args[1].is_function()) {
                        bool _f; auto s = string_this_coerce(ctx, args[1], _f); return s;
                    }
                    return args[1].to_string();
                }
                return " ";
            })();
            if (ctx.has_exception()) return Value();
            size_t str_len = utf16_length(str), target = static_cast<size_t>(tl);
            if (target <= str_len) return Value(str);
            size_t pad_need = target - str_len;
            std::string padding;
            if (!pad_string.empty()) {
                // Truncate by UTF-16 code unit; combine surrogate pairs into proper 4-byte
                // UTF-8 supplementary chars so the result compares equal to the original string.
                std::string repeated;
                while (utf16_length(repeated) < pad_need) repeated += pad_string;
                for (size_t i = 0; i < pad_need; i++) {
                    int32_t unit = utf16_code_unit_at(repeated, i);
                    if (unit < 0) break;
                    uint32_t u = static_cast<uint32_t>(unit);
                    if (u >= 0xD800 && u <= 0xDBFF && i + 1 < pad_need) {
                        int32_t low = utf16_code_unit_at(repeated, i + 1);
                        if (low >= 0xDC00 && low <= 0xDFFF) {
                            uint32_t cp = 0x10000 + ((u - 0xD800) << 10) + (static_cast<uint32_t>(low) - 0xDC00);
                            padding += static_cast<char>(0xF0 | (cp >> 18));
                            padding += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                            padding += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            padding += static_cast<char>(0x80 | (cp & 0x3F));
                            i++;
                            continue;
                        }
                    }
                    padding += encode_utf16_unit(u);
                }
            }
            return Value(padding + str);
        }, 1);
    PropertyDescriptor padStart_desc(Value(padStart_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("padStart", padStart_desc);

    auto padEnd_fn = ObjectFactory::create_native_function("padEnd",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            bool _ok; std::string str = get_string_this(ctx, _ok); if (!_ok) return Value();
            if (args.empty()) return Value(str);
            double tl = args[0].to_number();
            if (std::isnan(tl) || tl <= 0 || tl == std::numeric_limits<double>::infinity()) return Value(str);
            tl = std::floor(tl);
            std::string pad_string = ([&]() -> std::string {
                if (args.size() > 1 && !args[1].is_undefined()) {
                    if (args[1].is_symbol()) { ctx.throw_type_error("Cannot convert Symbol to string"); return {}; }
                    if (args[1].is_object() || args[1].is_function()) {
                        bool _f; auto s = string_this_coerce(ctx, args[1], _f); return s;
                    }
                    return args[1].to_string();
                }
                return " ";
            })();
            if (ctx.has_exception()) return Value();
            size_t str_len = utf16_length(str), target = static_cast<size_t>(tl);
            if (target <= str_len) return Value(str);
            size_t pad_need = target - str_len;
            std::string padding;
            if (!pad_string.empty()) {
                std::string repeated;
                while (utf16_length(repeated) < pad_need) repeated += pad_string;
                for (size_t i = 0; i < pad_need; i++) {
                    int32_t unit = utf16_code_unit_at(repeated, i);
                    if (unit < 0) break;
                    uint32_t u = static_cast<uint32_t>(unit);
                    if (u >= 0xD800 && u <= 0xDBFF && i + 1 < pad_need) {
                        int32_t low = utf16_code_unit_at(repeated, i + 1);
                        if (low >= 0xDC00 && low <= 0xDFFF) {
                            uint32_t cp = 0x10000 + ((u - 0xD800) << 10) + (static_cast<uint32_t>(low) - 0xDC00);
                            padding += static_cast<char>(0xF0 | (cp >> 18));
                            padding += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                            padding += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            padding += static_cast<char>(0x80 | (cp & 0x3F));
                            i++;
                            continue;
                        }
                    }
                    padding += encode_utf16_unit(u);
                }
            }
            return Value(str + padding);
        }, 1);
    PropertyDescriptor padEnd_desc(Value(padEnd_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("padEnd", padEnd_desc);

    // Helper: full spec ToString coercion for argument values (not `this`).
    // Checks @@toPrimitive("string"), then toString, then valueOf, propagates all exceptions.
    auto obj_to_string = [](Context& ctx, const Value& val) -> std::string {
        if (val.is_symbol()) { ctx.throw_type_error("Cannot convert a Symbol value to a string"); return {}; }
        if (val.is_object() || val.is_function()) {
            Object* obj = val.is_function() ? static_cast<Object*>(val.as_function()) : val.as_object();
            Symbol* toPrim_sym = Symbol::get_well_known(Symbol::TO_PRIMITIVE);
            if (toPrim_sym) {
                Value toPrim = obj->get_property(toPrim_sym->to_property_key());
                if (ctx.has_exception()) return {};
                if (!toPrim.is_null() && !toPrim.is_undefined()) {
                    if (!toPrim.is_function()) { ctx.throw_type_error("@@toPrimitive is not callable"); return {}; }
                    Value r = toPrim.as_function()->call(ctx, {Value(std::string("string"))}, val);
                    if (ctx.has_exception()) return {};
                    if (r.is_object() || r.is_function()) { ctx.throw_type_error("@@toPrimitive returned an object"); return {}; }
                    if (r.is_symbol()) { ctx.throw_type_error("Cannot convert a Symbol value to a string"); return {}; }
                    return r.to_string();
                }
            }
            Value ts = obj->get_property("toString");
            if (ctx.has_exception()) return {};
            if (ts.is_function()) {
                Value r = ts.as_function()->call(ctx, {}, val);
                if (ctx.has_exception()) return {};
                if (!r.is_object() && !r.is_function()) {
                    if (r.is_symbol()) { ctx.throw_type_error("Cannot convert a Symbol value to a string"); return {}; }
                    return r.to_string();
                }
            }
            Value vof = obj->get_property("valueOf");
            if (ctx.has_exception()) return {};
            if (vof.is_function()) {
                Value r = vof.as_function()->call(ctx, {}, val);
                if (ctx.has_exception()) return {};
                if (!r.is_object() && !r.is_function()) {
                    if (r.is_symbol()) { ctx.throw_type_error("Cannot convert a Symbol value to a string"); return {}; }
                    return r.to_string();
                }
            }
            ctx.throw_type_error("Cannot convert object to string");
            return {};
        }
        return val.to_string();
    };

    auto str_includes_fn = ObjectFactory::create_native_function("includes",
        [obj_to_string](Context& ctx, const std::vector<Value>& args) -> Value {
            bool _ok; std::string str = get_string_this(ctx, _ok); if (!_ok) return Value();

            if (args.empty()) return Value(false);

            // ES6: throw TypeError if argument is a regexp (has Symbol.match truthy)
            if (args[0].is_object() || args[0].is_function()) {
                Object* arg_obj = args[0].is_function()
                    ? static_cast<Object*>(args[0].as_function())
                    : args[0].as_object();
                Value sym_match = arg_obj->get_property("Symbol.match");
                if (ctx.has_exception()) return Value();
                if (sym_match.is_undefined()) {
                    // No Symbol.match property - check if it's a RegExp
                    if (arg_obj->get_type() == Object::ObjectType::RegExp) {
                        ctx.throw_type_error("First argument to String.prototype.includes must not be a regular expression");
                        return Value();
                    }
                } else if (sym_match.to_boolean()) {
                    ctx.throw_type_error("First argument to String.prototype.includes must not be a regular expression");
                    return Value();
                }
            }

            if (args[0].is_symbol()) {
                ctx.throw_type_error("Cannot convert a Symbol value to a string");
                return Value();
            }

            std::string search_string = obj_to_string(ctx, args[0]);
            size_t position = 0;
            if (args.size() > 1) {
                if (args[1].is_symbol()) {
                    ctx.throw_type_error("Cannot convert a Symbol value to a number");
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
            bool this_ok;
            std::string str = get_string_this(ctx, this_ok);
            if (!this_ok) return Value();

            if (args.empty()) return Value(false);

            // ES6: throw TypeError if argument is a regexp (has Symbol.match truthy)
            if (args[0].is_object() || args[0].is_function()) {
                Object* arg_obj = args[0].is_function()
                    ? static_cast<Object*>(args[0].as_function())
                    : args[0].as_object();
                Value sym_match = arg_obj->get_property("Symbol.match");
                if (ctx.has_exception()) return Value();
                if (sym_match.is_undefined()) {
                    if (arg_obj->get_type() == Object::ObjectType::RegExp) {
                        ctx.throw_type_error("First argument to String.prototype.startsWith must not be a regular expression");
                        return Value();
                    }
                } else if (sym_match.to_boolean()) {
                    ctx.throw_type_error("First argument to String.prototype.startsWith must not be a regular expression");
                    return Value();
                }
            }

            if (args[0].is_symbol()) {
                ctx.throw_type_error("Cannot convert a Symbol value to a string");
                return Value();
            }

            std::string search_string = obj_to_string(ctx, args[0]);
            size_t position = 0;
            if (args.size() > 1) {
                if (args[1].is_symbol()) {
                    ctx.throw_type_error("Cannot convert a Symbol value to a number");
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
            bool this_ok;
            std::string str = get_string_this(ctx, this_ok);
            if (!this_ok) return Value();

            if (args.empty()) return Value(false);

            // ES6: throw TypeError if argument is a regexp (has Symbol.match truthy)
            if (args[0].is_object() || args[0].is_function()) {
                Object* arg_obj = args[0].is_function()
                    ? static_cast<Object*>(args[0].as_function())
                    : args[0].as_object();
                Value sym_match = arg_obj->get_property("Symbol.match");
                if (ctx.has_exception()) return Value();
                if (sym_match.is_undefined()) {
                    if (arg_obj->get_type() == Object::ObjectType::RegExp) {
                        ctx.throw_type_error("First argument to String.prototype.endsWith must not be a regular expression");
                        return Value();
                    }
                } else if (sym_match.to_boolean()) {
                    ctx.throw_type_error("First argument to String.prototype.endsWith must not be a regular expression");
                    return Value();
                }
            }

            if (args[0].is_symbol()) {
                ctx.throw_type_error("Cannot convert a Symbol value to a string");
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
        // RequireObjectCoercible: original thisArg was null/undefined
        if (ctx.original_this_was_nullish()) {
            ctx.throw_type_error("String method called on null or undefined");
            return "";
        }
        if (this_value.is_symbol()) {
            ctx.throw_type_error("Cannot convert a Symbol value to a string");
            return "";
        }
        if (this_value.is_object() || this_value.is_function()) {
            bool _ok;
            return string_this_coerce(ctx, this_value, _ok);
        }
        return this_value.to_string();
    };

    auto match_fn = ObjectFactory::create_native_function("match",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Spec: GetMethod(regexp, @@match) is checked, and called with the RAW (not yet
            // ToString'd) `this` value, before this value is ever coerced -- so a poisoned
            // this.toString must not run if regexp[Symbol.match] short-circuits first.
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("String method called on null or undefined"); return Value(); }
            Value this_value = ctx.get_binding("this");

            Value regexp = args.empty() ? Value() : args[0];

            if (!regexp.is_null() && !regexp.is_undefined()) {
                Object* regexp_obj = regexp.is_function() ? static_cast<Object*>(regexp.as_function())
                    : (regexp.is_object() ? regexp.as_object() : nullptr);
                if (regexp_obj) {
                    Value matcher = regexp_obj->get_property("Symbol.match");
                    if (ctx.has_exception()) return Value();
                    if (!matcher.is_null() && !matcher.is_undefined()) {
                        if (!matcher.is_function()) { ctx.throw_type_error("Symbol.match is not a function"); return Value(); }
                        return matcher.as_function()->call(ctx, {this_value}, regexp);
                    }
                }
            }

            bool this_ok;
            std::string str = get_string_this(ctx, this_ok);
            if (!this_ok) return Value();

            // RegExpCreate(regexp, undefined): build a fresh RegExp, then delegate to its own
            // [Symbol.match] rather than re-implementing the exec/global-loop logic here.
            Value regexp_ctor = ctx.get_binding("RegExp");
            if (!regexp_ctor.is_function()) { ctx.throw_type_error("RegExp constructor not found"); return Value(); }
            Value rx = regexp_ctor.as_function()->construct(ctx, {regexp, Value()});
            if (ctx.has_exception()) return Value();
            Object* rx_obj = rx.is_function() ? static_cast<Object*>(rx.as_function()) : (rx.is_object() ? rx.as_object() : nullptr);
            if (!rx_obj) { ctx.throw_type_error("RegExpCreate did not return an object"); return Value(); }
            Value match_method = rx_obj->get_property("Symbol.match");
            if (ctx.has_exception()) return Value();
            if (!match_method.is_function()) { ctx.throw_type_error("RegExp.prototype[Symbol.match] is not a function"); return Value(); }
            return match_method.as_function()->call(ctx, {Value(str)}, rx);
        }, 1);
    PropertyDescriptor match_desc(Value(match_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("match", match_desc);

    auto matchAll_fn = ObjectFactory::create_native_function("matchAll",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("String method called on null or undefined"); return Value(); }
            Value this_value = ctx.get_binding("this");

            Value regexp = args.empty() ? Value() : args[0];

            if (!regexp.is_null() && !regexp.is_undefined()) {
                Object* regexp_obj = regexp.is_function() ? static_cast<Object*>(regexp.as_function())
                    : (regexp.is_object() ? regexp.as_object() : nullptr);
                if (regexp_obj) {
                    // IsRegExp: duck-typed via @@match, or a native RegExp.
                    Symbol* match_sym = Symbol::get_well_known(Symbol::MATCH);
                    Value matcher_check = match_sym ? regexp_obj->get_property(match_sym->to_property_key()) : Value();
                    if (ctx.has_exception()) return Value();
                    bool is_regexp = !matcher_check.is_undefined() ? matcher_check.to_boolean() : (regexp_obj->get_type() == Object::ObjectType::RegExp);
                    if (is_regexp) {
                        Value flags_val = regexp_obj->get_property("flags");
                        if (ctx.has_exception()) return Value();
                        if (flags_val.is_null() || flags_val.is_undefined()) {
                            ctx.throw_type_error("Cannot convert undefined or null flags to object");
                            return Value();
                        }
                        std::string flags;
                        if (flags_val.is_symbol()) { ctx.throw_type_error("Cannot convert a Symbol value to a string"); return Value(); }
                        if (flags_val.is_object() || flags_val.is_function()) {
                            bool _f; flags = string_this_coerce(ctx, flags_val, _f);
                            if (ctx.has_exception()) return Value();
                        } else {
                            flags = flags_val.to_string();
                        }
                        if (flags.find('g') == std::string::npos) {
                            ctx.throw_type_error("String.prototype.matchAll must be called with a global RegExp");
                            return Value();
                        }
                    }

                    // GetMethod(regexp, @@matchAll): a user-overridden matcher, called with the
                    // raw (not yet ToString'd) `this` value.
                    Symbol* matchAll_sym = Symbol::get_well_known(Symbol::MATCH_ALL);
                    Value matcher = matchAll_sym ? regexp_obj->get_property(matchAll_sym->to_property_key()) : Value();
                    if (ctx.has_exception()) return Value();
                    if (!matcher.is_null() && !matcher.is_undefined()) {
                        if (!matcher.is_function()) { ctx.throw_type_error("Symbol.matchAll is not a function"); return Value(); }
                        return matcher.as_function()->call(ctx, {this_value}, regexp);
                    }
                }
            }

            bool this_ok;
            std::string str = get_string_this(ctx, this_ok);
            if (!this_ok) return Value();

            // RegExpCreate(regexp, "g"): check for any installed RegExp.prototype[Symbol.matchAll]
            // (no built-in, but tests/user code can install one) before falling back to iterator.
            Value regexp_ctor = ctx.get_binding("RegExp");
            if (!regexp_ctor.is_function()) { ctx.throw_type_error("RegExp constructor not found"); return Value(); }
            Value rx = regexp_ctor.as_function()->construct(ctx, {regexp, Value(std::string("g"))});
            if (ctx.has_exception()) return Value();
            Object* rx_obj = rx.is_function() ? static_cast<Object*>(rx.as_function()) : (rx.is_object() ? rx.as_object() : nullptr);
            if (!rx_obj) { ctx.throw_type_error("RegExpCreate did not return an object"); return Value(); }
            Value rx_matchAll = rx_obj->get_property("Symbol.matchAll");
            if (ctx.has_exception()) return Value();
            if (rx_matchAll.is_function()) {
                return rx_matchAll.as_function()->call(ctx, {Value(str)}, rx);
            }
            // Invoke(rx, @@matchAll, «S»): undefined/non-callable throws TypeError (per spec Invoke).
            if (!rx_matchAll.is_function()) {
                ctx.throw_type_error(rx_matchAll.is_null() || rx_matchAll.is_undefined()
                    ? "rx[Symbol.matchAll] is not defined" : "rx[Symbol.matchAll] is not a function");
                return Value();
            }
            return rx_matchAll.as_function()->call(ctx, {Value(str)}, rx);
        }, 1);
    PropertyDescriptor matchAll_desc(Value(matchAll_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("matchAll", matchAll_desc);

    auto search_fn = ObjectFactory::create_native_function("search",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Spec: GetMethod(regexp, @@search) is checked, and called with the RAW `this`
            // value, before this value is ever coerced.
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("String method called on null or undefined"); return Value(); }
            Value this_value = ctx.get_binding("this");

            Value regexp = args.empty() ? Value() : args[0];

            if (!regexp.is_null() && !regexp.is_undefined()) {
                Object* regexp_obj = regexp.is_function() ? static_cast<Object*>(regexp.as_function())
                    : (regexp.is_object() ? regexp.as_object() : nullptr);
                if (regexp_obj) {
                    Value searcher = regexp_obj->get_property("Symbol.search");
                    if (ctx.has_exception()) return Value();
                    if (searcher.is_function()) {
                        return searcher.as_function()->call(ctx, {this_value}, regexp);
                    }
                }
            }

            bool this_ok;
            std::string str = get_string_this(ctx, this_ok);
            if (!this_ok) return Value();

            Value regexp_ctor = ctx.get_binding("RegExp");
            if (!regexp_ctor.is_function()) { ctx.throw_type_error("RegExp constructor not found"); return Value(); }
            Value rx = regexp_ctor.as_function()->construct(ctx, {regexp, Value()});
            if (ctx.has_exception()) return Value();
            Object* rx_obj = rx.is_function() ? static_cast<Object*>(rx.as_function()) : (rx.is_object() ? rx.as_object() : nullptr);
            if (!rx_obj) { ctx.throw_type_error("RegExpCreate did not return an object"); return Value(); }
            Value search_method = rx_obj->get_property("Symbol.search");
            if (ctx.has_exception()) return Value();
            if (!search_method.is_function()) { ctx.throw_type_error("RegExp.prototype[Symbol.search] is not a function"); return Value(); }
            return search_method.as_function()->call(ctx, {Value(str)}, rx);
        }, 1);
    PropertyDescriptor search_desc(Value(search_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("search", search_desc);

    auto replace_fn = ObjectFactory::create_native_function("replace",
        [obj_to_string](Context& ctx, const std::vector<Value>& args) -> Value {
            // Spec: GetMethod(searchValue, @@replace) must happen BEFORE ToString(this).
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("String method called on null or undefined"); return Value(); }
            Value this_value = ctx.get_binding("this");

            Value search_value = args.empty() ? Value() : args[0];
            Value replace_value = args.size() > 1 ? args[1] : Value();

            if (!search_value.is_null() && !search_value.is_undefined()) {
                Object* sv_obj = search_value.is_function() ? static_cast<Object*>(search_value.as_function())
                    : (search_value.is_object() ? search_value.as_object() : nullptr);
                if (sv_obj) {
                    Value replacer = sv_obj->get_property("Symbol.replace");
                    if (ctx.has_exception()) return Value();
                    if (!replacer.is_null() && !replacer.is_undefined()) {
                        if (!replacer.is_function()) { ctx.throw_type_error("Symbol.replace is not a function"); return Value(); }
                        return replacer.as_function()->call(ctx, {this_value, replace_value}, search_value);
                    }
                }
            }

            bool this_ok;
            std::string str = get_string_this(ctx, this_ok);
            if (!this_ok) return Value();

            std::string search;
            if (search_value.is_symbol()) { ctx.throw_type_error("Cannot convert a Symbol value to a string"); return Value(); }
            if (search_value.is_object() || search_value.is_function()) {
                bool _f; search = string_this_coerce(ctx, search_value, _f);
                if (ctx.has_exception()) return Value();
            } else {
                search = search_value.to_string();
            }

            bool is_function_replace = replace_value.is_function();
            std::string replace_str;
            if (!is_function_replace) {
                if (replace_value.is_symbol()) { ctx.throw_type_error("Cannot convert a Symbol value to a string"); return Value(); }
                if (replace_value.is_object() || replace_value.is_function()) {
                    bool _f; replace_str = string_this_coerce(ctx, replace_value, _f);
                    if (ctx.has_exception()) return Value();
                } else {
                    replace_str = replace_value.to_string();
                }
            }

            size_t pos = str.find(search);
            if (pos == std::string::npos) return Value(str);

            std::string replacement;
            if (is_function_replace) {
                std::vector<Value> fn_args = { Value(search), Value(static_cast<double>(pos)), Value(str) };
                Value r = replace_value.as_function()->call(ctx, fn_args, Value());
                if (ctx.has_exception()) return Value();
                replacement = obj_to_string(ctx, r);
                if (ctx.has_exception()) return Value();
            } else {
                replacement = apply_substitution(replace_str, str, pos, search);
            }

            return Value(str.substr(0, pos) + replacement + str.substr(pos + search.length()));
        }, 2);
    PropertyDescriptor replace_desc(Value(replace_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("replace", replace_desc);

    auto replaceAll_fn = ObjectFactory::create_native_function("replaceAll",
        [obj_to_string](Context& ctx, const std::vector<Value>& args) -> Value {
            // Spec order matters: RequireObjectCoercible(this) only checks nullish here --
            // ToString(this) must NOT run yet, since the IsRegExp/flags validation below has
            // to happen first and can throw before `this` or replaceValue are ever coerced.
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("String method called on null or undefined"); return Value(); }

            Value search_value = args.empty() ? Value() : args[0];
            Value replace_value = args.size() > 1 ? args[1] : Value();

            // IsRegExp: duck-typed via @@match (any object), not just a native RegExp.
            if (!search_value.is_null() && !search_value.is_undefined()) {
                Object* sv_obj = search_value.is_function() ? static_cast<Object*>(search_value.as_function())
                    : (search_value.is_object() ? search_value.as_object() : nullptr);
                if (sv_obj) {
                    Symbol* match_sym = Symbol::get_well_known(Symbol::MATCH);
                    Value matcher = match_sym ? sv_obj->get_property(match_sym->to_property_key()) : Value();
                    if (ctx.has_exception()) return Value();
                    bool is_regexp = !matcher.is_undefined() ? matcher.to_boolean() : (sv_obj->get_type() == Object::ObjectType::RegExp);
                    if (is_regexp) {
                        Value flags_val = sv_obj->get_property("flags");
                        if (ctx.has_exception()) return Value();
                        if (flags_val.is_null() || flags_val.is_undefined()) {
                            ctx.throw_type_error("Cannot convert undefined or null flags to object");
                            return Value();
                        }
                        std::string flags;
                        if (flags_val.is_symbol()) { ctx.throw_type_error("Cannot convert a Symbol value to a string"); return Value(); }
                        if (flags_val.is_object() || flags_val.is_function()) {
                            bool _f; flags = string_this_coerce(ctx, flags_val, _f);
                            if (ctx.has_exception()) return Value();
                        } else {
                            flags = flags_val.to_string();
                        }
                        if (flags.find('g') == std::string::npos) {
                            ctx.throw_type_error("String.prototype.replaceAll must be called with a global RegExp");
                            return Value();
                        }
                    }
                    // GetMethod(searchValue, @@replace): delegate if callable.
                    // Object.cpp's own-undefined-property fix ensures explicit Symbol.replace=undefined
                    // won't find the prototype method, so this correctly skips delegation for those.
                    Value this_raw = ctx.get_binding("this");
                    Value replacer = sv_obj->get_property("Symbol.replace");
                    if (ctx.has_exception()) return Value();
                    if (!replacer.is_null() && !replacer.is_undefined()) {
                        if (!replacer.is_function()) { ctx.throw_type_error("Symbol.replace is not a function"); return Value(); }
                        return replacer.as_function()->call(ctx, {this_raw, replace_value}, search_value);
                    }
                }
            }

            bool this_ok;
            std::string str = get_string_this(ctx, this_ok);
            if (!this_ok) return Value();

            std::string search;
            if (search_value.is_symbol()) { ctx.throw_type_error("Cannot convert a Symbol value to a string"); return Value(); }
            if (search_value.is_object() || search_value.is_function()) {
                bool _f; search = string_this_coerce(ctx, search_value, _f);
                if (ctx.has_exception()) return Value();
            } else {
                search = search_value.to_string();
            }

            bool is_function = replace_value.is_function();
            std::string replace_str;
            if (!is_function) {
                if (replace_value.is_symbol()) { ctx.throw_type_error("Cannot convert a Symbol value to a string"); return Value(); }
                if (replace_value.is_object()) {
                    bool _f; replace_str = string_this_coerce(ctx, replace_value, _f);
                    if (ctx.has_exception()) return Value();
                } else {
                    replace_str = replace_value.to_string();
                }
            }

            size_t search_len = search.length();
            size_t advance = std::max<size_t>(1, search_len);
            std::vector<size_t> positions;
            {
                size_t pos = 0;
                while (true) {
                    size_t found = str.find(search, pos);
                    if (found == std::string::npos) break;
                    positions.push_back(found);
                    pos = found + advance;
                }
            }

            std::string result;
            size_t end_of_last_match = 0;
            for (size_t p : positions) {
                result += str.substr(end_of_last_match, p - end_of_last_match);
                std::string replacement;
                if (is_function) {
                    std::vector<Value> fn_args = { Value(search), Value(static_cast<double>(p)), Value(str) };
                    Value r = replace_value.as_function()->call(ctx, fn_args);
                    if (ctx.has_exception()) return Value();
                    replacement = obj_to_string(ctx, r);
                    if (ctx.has_exception()) return Value();
                } else {
                    replacement = apply_substitution(replace_str, str, p, search);
                }
                result += replacement;
                end_of_last_match = p + search_len;
            }
            if (end_of_last_match < str.length()) result += str.substr(end_of_last_match);
            return Value(result);
        }, 2);
    PropertyDescriptor replaceAll_desc(Value(replaceAll_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("replaceAll", replaceAll_desc);

    auto trim_fn = ObjectFactory::create_native_function("trim",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            bool _ok; std::string str = get_string_this(ctx, _ok); if (!_ok) return Value();
            return Value(unicode_trim(str));
        }, 0);
    PropertyDescriptor trim_desc(Value(trim_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("trim", trim_desc);

    auto trimStart_fn = ObjectFactory::create_native_function("trimStart",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            bool _ok; std::string str = get_string_this(ctx, _ok); if (!_ok) return Value();
            size_t start = 0;
            while (start < str.size()) { size_t n = is_unicode_whitespace(str, start); if (!n) break; start += n; }
            return Value(str.substr(start));
        }, 0);
    PropertyDescriptor trimStart_desc(Value(trimStart_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("trimStart", trimStart_desc);
    string_prototype->set_property_descriptor("trimLeft", trimStart_desc);

    auto trimEnd_fn = ObjectFactory::create_native_function("trimEnd",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            bool _ok; std::string str = get_string_this(ctx, _ok); if (!_ok) return Value();
            size_t end = str.size();
            while (end > 0) {
                size_t p = end - 1;
                while (p > 0 && (static_cast<unsigned char>(str[p]) & 0xC0) == 0x80) p--;
                size_t n = is_unicode_whitespace(str, p);
                if (!n || p + n != end) break;
                end = p;
            }
            return Value(str.substr(0, end));
        }, 0);
    PropertyDescriptor trimEnd_desc(Value(trimEnd_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("trimEnd", trimEnd_desc);
    string_prototype->set_property_descriptor("trimRight", trimEnd_desc);

    auto codePointAt_fn = ObjectFactory::create_native_function("codePointAt",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            bool this_ok;
            std::string str = get_string_this(ctx, this_ok);
            if (!this_ok) return Value();

            if (args.size() == 0 || str.empty()) return Value();

            if (args[0].is_symbol()) { ctx.throw_type_error("Cannot convert a Symbol value to a number"); return Value(); }
            double dpos = args[0].to_number();
            if (ctx.has_exception()) return Value();
            int32_t pos = std::isnan(dpos) ? 0 : static_cast<int32_t>(dpos);
            if (pos < 0 || static_cast<size_t>(pos) >= utf16_length(str)) {
                return Value();
            }

            int32_t cp = utf16_code_point_at(str, static_cast<size_t>(pos));
            if (cp < 0) return Value();
            return Value(static_cast<double>(cp));
        }, 1);
    PropertyDescriptor codePointAt_desc(Value(codePointAt_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("codePointAt", codePointAt_desc);

    auto localeCompare_fn = ObjectFactory::create_native_function("localeCompare",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            bool this_ok;
            std::string str = get_string_this(ctx, this_ok);
            if (!this_ok) return Value();

            std::string that = args.empty() ? "undefined" : args[0].to_string();

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
                if (args[0].is_symbol()) { ctx.throw_type_error("Cannot convert a Symbol value to a number"); return Value(); }
                index = static_cast<uint32_t>(args[0].to_number());
                if (ctx.has_exception()) return Value();
            }

            int32_t unit = utf16_code_unit_at(str, index);
            if (unit < 0) {
                return Value(std::string(""));
            }

            return Value(encode_utf16_unit(static_cast<uint32_t>(unit)));
        }, 1);
    PropertyDescriptor charAt_desc(Value(charAt_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("charAt", charAt_desc);

    auto string_at_fn = ObjectFactory::create_native_function("at",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("String method called on null or undefined"); return Value(); }
            std::string str = this_value.to_string();

            Value idx_arg = args.empty() ? Value() : args[0];  // undefined if no args → ToInteger → 0
            if (idx_arg.is_symbol() || idx_arg.is_bigint()) {
                ctx.throw_type_error("Cannot convert Symbol/BigInt to number");
                return Value();
            }
            double d = idx_arg.to_number();
            if (ctx.has_exception()) return Value();
            int64_t index = std::isnan(d) ? 0 : static_cast<int64_t>(d);
            int64_t len = static_cast<int64_t>(utf16_length(str));

            if (index < 0) {
                index = len + index;
            }

            if (index < 0 || index >= len) {
                return Value();
            }

            int32_t unit = utf16_code_unit_at(str, static_cast<size_t>(index));
            if (unit < 0) return Value();
            return Value(encode_utf16_unit(static_cast<uint32_t>(unit)));
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
                if (args[0].is_symbol()) { ctx.throw_type_error("Cannot convert a Symbol value to a number"); return Value(); }
                index = static_cast<uint32_t>(args[0].to_number());
                if (ctx.has_exception()) return Value();
            }

            int32_t unit = utf16_code_unit_at(str, index);
            if (unit < 0) {
                return Value(std::numeric_limits<double>::quiet_NaN());
            }

            return Value(static_cast<double>(unit));
        }, 1);
    PropertyDescriptor charCodeAt_desc(Value(charCodeAt_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("charCodeAt", charCodeAt_desc);

    auto str_indexOf_fn = ObjectFactory::create_native_function("indexOf",
        [toString_helper, obj_to_string](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            std::string str = toString_helper(ctx, this_value);

            if (args.empty()) {
                return Value(-1.0);
            }

            std::string search = obj_to_string(ctx, args[0]);
            if (ctx.has_exception()) return Value();
            size_t start = 0;
            if (args.size() > 1 && !args[1].is_undefined()) {
                if (args[1].is_symbol() || args[1].is_bigint()) {
                    ctx.throw_type_error("Cannot convert Symbol/BigInt to number");
                    return Value();
                }
                double pos = args[1].to_number();
                if (ctx.has_exception()) return Value();
                if (!std::isnan(pos) && pos >= 0) start = static_cast<size_t>(pos);
            }

            size_t found_pos = str.find(search, start);
            return Value(found_pos == std::string::npos ? -1.0 : static_cast<double>(found_pos));
        }, 1);
    PropertyDescriptor string_indexOf_desc(Value(str_indexOf_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("indexOf", string_indexOf_desc);

    auto str_split_fn = ObjectFactory::create_native_function("split",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Spec: GetMethod(separator, @@split) before ToString(this); call with (O, limit).
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("String method called on null or undefined"); return Value(); }
            Value this_value = ctx.get_binding("this");

            // GetMethod(separator, @@split): pass raw `this` and limit, before any coercion.
            Value separator = args.empty() ? Value() : args[0];
            Value limit_val = args.size() > 1 ? args[1] : Value();

            if (!separator.is_null() && !separator.is_undefined()) {
                Object* sep_obj = separator.is_function() ? static_cast<Object*>(separator.as_function())
                    : (separator.is_object() ? separator.as_object() : nullptr);
                if (sep_obj) {
                    Value splitter = sep_obj->get_property("Symbol.split");
                    if (ctx.has_exception()) return Value();
                    if (!splitter.is_null() && !splitter.is_undefined()) {
                        if (!splitter.is_function()) { ctx.throw_type_error("Symbol.split is not a function"); return Value(); }
                        return splitter.as_function()->call(ctx, {this_value, limit_val}, separator);
                    }
                }
            }

            bool this_ok;
            std::string str = get_string_this(ctx, this_ok);
            if (!this_ok) return Value();

            // ToUint32(limit) before ToString(separator) per spec.
            uint32_t lim;
            if (limit_val.is_undefined()) {
                lim = 0xFFFFFFFFu;
            } else {
                double d = limit_val.to_number();
                if (ctx.has_exception()) return Value();
                lim = std::isnan(d) ? 0u : static_cast<uint32_t>(d);
            }

            auto result_array = ObjectFactory::create_array(0);

            if (separator.is_undefined()) {
                if (lim != 0) result_array->set_element(0, Value(str));
                if (lim != 0) result_array->set_length(1);
                return Value(result_array.release());
            }

            // For RegExp separators that reached here (Symbol.split was null/undefined),
            // delegate to RegExp.prototype[Symbol.split] to get correct Unicode/flag handling.
            if (separator.is_object() || separator.is_function()) {
                Object* sep_obj = separator.is_function() ? static_cast<Object*>(separator.as_function()) : separator.as_object();
                Value split_method = sep_obj->get_property("Symbol.split");
                if (ctx.has_exception()) return Value();
                if (split_method.is_function()) {
                    return split_method.as_function()->call(ctx, {this_value, limit_val}, separator);
                }
                // Object with no usable Symbol.split -- fall through to string coercion below.
            }

            // Fallback: ToString(separator) -- must happen BEFORE the lim=0 early-exit per ES2024.
            std::string sep_str;
            if (separator.is_symbol()) { ctx.throw_type_error("Cannot convert a Symbol value to a string"); return Value(); }
            if (separator.is_object() || separator.is_function()) {
                bool _f; sep_str = string_this_coerce(ctx, separator, _f);
                if (ctx.has_exception()) return Value();
            } else {
                sep_str = separator.to_string();
            }

            if (lim == 0) return Value(result_array.release());

            uint32_t idx = 0;
            if (sep_str.empty()) {
                for (size_t i = 0; i < utf16_length(str) && idx < lim; i++) {
                    int32_t unit = utf16_code_unit_at(str, i);
                    if (unit < 0) break;
                    result_array->set_element(idx++, Value(encode_utf16_unit(static_cast<uint32_t>(unit))));
                }
            } else {
                size_t start = 0, found;
                while (idx < lim && (found = str.find(sep_str, start)) != std::string::npos) {
                    result_array->set_element(idx++, Value(str.substr(start, found - start)));
                    start = found + sep_str.length();
                }
                if (idx < lim) result_array->set_element(idx++, Value(str.substr(start)));
            }
            result_array->set_length(idx);
            return Value(result_array.release());
        }, 2);
    PropertyDescriptor string_split_desc(Value(str_split_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("split", string_split_desc);

    // SpecialCasing.txt non-conditional toUpperCase multi-char mappings.
    static const std::unordered_map<utf8proc_int32_t, std::vector<utf8proc_int32_t>> special_upper = {
        {0x00DF, {0x0053, 0x0053}},  // ß → SS
        {0x0149, {0x02BC, 0x004E}},  // ŉ → ʼN
        {0x0390, {0x0399, 0x0308, 0x0301}},  // ΐ → Ϊ́
        {0x03B0, {0x03A5, 0x0308, 0x0301}},  // ΰ → Ϋ́
        {0x01F0, {0x004A, 0x030C}},  // ǰ → J̌
        {0x1E96, {0x0048, 0x0331}},  // ẖ → H̱
        {0x1E97, {0x0054, 0x0308}},  // ẗ → T̈
        {0x1E98, {0x0057, 0x030A}},  // ẘ → Ẉ
        {0x1E99, {0x0059, 0x030A}},  // ẙ → Y̊
        {0x1E9A, {0x0041, 0x02BE}},  // ẚ → Aʾ
        {0x0587, {0x0535, 0x0552}},  // ﬓ → ԵՒ
        {0xFB00, {0x0046, 0x0046}},  // ﬀ → FF
        {0xFB01, {0x0046, 0x0049}},  // ﬁ → FI
        {0xFB02, {0x0046, 0x004C}},  // ﬂ → FL
        {0xFB03, {0x0046, 0x0046, 0x0049}},  // ﬃ → FFI
        {0xFB04, {0x0046, 0x0046, 0x004C}},  // ﬄ → FFL
        {0xFB05, {0x0053, 0x0054}},  // ﬅ → ST
        {0xFB06, {0x0053, 0x0054}},  // ﬆ → ST
        {0xFB13, {0x0544, 0x0546}},  // ﬓ → ՄՆ
        {0xFB14, {0x0544, 0x0535}},  // ﬔ → ՄԵ
        {0xFB15, {0x0544, 0x053B}},  // ﬕ → ՄԻ
        {0xFB16, {0x054E, 0x0546}},  // ﬖ → ՎՆ
        {0xFB17, {0x0544, 0x053D}},  // ﬗ → ՄԽ
        {0x1F50, {0x03A5, 0x0313}},  {0x1F52, {0x03A5, 0x0313, 0x0300}},
        {0x1F54, {0x03A5, 0x0313, 0x0301}},  {0x1F56, {0x03A5, 0x0313, 0x0342}},
        {0x1FB6, {0x0391, 0x0342}},  {0x1FC6, {0x0397, 0x0342}},
        {0x1FD2, {0x0399, 0x0308, 0x0300}},  {0x1FD3, {0x0399, 0x0308, 0x0301}},
        {0x1FD6, {0x0399, 0x0342}},  {0x1FD7, {0x0399, 0x0308, 0x0342}},
        {0x1FE2, {0x03A5, 0x0308, 0x0300}},  {0x1FE3, {0x03A5, 0x0308, 0x0301}},
        {0x1FE4, {0x03A1, 0x0313}},  {0x1FE6, {0x03A5, 0x0342}},
        {0x1FE7, {0x03A5, 0x0308, 0x0342}},  {0x1FF6, {0x03A9, 0x0342}},
        {0x1FB2, {0x1FBA, 0x0399}},  {0x1FB3, {0x0391, 0x0399}},
        {0x1FB4, {0x0386, 0x0399}},  {0x1FB7, {0x0391, 0x0342, 0x0399}},
        {0x1FBC, {0x0391, 0x0399}},  {0x1FC2, {0x1FCA, 0x0399}},
        {0x1FC3, {0x0397, 0x0399}},  {0x1FC4, {0x0389, 0x0399}},
        {0x1FC7, {0x0397, 0x0342, 0x0399}},  {0x1FCC, {0x0397, 0x0399}},
        {0x1FF2, {0x1FFA, 0x0399}},  {0x1FF3, {0x03A9, 0x0399}},
        {0x1FF4, {0x038F, 0x0399}},  {0x1FF7, {0x03A9, 0x0342, 0x0399}},
        {0x1FFC, {0x03A9, 0x0399}},
        {0x1F80, {0x1F08, 0x0399}},  {0x1F81, {0x1F09, 0x0399}},
        {0x1F82, {0x1F0A, 0x0399}},  {0x1F83, {0x1F0B, 0x0399}},
        {0x1F84, {0x1F0C, 0x0399}},  {0x1F85, {0x1F0D, 0x0399}},
        {0x1F86, {0x1F0E, 0x0399}},  {0x1F87, {0x1F0F, 0x0399}},
        {0x1F88, {0x1F08, 0x0399}},  {0x1F89, {0x1F09, 0x0399}},
        {0x1F8A, {0x1F0A, 0x0399}},  {0x1F8B, {0x1F0B, 0x0399}},
        {0x1F8C, {0x1F0C, 0x0399}},  {0x1F8D, {0x1F0D, 0x0399}},
        {0x1F8E, {0x1F0E, 0x0399}},  {0x1F8F, {0x1F0F, 0x0399}},
        {0x1F90, {0x1F28, 0x0399}},  {0x1F91, {0x1F29, 0x0399}},
        {0x1F92, {0x1F2A, 0x0399}},  {0x1F93, {0x1F2B, 0x0399}},
        {0x1F94, {0x1F2C, 0x0399}},  {0x1F95, {0x1F2D, 0x0399}},
        {0x1F96, {0x1F2E, 0x0399}},  {0x1F97, {0x1F2F, 0x0399}},
        {0x1F98, {0x1F28, 0x0399}},  {0x1F99, {0x1F29, 0x0399}},
        {0x1F9A, {0x1F2A, 0x0399}},  {0x1F9B, {0x1F2B, 0x0399}},
        {0x1F9C, {0x1F2C, 0x0399}},  {0x1F9D, {0x1F2D, 0x0399}},
        {0x1F9E, {0x1F2E, 0x0399}},  {0x1F9F, {0x1F2F, 0x0399}},
        {0x1FA0, {0x1F68, 0x0399}},  {0x1FA1, {0x1F69, 0x0399}},
        {0x1FA2, {0x1F6A, 0x0399}},  {0x1FA3, {0x1F6B, 0x0399}},
        {0x1FA4, {0x1F6C, 0x0399}},  {0x1FA5, {0x1F6D, 0x0399}},
        {0x1FA6, {0x1F6E, 0x0399}},  {0x1FA7, {0x1F6F, 0x0399}},
        {0x1FA8, {0x1F68, 0x0399}},  {0x1FA9, {0x1F69, 0x0399}},
        {0x1FAA, {0x1F6A, 0x0399}},  {0x1FAB, {0x1F6B, 0x0399}},
        {0x1FAC, {0x1F6C, 0x0399}},  {0x1FAD, {0x1F6D, 0x0399}},
        {0x1FAE, {0x1F6E, 0x0399}},  {0x1FAF, {0x1F6F, 0x0399}},
    };

    // Unicode-aware case conversion using utf8proc codepoint functions + SpecialCasing.txt table.
    auto unicode_case_convert = [](const std::string& s, bool to_lower) -> std::string {
        // Pre-decode all codepoints for context-sensitive rules (Final Sigma).
        std::vector<utf8proc_int32_t> cps;
        std::vector<size_t> offsets;
        { size_t j = 0; while (j < s.size()) { utf8proc_int32_t c2=0; utf8proc_ssize_t l2 = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t*>(s.c_str()+j), s.size()-j, &c2); if (l2<=0){j++;continue;} offsets.push_back(j); cps.push_back(c2); j+=l2; } offsets.push_back(s.size()); }
        auto is_cased = [](utf8proc_int32_t c) -> bool {
            utf8proc_category_t cat = utf8proc_category(c);
            return cat == UTF8PROC_CATEGORY_LU || cat == UTF8PROC_CATEGORY_LL || cat == UTF8PROC_CATEGORY_LT;
        };
        auto is_case_ignorable = [](utf8proc_int32_t c) -> bool {
            utf8proc_category_t cat = utf8proc_category(c);
            return cat == UTF8PROC_CATEGORY_MN || cat == UTF8PROC_CATEGORY_ME ||
                   cat == UTF8PROC_CATEGORY_CF || cat == UTF8PROC_CATEGORY_LM ||
                   cat == UTF8PROC_CATEGORY_SK || c == 0x0027 || c == 0x002E;
        };

        std::string result;
        size_t i = 0;
        size_t ci = 0;
        while (ci < cps.size()) {
            utf8proc_int32_t cp = cps[ci];
            size_t byte_start = offsets[ci];
            size_t byte_end = offsets[ci+1];
            utf8proc_ssize_t len = byte_end - byte_start;
            i = byte_end;
            // SpecialCasing: İ (U+0130) → i + combining dot above in toLowerCase
            if (to_lower && cp == 0x0130) { result += "\x69\xCC\x87"; ci++; continue; }
            // SpecialCasing: ẞ (U+1E9E) → ß (U+00DF) in toLowerCase
            if (to_lower && cp == 0x1E9E) { result += "\xC3\x9F"; ci++; continue; }
            // Final Sigma: Σ (03A3) → ς (03C2) if preceded by Cased (skip Case_Ignorable) and NOT followed by Cased.
            if (to_lower && cp == 0x03A3) {
                bool preceded_by_cased = false;
                for (int k = static_cast<int>(ci) - 1; k >= 0; k--) {
                    if (is_cased(cps[k])) { preceded_by_cased = true; break; }
                    if (!is_case_ignorable(cps[k])) break;
                }
                bool followed_by_cased = false;
                for (size_t k = ci + 1; k < cps.size(); k++) {
                    if (is_cased(cps[k])) { followed_by_cased = true; break; }
                    if (!is_case_ignorable(cps[k])) break;
                }
                if (preceded_by_cased && !followed_by_cased) {
                    result += "\xCF\x82"; ci++; continue;  // ς
                }
            }
            // SpecialCasing toUpperCase: multi-char mappings
            if (!to_lower) {
                auto it = special_upper.find(cp);
                if (it != special_upper.end()) {
                    for (utf8proc_int32_t c2 : it->second) {
                        utf8proc_uint8_t buf2[4];
                        utf8proc_ssize_t o2 = utf8proc_encode_char(c2, buf2);
                        if (o2 > 0) result.append(reinterpret_cast<char*>(buf2), o2);
                    }
                    ci++; continue;
                }
            }
            utf8proc_int32_t mapped = to_lower ? utf8proc_tolower(cp) : utf8proc_toupper(cp);
            utf8proc_uint8_t buf[4];
            utf8proc_ssize_t out = utf8proc_encode_char(mapped, buf);
            if (out > 0) result.append(reinterpret_cast<char*>(buf), out);
            else if (ci < offsets.size()-1) result.append(s, offsets[ci], offsets[ci+1]-offsets[ci]);
            ci++;
        }
        return result;
    };

    auto toLowerCase_fn = ObjectFactory::create_native_function("toLowerCase",
        [toString_helper, unicode_case_convert](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            std::string str = toString_helper(ctx, this_value);
            return Value(unicode_case_convert(str, true));
        });
    PropertyDescriptor toLowerCase_desc(Value(toLowerCase_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("toLowerCase", toLowerCase_desc);

    auto str_concat_fn = ObjectFactory::create_native_function("concat",
        [toString_helper, obj_to_string](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            std::string result = toString_helper(ctx, this_value);
            if (ctx.has_exception()) return Value();

            for (const auto& arg : args) {
                std::string s = obj_to_string(ctx, arg);
                if (ctx.has_exception()) return Value();
                result += s;
            }

            return Value(result);
        }, 1);
    PropertyDescriptor str_concat_desc(Value(str_concat_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("concat", str_concat_desc);

    auto toUpperCase_fn = ObjectFactory::create_native_function("toUpperCase",
        [toString_helper, unicode_case_convert](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            std::string str = toString_helper(ctx, this_value);
            return Value(unicode_case_convert(str, false));
        });
    PropertyDescriptor toUpperCase_desc(Value(toUpperCase_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("toUpperCase", toUpperCase_desc);

    auto toLocaleLowerCase_fn = ObjectFactory::create_native_function("toLocaleLowerCase",
        [toString_helper, unicode_case_convert](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            std::string str = toString_helper(ctx, this_value);
            return Value(unicode_case_convert(str, true));
        });
    PropertyDescriptor toLocaleLowerCase_desc(Value(toLocaleLowerCase_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("toLocaleLowerCase", toLocaleLowerCase_desc);

    auto toLocaleUpperCase_fn = ObjectFactory::create_native_function("toLocaleUpperCase",
        [toString_helper, unicode_case_convert](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            std::string str = toString_helper(ctx, this_value);
            return Value(unicode_case_convert(str, false));
        });
    PropertyDescriptor toLocaleUpperCase_desc(Value(toLocaleUpperCase_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("toLocaleUpperCase", toLocaleUpperCase_desc);

    auto str_slice_fn = ObjectFactory::create_native_function("slice",
        [toString_helper](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            std::string str = toString_helper(ctx, this_value);

            if (args.empty()) return Value(str);

            // Spec uses UTF-16 code unit positions, not byte positions.
            int utf16len = static_cast<int>(utf16_length(str));
            auto to_int_or_inf = [](double d, int len) -> int {
                if (std::isnan(d)) return 0;
                if (d == std::numeric_limits<double>::infinity()) return len;
                if (d == -std::numeric_limits<double>::infinity()) return 0;
                return static_cast<int>(std::min(std::max(d, static_cast<double>(INT_MIN)), static_cast<double>(INT_MAX)));
            };
            int start = to_int_or_inf(args[0].to_number(), utf16len);
            int end = (args.size() > 1 && !args[1].is_undefined())
                ? to_int_or_inf(args[1].to_number(), utf16len) : utf16len;

            if (start < 0) start = std::max(utf16len + start, 0);
            else start = std::min(start, utf16len);

            if (end < 0) end = std::max(utf16len + end, 0);
            else end = std::min(end, utf16len);

            if (start >= end) return Value(std::string(""));
            size_t byte_start = utf16_index_to_byte_pos(str, static_cast<size_t>(start));
            size_t byte_end = utf16_index_to_byte_pos(str, static_cast<size_t>(end));
            return Value(str.substr(byte_start, byte_end - byte_start));
        }, 2);
    PropertyDescriptor str_slice_desc(Value(str_slice_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("slice", str_slice_desc);

    // ES1: 15.5.4.7 String.prototype.lastIndexOf(searchString, position)
    auto str_lastIndexOf_fn = ObjectFactory::create_native_function("lastIndexOf",
        [toString_helper, obj_to_string](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            std::string str = toString_helper(ctx, this_value);

            if (args.empty()) {
                return Value(-1.0);
            }

            std::string search = obj_to_string(ctx, args[0]);
            if (ctx.has_exception()) return Value();
            size_t start = str.length();

            if (args.size() > 1 && !args[1].is_undefined()) {
                double pos = args[1].to_number();
                if (ctx.has_exception()) return Value();
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

            if (args.size() > 1 && !args[1].is_undefined()) {
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
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("String method called on null or undefined"); return Value(); }
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
            bool this_ok;
            std::string str = get_string_this(ctx, this_ok);
            if (!this_ok) return Value();
            return Value(utf16_is_well_formed(str));
        }, 0);
    PropertyDescriptor isWellFormed_desc(Value(isWellFormed_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("isWellFormed", isWellFormed_desc);

    auto toWellFormed_fn = ObjectFactory::create_native_function("toWellFormed",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            bool this_ok;
            std::string str = get_string_this(ctx, this_ok);
            if (!this_ok) return Value();
            return Value(utf16_to_well_formed(str));
        }, 0);
    PropertyDescriptor toWellFormed_desc(Value(toWellFormed_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("toWellFormed", toWellFormed_desc);

    auto repeat_fn = ObjectFactory::create_native_function("repeat",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            bool _ok; std::string str = get_string_this(ctx, _ok); if (!_ok) return Value();
            if (args.empty()) return Value(std::string(""));
            double count_d = args[0].to_number();
            if (ctx.has_exception()) return Value();
            if (std::isnan(count_d)) count_d = 0;
            count_d = std::floor(count_d);
            if (count_d < 0 || std::isinf(count_d)) { ctx.throw_range_error("Invalid count value"); return Value(); }
            int count = static_cast<int>(count_d);
            if (count == 0 || str.empty()) return Value(std::string(""));
            std::string result;
            result.reserve(str.length() * static_cast<size_t>(count));
            for (int i = 0; i < count; i++) result += str;
            return Value(result);
        }, 1);
    PropertyDescriptor repeat_desc(Value(repeat_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("repeat", repeat_desc);

    auto normalize_fn = ObjectFactory::create_native_function("normalize",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            bool this_ok;
            std::string str = get_string_this(ctx, this_ok);
            if (!this_ok) return Value();

            std::string form = "NFC";
            if (!args.empty() && !args[0].is_undefined()) {
                if (args[0].is_symbol()) { ctx.throw_type_error("Cannot convert a Symbol value to a string"); return Value(); }
                if (args[0].is_object() || args[0].is_function()) {
                    bool _f; form = string_this_coerce(ctx, args[0], _f);
                    if (ctx.has_exception()) return Value();
                } else {
                    form = args[0].to_string();
                }
            }

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
                ctx.throw_range_error("The normalization form should be one of NFC, NFD, NFKC, NFKD");
                return Value();
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
            bool this_ok;
            std::string str = get_string_this(ctx, this_ok);
            if (!this_ok) return Value();
            auto iterator = ObjectFactory::create_object();
            if (Iterator::s_string_iterator_prototype_) {
                iterator->set_prototype(Iterator::s_string_iterator_prototype_);
            }
            struct StringIterState { std::string str; size_t index = 0; };
            auto state = std::make_shared<StringIterState>(StringIterState{str, 0});
            auto next_fn = ObjectFactory::create_native_function("next",
                [state](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx; (void)args;
                    auto result = ObjectFactory::create_object();
                    if (state->index >= state->str.length()) {
                        result->set_property("done", Value(true));
                        result->set_property("value", Value());
                    } else {
                        unsigned char ch = static_cast<unsigned char>(state->str[state->index]);
                        size_t char_len = 1;
                        if (ch >= 0xF0) char_len = 4;
                        else if (ch >= 0xE0) char_len = 3;
                        else if (ch >= 0xC0) char_len = 2;
                        if (state->index + char_len > state->str.length()) char_len = 1;
                        std::string codepoint = state->str.substr(state->index, char_len);
                        result->set_property("done", Value(false));
                        result->set_property("value", Value(codepoint));
                        state->index += char_len;
                    }
                    return Value(result.release());
                }, 0);
            iterator->set_property("next", Value(next_fn.release()));
            return Value(iterator.release());
        }, 0);
    string_prototype->set_property("Symbol.iterator", Value(string_iterator_fn.release()), PropertyAttributes::BuiltinFunction);

    Object* proto_ptr = string_prototype.get();
    string_constructor->set_property("prototype", Value(string_prototype.release()), PropertyAttributes::None);
    proto_ptr->set_property("constructor", Value(string_constructor.get()), PropertyAttributes::BuiltinFunction);

    auto string_raw_fn = ObjectFactory::create_native_function("raw",
        [obj_to_string](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                ctx.throw_type_error("String.raw requires at least 1 argument");
                return Value();
            }

            // Per spec: template must be coercible to object (null/undefined throw TypeError)
            if (args[0].is_null() || args[0].is_undefined()) {
                ctx.throw_type_error("String.raw: template is null or undefined");
                return Value();
            }
            if (!args[0].is_object() && !args[0].is_function()) {
                // Non-object primitive: wrap (per spec ToObject coercion, but skip for now)
                return Value(std::string(""));
            }
            Object* template_obj = args[0].is_object() ? args[0].as_object()
                : static_cast<Object*>(args[0].as_function());
            // Get raw array via get_property (fires get("raw") trap on Proxy)
            Value raw_val = template_obj->get_property("raw");
            if (ctx.has_exception()) return Value();
            if (!raw_val.is_object() && !raw_val.is_function()) {
                ctx.throw_type_error("String.raw: template.raw is not an object");
                return Value();
            }
            Object* raw_obj = raw_val.is_object() ? raw_val.as_object() : static_cast<Object*>(raw_val.as_function());
            // Get length via get_property
            Value len_val = raw_obj->get_property("length");
            if (ctx.has_exception()) return Value();
            if (len_val.is_symbol() || len_val.is_bigint()) {
                ctx.throw_type_error("String.raw: template.raw.length is a Symbol");
                return Value();
            }
            double length_d = len_val.is_number() ? len_val.to_number() : 0.0;
            if (length_d <= 0 || std::isnan(length_d)) return Value(std::string(""));
            uint32_t length = static_cast<uint32_t>(std::min(length_d, static_cast<double>(0xFFFFFFFFu)));
            if (ctx.has_exception()) return Value();
            std::string result;
            for (uint32_t i = 0; i < length; i++) {
                Value chunk = raw_obj->get_property(std::to_string(i));
                if (ctx.has_exception()) return Value();
                std::string seg = obj_to_string(ctx, chunk);
                if (ctx.has_exception()) return Value();
                result += seg;
                if (i + 1 == length) break;
                if (i + 1 < static_cast<uint32_t>(args.size())) {
                    std::string sub = obj_to_string(ctx, args[i + 1]);
                    if (ctx.has_exception()) return Value();
                    result += sub;
                }
            }
            return Value(result);
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
                if (ctx.has_exception()) return Value();
                if (std::isnan(num) || num < 0 || num > 0x10FFFF || num != std::floor(num)) {
                    ctx.throw_range_error("Invalid code point");
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

    Object* global_object = ctx.get_global_object();
    Value global_string = global_object ? global_object->get_property("String") : Value();
    if (global_string.is_function()) {
        Object* global_string_obj = global_string.as_function();
        Value prototype_val = global_string_obj->get_property("prototype");
        if (prototype_val.is_object()) {
            Object* global_prototype = prototype_val.as_object();

            auto global_includes_fn = ObjectFactory::create_native_function("includes",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    bool _ok; std::string str = get_string_this(ctx, _ok); if (!_ok) return Value();
                    if (args.empty()) return Value(false);
                    // ES6: throw TypeError if argument is a regexp (has Symbol.match truthy)
                    if (args[0].is_object() || args[0].is_function()) {
                        Object* arg_obj = args[0].is_function()
                            ? static_cast<Object*>(args[0].as_function())
                            : args[0].as_object();
                        Value sym_match = arg_obj->get_property("Symbol.match");
                        if (ctx.has_exception()) return Value();
                        if (sym_match.is_undefined()) {
                            if (arg_obj->get_type() == Object::ObjectType::RegExp) {
                                ctx.throw_type_error("First argument to String.prototype.includes must not be a regular expression");
                                return Value();
                            }
                        } else if (sym_match.to_boolean()) {
                            ctx.throw_type_error("First argument to String.prototype.includes must not be a regular expression");
                            return Value();
                        }
                    }
                    if (args[0].is_symbol()) {
                        ctx.throw_type_error("Cannot convert a Symbol value to a string");
                        return Value();
                    }
                    // Convert argument to string (call toString() for objects)
                    std::string search_string;
                    if (args[0].is_object() || args[0].is_function()) {
                        Object* obj = args[0].is_function()
                            ? static_cast<Object*>(args[0].as_function())
                            : args[0].as_object();
                        Value ts = obj->get_property("toString");
                        if (ts.is_function()) {
                            Value r = ts.as_function()->call(ctx, {}, args[0]);
                            if (!ctx.has_exception() && r.is_string()) search_string = r.to_string();
                            else search_string = args[0].to_string();
                        } else search_string = args[0].to_string();
                    } else search_string = args[0].to_string();
                    size_t position = 0;
                    if (args.size() > 1) {
                        if (args[1].is_symbol()) {
                            ctx.throw_type_error("Cannot convert a Symbol value to a number");
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
            PropertyDescriptor global_includes_length_desc(Value(1.0), PropertyAttributes::Configurable);
            global_includes_length_desc.set_enumerable(false);
            global_includes_length_desc.set_writable(false);
            global_includes_fn->set_property_descriptor("length", global_includes_length_desc);
            global_prototype->set_property("includes", Value(global_includes_fn.release()), PropertyAttributes::BuiltinFunction);

            auto string_valueOf_fn = ObjectFactory::create_native_function("valueOf",
                [](Context& ctx, const std::vector<Value>&) -> Value {
                    Value this_val = ctx.get_binding("this");
                    if (this_val.is_string()) return this_val;
                    if (this_val.is_object() || this_val.is_function()) {
                        Object* obj = this_val.is_function() ? static_cast<Object*>(this_val.as_function()) : this_val.as_object();
                        Value pv = obj->get_property("[[PrimitiveValue]]");
                        if (!pv.is_undefined() && pv.is_string()) return pv;
                    }
                    ctx.throw_type_error("String.prototype.valueOf requires a string or String object");
                    return Value();
                });

            PropertyDescriptor string_valueOf_length_desc(Value(0.0), PropertyAttributes::Configurable);
            string_valueOf_length_desc.set_enumerable(false);
            string_valueOf_length_desc.set_writable(false);
            string_valueOf_fn->set_property_descriptor("length", string_valueOf_length_desc);

            PropertyDescriptor string_valueOf_name_desc(Value(std::string("valueOf")), PropertyAttributes::None);
            string_valueOf_name_desc.set_configurable(true);
            string_valueOf_name_desc.set_enumerable(false);
            string_valueOf_name_desc.set_writable(false);
            string_valueOf_fn->set_property_descriptor("name", string_valueOf_name_desc);

            global_prototype->set_property("valueOf", Value(string_valueOf_fn.release()), PropertyAttributes::BuiltinFunction);

            auto string_toString_fn = ObjectFactory::create_native_function("toString",
                [](Context& ctx, const std::vector<Value>&) -> Value {
                    Value this_val = ctx.get_binding("this");
                    if (this_val.is_string()) return this_val;
                    if (this_val.is_object() || this_val.is_function()) {
                        Object* obj = this_val.is_function() ? static_cast<Object*>(this_val.as_function()) : this_val.as_object();
                        Value pv = obj->get_property("[[PrimitiveValue]]");
                        if (!pv.is_undefined() && pv.is_string()) return pv;
                    }
                    ctx.throw_type_error("String.prototype.toString requires a string or String object");
                    return Value();
                });

            PropertyDescriptor string_toString_length_desc(Value(0.0), PropertyAttributes::Configurable);
            string_toString_length_desc.set_enumerable(false);
            string_toString_length_desc.set_writable(false);
            string_toString_fn->set_property_descriptor("length", string_toString_length_desc);

            PropertyDescriptor string_toString_name_desc(Value(std::string("toString")), PropertyAttributes::None);
            string_toString_name_desc.set_configurable(true);
            string_toString_name_desc.set_enumerable(false);
            string_toString_name_desc.set_writable(false);
            string_toString_fn->set_property_descriptor("name", string_toString_name_desc);

            global_prototype->set_property("toString", Value(string_toString_fn.release()), PropertyAttributes::BuiltinFunction);

            auto string_trim_fn = ObjectFactory::create_native_function("trim",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)args;
                    bool _ok; std::string str = get_string_this(ctx, _ok); if (!_ok) return Value();
                    return Value(unicode_trim(str));
                });
            global_prototype->set_property("trim", Value(string_trim_fn.release()), PropertyAttributes::BuiltinFunction);

            auto string_trimStart_fn = ObjectFactory::create_native_function("trimStart",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)args;
                    bool _ok; std::string str = get_string_this(ctx, _ok); if (!_ok) return Value();
                    size_t start = 0;
                    while (start < str.size()) { size_t n = is_unicode_whitespace(str, start); if (!n) break; start += n; }
                    return Value(str.substr(start));
                });
            Function* trimStart_raw = string_trimStart_fn.get();
            global_prototype->set_property("trimStart", Value(string_trimStart_fn.release()), PropertyAttributes::BuiltinFunction);
            global_prototype->set_property("trimLeft", Value(trimStart_raw), PropertyAttributes::BuiltinFunction);

            auto string_trimEnd_fn = ObjectFactory::create_native_function("trimEnd",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)args;
                    bool _ok; std::string str = get_string_this(ctx, _ok); if (!_ok) return Value();
                    size_t end = str.size();
                    while (end > 0) {
                        size_t p = end - 1;
                        while (p > 0 && (static_cast<unsigned char>(str[p]) & 0xC0) == 0x80) p--;
                        size_t n = is_unicode_whitespace(str, p);
                        if (!n || p + n != end) break;
                        end = p;
                    }
                    return Value(str.substr(0, end));
                });
            Function* trimEnd_raw = string_trimEnd_fn.get();
            global_prototype->set_property("trimEnd", Value(string_trimEnd_fn.release()), PropertyAttributes::BuiltinFunction);
            global_prototype->set_property("trimRight", Value(trimEnd_raw), PropertyAttributes::BuiltinFunction);

        }
    }

}

} // namespace Quanta
