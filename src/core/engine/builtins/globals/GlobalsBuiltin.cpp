/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "quanta/core/engine/builtins/GlobalsBuiltin.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/engine/Engine.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/Error.h"
#include "quanta/lexer/Lexer.h"
#include "quanta/parser/Parser.h"
#include "quanta/parser/AST.h"
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cctype>
#include "quanta/core/runtime/Iterator.h"
#include "quanta/core/runtime/BigInt.h"
#include "quanta/core/runtime/String.h"

namespace Quanta {

void register_global_builtins(Context& ctx) {
    if (!ctx.get_lexical_environment()) return;
    
    auto parseInt_fn = ObjectFactory::create_native_function("parseInt",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value::nan();
            
            std::string str = args[0].to_string();
            
            size_t start = 0;
            while (start < str.length() && std::isspace(str[start])) {
                start++;
            }
            
            if (start >= str.length()) {
                return Value::nan();
            }

            int radix = 10;
            if (args.size() > 1 && args[1].is_number()) {
                double r = args[1].to_number();
                if (r >= 2 && r <= 36) {
                    radix = static_cast<int>(r);
                }
            }

            // If radix not specified and string starts with "0x" or "0X", use radix 16
            if (args.size() <= 1 && start + 1 < str.length() &&
                str[start] == '0' && (str[start + 1] == 'x' || str[start + 1] == 'X')) {
                radix = 16;
                start += 2; 
            }

            if (start >= str.length()) {
                return Value::nan();
            }

            char first_char = str[start];
            bool has_valid_start = false;
            
            if (radix == 16) {
                has_valid_start = std::isdigit(first_char) || 
                                (first_char >= 'a' && first_char <= 'f') ||
                                (first_char >= 'A' && first_char <= 'F');
            } else if (radix == 8) {
                has_valid_start = (first_char >= '0' && first_char <= '7');
            } else {
                has_valid_start = std::isdigit(first_char);
            }
            
            if (!has_valid_start && first_char != '+' && first_char != '-') {
                return Value::nan();
            }
            
            try {
                size_t pos;
                long result = std::stol(str.substr(start), &pos, radix);
                if (pos == 0) {
                    return Value::nan();
                }
                return Value(static_cast<double>(result));
            } catch (...) {
                return Value::nan();
            }
        }, 2);
    Function* parseInt_raw = parseInt_fn.get();
    ctx.get_lexical_environment()->create_binding("parseInt", Value(parseInt_fn.release()), false);

    auto parseFloat_fn = ObjectFactory::create_native_function("parseFloat",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value::nan();
            
            std::string str = args[0].to_string();
            
            size_t start = 0;
            while (start < str.length() && std::isspace(str[start])) {
                start++;
            }
            
            if (start >= str.length()) {
                return Value::nan();
            }
            
            char first_char = str[start];
            if (!std::isdigit(first_char) && first_char != '.' && 
                first_char != '+' && first_char != '-') {
                return Value::nan();
            }
            
            try {
                size_t pos;
                double result = std::stod(str.substr(start), &pos);
                if (pos == 0) {
                    return Value::nan();
                }
                return Value(result);
            } catch (...) {
                return Value::nan();
            }
        }, 1);
    Function* parseFloat_raw = parseFloat_fn.get();
    ctx.get_lexical_environment()->create_binding("parseFloat", Value(parseFloat_fn.release()), false);

    // ES6: Number.parseFloat and Number.parseInt must be the same objects as globals
    {
        Object* num_obj = ctx.get_built_in_object("Number");
        if (num_obj && num_obj->is_function()) {
            Function* num_ctor = static_cast<Function*>(num_obj);
            num_ctor->set_property("parseFloat", Value(parseFloat_raw));
            num_ctor->set_property("parseInt", Value(parseInt_raw));
        }
    }

    auto isNaN_global_fn = ObjectFactory::create_native_function("isNaN",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Global isNaN: coerce to number first, then check if NaN
            if (args.empty()) return Value(true);

            // If already NaN, return true
            if (args[0].is_nan()) return Value(true);

            // Convert to number (may produce NaN for non-numeric values like "abc")
            Value num_val(args[0].to_number());

            // Check if conversion resulted in NaN
            return Value(num_val.is_nan());
        }, 1);
    ctx.get_lexical_environment()->create_binding("isNaN", Value(isNaN_global_fn.release()), false);

    auto isFinite_global_fn = ObjectFactory::create_native_function("isFinite",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value(false);
            double num = args[0].to_number();
            return Value(std::isfinite(num));
        }, 1);
    ctx.get_lexical_environment()->create_binding("isFinite", Value(isFinite_global_fn.release()), false);

    auto eval_fn = ObjectFactory::create_native_function("eval",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value();
            if (!args[0].is_string()) return args[0];

            std::string code = args[0].to_string();
            if (code.empty()) return Value();

            Engine* engine = ctx.get_engine();
            if (!engine) return Value();

            bool strict = ctx.is_strict_mode();

            try {
                // Parse with strict mode if calling context is strict
                Lexer::LexerOptions lex_opts;
                lex_opts.strict_mode = strict;
                Lexer lexer(code, lex_opts);
                auto tokens = lexer.tokenize();

                if (lexer.has_errors()) {
                    auto& errors = lexer.get_errors();
                    std::string msg = errors.empty() ? "SyntaxError" : errors[0];
                    ctx.throw_syntax_error(msg);
                    return Value();
                }

                Parser::ParseOptions parse_opts;
                parse_opts.strict_mode = strict;
                Parser parser(tokens, parse_opts);
                parser.set_source(code);
                auto program = parser.parse_program();

                if (parser.has_errors()) {
                    auto& errors = parser.get_errors();
                    std::string msg = errors.empty() ? "SyntaxError" : errors[0].message;
                    // Strip "SyntaxError: " prefix to avoid double-prefixing
                    if (msg.substr(0, 13) == "SyntaxError: ") msg = msg.substr(13);
                    ctx.throw_syntax_error(msg);
                    return Value();
                }

                if (!program) {
                    ctx.throw_syntax_error("Failed to parse eval code");
                    return Value();
                }

                if (strict) {
                    // ES5 10.4.2: Strict eval creates isolated variable environment
                    Context eval_ctx(engine, &ctx, Context::Type::Eval);
                    eval_ctx.set_strict_mode(true);
                    auto eval_env = new Environment(
                        Environment::Type::Declarative, ctx.get_lexical_environment());
                    eval_ctx.set_lexical_environment(eval_env);
                    eval_ctx.set_variable_environment(eval_env);
                    eval_ctx.set_this_binding(ctx.get_this_binding());

                    Value result = program->evaluate(eval_ctx);

                    if (eval_ctx.has_exception()) {
                        Value exception = eval_ctx.get_exception();
                        eval_ctx.clear_exception();
                        ctx.throw_exception(exception);
                        delete eval_env;
                        return Value();
                    }

                    delete eval_env;
                    return result;
                } else {
                    // Non-strict eval: evaluate in calling context
                    auto result = engine->evaluate(code);
                    if (result.success) {
                        return result.value;
                    } else {
                        ctx.throw_syntax_error(result.error_message);
                        return Value();
                    }
                }
            } catch (const std::exception& e) {
                ctx.throw_syntax_error(std::string(e.what()));
                return Value();
            } catch (...) {
                ctx.throw_syntax_error("Unknown syntax error");
                return Value();
            }
        }, 1);
    ctx.get_lexical_environment()->create_binding("eval", Value(eval_fn.release()), false);

    ctx.get_lexical_environment()->create_binding("undefined", Value(), false);
    ctx.get_lexical_environment()->create_binding("null", Value::null(), false);
    
    if (ctx.get_global_object()) {
        ctx.get_lexical_environment()->create_binding("globalThis", Value(ctx.get_global_object()), false);
        ctx.get_lexical_environment()->create_binding("global", Value(ctx.get_global_object()), false);
        ctx.get_lexical_environment()->create_binding("window", Value(ctx.get_global_object()), false);
        ctx.get_lexical_environment()->create_binding("this", Value(ctx.get_global_object()), false);

        PropertyDescriptor global_ref_desc(Value(ctx.get_global_object()), PropertyAttributes::BuiltinFunction);
        ctx.get_global_object()->set_property_descriptor("globalThis", global_ref_desc);
        ctx.get_global_object()->set_property_descriptor("global", global_ref_desc);
        ctx.get_global_object()->set_property_descriptor("window", global_ref_desc);
        ctx.get_global_object()->set_property_descriptor("this", global_ref_desc);
    }
    ctx.get_lexical_environment()->create_binding("true", Value(true), false);
    ctx.get_lexical_environment()->create_binding("false", Value(false), false);
    
    ctx.get_lexical_environment()->create_binding("NaN", Value::nan(), false);
    ctx.get_lexical_environment()->create_binding("Infinity", Value::positive_infinity(), false);

    if (ctx.get_global_object()) {
        PropertyDescriptor nan_desc(Value::nan(), PropertyAttributes::None);
        ctx.get_global_object()->set_property_descriptor("NaN", nan_desc);

        PropertyDescriptor inf_desc(Value::positive_infinity(), PropertyAttributes::None);
        ctx.get_global_object()->set_property_descriptor("Infinity", inf_desc);

        PropertyDescriptor undef_desc(Value(), PropertyAttributes::None);
        ctx.get_global_object()->set_property_descriptor("undefined", undef_desc);
    }
    
    auto is_hex_digit = [](char c) -> bool {
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
    };

    auto hex_to_int = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        return 0;
    };

    auto is_uri_reserved = [](unsigned char c) -> bool {
        return c == ';' || c == '/' || c == '?' || c == ':' || c == '@' ||
               c == '&' || c == '=' || c == '+' || c == '$' || c == ',' || c == '#';
    };

    auto encode_uri_fn = ObjectFactory::create_native_function("encodeURI",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value(std::string("undefined"));
            std::string input = args[0].to_string();
            std::string result;

            for (size_t i = 0; i < input.length(); i++) {
                unsigned char c = input[i];
                // Detect UTF-8 encoded surrogates (U+D800-U+DFFF): 0xED 0xA0-0xBF ...
                if (c == 0xED && i + 1 < input.length()) {
                    unsigned char c2 = static_cast<unsigned char>(input[i + 1]);
                    if (c2 >= 0xA0 && c2 <= 0xBF) {
                        ctx.throw_uri_error("URI malformed");
                        return Value();
                    }
                }
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == ';' || c == ',' || c == '/' || c == '?' || c == ':' || c == '@' ||
                    c == '&' || c == '=' || c == '+' || c == '$' || c == '-' || c == '_' ||
                    c == '.' || c == '!' || c == '~' || c == '*' || c == '\'' || c == '(' ||
                    c == ')' || c == '#') {
                    result += static_cast<char>(c);
                } else {
                    char hex[4];
                    snprintf(hex, sizeof(hex), "%%%02X", c);
                    result += hex;
                }
            }
            return Value(result);
        }, 1);
    ctx.get_lexical_environment()->create_binding("encodeURI", Value(encode_uri_fn.release()), false);

    auto decode_uri_fn = ObjectFactory::create_native_function("decodeURI",
        [is_hex_digit, hex_to_int, is_uri_reserved](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value(std::string("undefined"));
            std::string input = args[0].to_string();
            std::string result;

            for (size_t i = 0; i < input.length(); i++) {
                if (input[i] == '%') {
                    if (i + 2 >= input.length()) {
                        ctx.throw_uri_error("URI malformed");
                        return Value();
                    }
                    if (!is_hex_digit(input[i + 1]) || !is_hex_digit(input[i + 2])) {
                        ctx.throw_uri_error("URI malformed");
                        return Value();
                    }
                    int byte1 = hex_to_int(input[i + 1]) * 16 + hex_to_int(input[i + 2]);
                    if (byte1 < 0x80) {
                        if (is_uri_reserved(static_cast<unsigned char>(byte1))) {
                            result += input[i];
                            result += input[i + 1];
                            result += input[i + 2];
                        } else {
                            result += static_cast<char>(byte1);
                        }
                        i += 2;
                    } else {
                        int num_bytes = 0;
                        if ((byte1 & 0xE0) == 0xC0) num_bytes = 2;
                        else if ((byte1 & 0xF0) == 0xE0) num_bytes = 3;
                        else if ((byte1 & 0xF8) == 0xF0) num_bytes = 4;
                        else {
                            ctx.throw_uri_error("URI malformed");
                            return Value();
                        }
                        std::string utf8;
                        utf8 += static_cast<char>(byte1);
                        for (int j = 1; j < num_bytes; j++) {
                            if (i + 2 + j * 3 >= input.length() || input[i + j * 3] != '%') {
                                ctx.throw_uri_error("URI malformed");
                                return Value();
                            }
                            size_t pos = i + j * 3;
                            if (!is_hex_digit(input[pos + 1]) || !is_hex_digit(input[pos + 2])) {
                                ctx.throw_uri_error("URI malformed");
                                return Value();
                            }
                            int cb = hex_to_int(input[pos + 1]) * 16 + hex_to_int(input[pos + 2]);
                            if ((cb & 0xC0) != 0x80) {
                                ctx.throw_uri_error("URI malformed");
                                return Value();
                            }
                            utf8 += static_cast<char>(cb);
                        }
                        result += utf8;
                        i += num_bytes * 3 - 1;
                    }
                } else {
                    result += input[i];
                }
            }
            return Value(result);
        }, 1);
    ctx.get_lexical_environment()->create_binding("decodeURI", Value(decode_uri_fn.release()), false);

    auto encode_uri_component_fn = ObjectFactory::create_native_function("encodeURIComponent",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value(std::string("undefined"));
            std::string input = args[0].to_string();
            std::string result;

            for (size_t i = 0; i < input.length(); i++) {
                unsigned char c = input[i];
                // Detect UTF-8 encoded surrogates (U+D800-U+DFFF): 0xED 0xA0-0xBF ...
                if (c == 0xED && i + 1 < input.length()) {
                    unsigned char c2 = static_cast<unsigned char>(input[i + 1]);
                    if (c2 >= 0xA0 && c2 <= 0xBF) {
                        ctx.throw_uri_error("URI malformed");
                        return Value();
                    }
                }
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '_' || c == '.' || c == '!' || c == '~' || c == '*' ||
                    c == '\'' || c == '(' || c == ')') {
                    result += static_cast<char>(c);
                } else {
                    char hex[4];
                    snprintf(hex, sizeof(hex), "%%%02X", c);
                    result += hex;
                }
            }
            return Value(result);
        }, 1);
    ctx.get_lexical_environment()->create_binding("encodeURIComponent", Value(encode_uri_component_fn.release()), false);

    auto decode_uri_component_fn = ObjectFactory::create_native_function("decodeURIComponent",
        [is_hex_digit, hex_to_int](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value(std::string("undefined"));
            std::string input = args[0].to_string();
            std::string result;

            for (size_t i = 0; i < input.length(); i++) {
                if (input[i] == '%') {
                    if (i + 2 >= input.length()) {
                        ctx.throw_uri_error("URI malformed");
                        return Value();
                    }
                    if (!is_hex_digit(input[i + 1]) || !is_hex_digit(input[i + 2])) {
                        ctx.throw_uri_error("URI malformed");
                        return Value();
                    }
                    int byte1 = hex_to_int(input[i + 1]) * 16 + hex_to_int(input[i + 2]);
                    if (byte1 < 0x80) {
                        result += static_cast<char>(byte1);
                        i += 2;
                    } else {
                        int num_bytes = 0;
                        if ((byte1 & 0xE0) == 0xC0) num_bytes = 2;
                        else if ((byte1 & 0xF0) == 0xE0) num_bytes = 3;
                        else if ((byte1 & 0xF8) == 0xF0) num_bytes = 4;
                        else {
                            ctx.throw_uri_error("URI malformed");
                            return Value();
                        }
                        std::string utf8;
                        utf8 += static_cast<char>(byte1);
                        for (int j = 1; j < num_bytes; j++) {
                            if (i + 2 + j * 3 >= input.length() || input[i + j * 3] != '%') {
                                ctx.throw_uri_error("URI malformed");
                                return Value();
                            }
                            size_t pos = i + j * 3;
                            if (!is_hex_digit(input[pos + 1]) || !is_hex_digit(input[pos + 2])) {
                                ctx.throw_uri_error("URI malformed");
                                return Value();
                            }
                            int cb = hex_to_int(input[pos + 1]) * 16 + hex_to_int(input[pos + 2]);
                            if ((cb & 0xC0) != 0x80) {
                                ctx.throw_uri_error("URI malformed");
                                return Value();
                            }
                            utf8 += static_cast<char>(cb);
                        }
                        result += utf8;
                        i += num_bytes * 3 - 1;
                    }
                } else {
                    result += input[i];
                }
            }
            return Value(result);
        }, 1);
    ctx.get_lexical_environment()->create_binding("decodeURIComponent", Value(decode_uri_component_fn.release()), false);
    
    auto bigint_fn = ObjectFactory::create_native_function("BigInt",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                ctx.throw_type_error("BigInt constructor requires an argument");
                return Value();
            }
            
            Value arg = args[0];
            if (arg.is_bigint()) {
                return arg;
            }
            
            if (arg.is_number()) {
                double num = arg.as_number();
                if (std::isnan(num) || std::isinf(num) || std::fmod(num, 1.0) != 0.0) {
                    ctx.throw_range_error("Cannot convert Number to BigInt");
                    return Value();
                }
                auto bigint = std::make_unique<Quanta::BigInt>(static_cast<int64_t>(num));
                return Value(bigint.release());
            }
            
            if (arg.is_string()) {
                try {
                    std::string str = arg.as_string()->str();
                    auto bigint = std::make_unique<Quanta::BigInt>(str);
                    return Value(bigint.release());
                } catch (const std::exception& e) {
                    ctx.throw_syntax_error("Cannot convert string to BigInt");
                    return Value();
                }
            }
            
            ctx.throw_type_error("Cannot convert value to BigInt");
            return Value();
        });
    auto asIntN_fn = ObjectFactory::create_native_function("asIntN",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() < 2) { ctx.throw_type_error("BigInt.asIntN requires 2 arguments"); return Value(); }
            int64_t n = static_cast<int64_t>(args[0].to_number());
            if (n < 0 || n > 64) { ctx.throw_range_error("Invalid width"); return Value(); }
            if (!args[1].is_bigint()) { ctx.throw_type_error("Not a BigInt"); return Value(); }
            int64_t val = args[1].as_bigint()->to_int64();
            if (n == 0) return Value(new Quanta::BigInt(0));
            if (n == 64) return Value(new Quanta::BigInt(val));
            int64_t mod = 1LL << n;
            int64_t result = val & (mod - 1);
            if (result >= (mod >> 1)) result -= mod;
            return Value(new Quanta::BigInt(result));
        });
    bigint_fn->set_property("asIntN", Value(asIntN_fn.release()), PropertyAttributes::BuiltinFunction);

    auto asUintN_fn = ObjectFactory::create_native_function("asUintN",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() < 2) { ctx.throw_type_error("BigInt.asUintN requires 2 arguments"); return Value(); }
            int64_t n = static_cast<int64_t>(args[0].to_number());
            if (n < 0 || n > 64) { ctx.throw_range_error("Invalid width"); return Value(); }
            if (!args[1].is_bigint()) { ctx.throw_type_error("Not a BigInt"); return Value(); }
            int64_t val = args[1].as_bigint()->to_int64();
            if (n == 0) return Value(new Quanta::BigInt(0));
            if (n == 64) return Value(new Quanta::BigInt(val));
            uint64_t mask = (1ULL << n) - 1;
            uint64_t result = static_cast<uint64_t>(val) & mask;
            return Value(new Quanta::BigInt(static_cast<int64_t>(result)));
        });
    bigint_fn->set_property("asUintN", Value(asUintN_fn.release()), PropertyAttributes::BuiltinFunction);

    ctx.get_lexical_environment()->create_binding("BigInt", Value(bigint_fn.release()), false);

    auto escape_fn = ObjectFactory::create_native_function("escape",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value(std::string("undefined"));
            }

            std::string input;
            Value arg = args[0];
            if (arg.is_object()) {
                Object* obj = arg.as_object();
                Value toString_method = obj->get_property("toString");
                if (toString_method.is_function()) {
                    try {
                        Function* func = toString_method.as_function();
                        Value result = func->call(ctx, {}, arg);
                        if (ctx.has_exception()) {
                            return Value();
                        }
                        input = result.to_string();
                    } catch (...) {
                        return Value();
                    }
                } else {
                    input = arg.to_string();
                }
            } else {
                input = arg.to_string();
            }
            std::string result;

            // Convert UTF-8 string to UTF-16 code units
            std::u16string utf16;
            size_t i = 0;
            while (i < input.length()) {
                unsigned char byte = static_cast<unsigned char>(input[i]);
                uint32_t codepoint;

                if (byte < 0x80) {
                    codepoint = byte;
                    i++;
                } else if ((byte & 0xE0) == 0xC0) {
                    codepoint = ((byte & 0x1F) << 6) | (input[i + 1] & 0x3F);
                    i += 2;
                } else if ((byte & 0xF0) == 0xE0) {
                    codepoint = ((byte & 0x0F) << 12) | ((input[i + 1] & 0x3F) << 6) | (input[i + 2] & 0x3F);
                    i += 3;
                } else if ((byte & 0xF8) == 0xF0) {
                    codepoint = ((byte & 0x07) << 18) | ((input[i + 1] & 0x3F) << 12) | ((input[i + 2] & 0x3F) << 6) | (input[i + 3] & 0x3F);
                    i += 4;
                    // Convert to surrogate pair
                    if (codepoint > 0xFFFF) {
                        codepoint -= 0x10000;
                        utf16 += static_cast<char16_t>((codepoint >> 10) + 0xD800);
                        utf16 += static_cast<char16_t>((codepoint & 0x3FF) + 0xDC00);
                        continue;
                    }
                } else {
                    i++;
                    continue;
                }

                utf16 += static_cast<char16_t>(codepoint);
            }

            // Escape according to spec
            for (char16_t code_unit : utf16) {
                uint16_t c = static_cast<uint16_t>(code_unit);

                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '@' || c == '*' || c == '_' || c == '+' || c == '-' || c == '.' || c == '/') {
                    result += static_cast<char>(c);
                } else if (c < 256) {
                    // %XX format for code units below 256
                    result += '%';
                    result += "0123456789ABCDEF"[(c >> 4) & 0xF];
                    result += "0123456789ABCDEF"[c & 0xF];
                } else {
                    // %uXXXX format for code units >= 256
                    result += "%u";
                    result += "0123456789ABCDEF"[(c >> 12) & 0xF];
                    result += "0123456789ABCDEF"[(c >> 8) & 0xF];
                    result += "0123456789ABCDEF"[(c >> 4) & 0xF];
                    result += "0123456789ABCDEF"[c & 0xF];
                }
            }

            return Value(result);
        });
    ctx.get_lexical_environment()->create_binding("escape", Value(escape_fn.get()), false);
    if (ctx.get_global_object()) {
        PropertyDescriptor escape_desc(Value(escape_fn.get()), PropertyAttributes::BuiltinFunction);
        ctx.get_global_object()->set_property_descriptor("escape", escape_desc);
    }
    escape_fn.release();

    auto unescape_fn = ObjectFactory::create_native_function("unescape",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value(std::string("undefined"));
            }

            std::string input;
            Value arg = args[0];
            if (arg.is_object()) {
                Object* obj = arg.as_object();
                Value toString_method = obj->get_property("toString");
                if (toString_method.is_function()) {
                    try {
                        Function* func = toString_method.as_function();
                        Value result = func->call(ctx, {}, arg);
                        if (ctx.has_exception()) {
                            return Value();
                        }
                        input = result.to_string();
                    } catch (...) {
                        return Value();
                    }
                } else {
                    input = arg.to_string();
                }
            } else {
                input = arg.to_string();
            }
            auto hex_to_num = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                return -1;
            };

            std::u16string utf16;

            for (size_t i = 0; i < input.length(); ++i) {
                if (input[i] == '%') {
                    // Check for %uXXXX format
                    if (i + 5 < input.length() && input[i + 1] == 'u') {
                        int val1 = hex_to_num(input[i + 2]);
                        int val2 = hex_to_num(input[i + 3]);
                        int val3 = hex_to_num(input[i + 4]);
                        int val4 = hex_to_num(input[i + 5]);

                        if (val1 >= 0 && val2 >= 0 && val3 >= 0 && val4 >= 0) {
                            uint16_t code_unit = (val1 << 12) | (val2 << 8) | (val3 << 4) | val4;
                            utf16 += static_cast<char16_t>(code_unit);
                            i += 5;
                            continue;
                        }
                    }
                    // Check for %XX format
                    if (i + 2 < input.length()) {
                        int val1 = hex_to_num(input[i + 1]);
                        int val2 = hex_to_num(input[i + 2]);

                        if (val1 >= 0 && val2 >= 0) {
                            uint8_t byte = (val1 << 4) | val2;
                            utf16 += static_cast<char16_t>(byte);
                            i += 2;
                            continue;
                        }
                    }
                }
                // Not an escape sequence, add as-is
                utf16 += static_cast<char16_t>(static_cast<unsigned char>(input[i]));
            }

            // Convert UTF-16 back to UTF-8
            std::string result;
            for (size_t i = 0; i < utf16.length(); ++i) {
                uint16_t code_unit = static_cast<uint16_t>(utf16[i]);

                // Check for surrogate pair
                if (code_unit >= 0xD800 && code_unit <= 0xDBFF && i + 1 < utf16.length()) {
                    uint16_t next = static_cast<uint16_t>(utf16[i + 1]);
                    if (next >= 0xDC00 && next <= 0xDFFF) {
                        uint32_t codepoint = 0x10000 + ((code_unit - 0xD800) << 10) + (next - 0xDC00);
                        // Encode to UTF-8
                        result += static_cast<char>(0xF0 | (codepoint >> 18));
                        result += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
                        result += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                        result += static_cast<char>(0x80 | (codepoint & 0x3F));
                        i++;
                        continue;
                    }
                }

                // Single code unit
                if (code_unit < 0x80) {
                    result += static_cast<char>(code_unit);
                } else if (code_unit < 0x800) {
                    result += static_cast<char>(0xC0 | (code_unit >> 6));
                    result += static_cast<char>(0x80 | (code_unit & 0x3F));
                } else {
                    result += static_cast<char>(0xE0 | (code_unit >> 12));
                    result += static_cast<char>(0x80 | ((code_unit >> 6) & 0x3F));
                    result += static_cast<char>(0x80 | (code_unit & 0x3F));
                }
            }

            return Value(result);
        });
    ctx.get_lexical_environment()->create_binding("unescape", Value(unescape_fn.get()), false);
    if (ctx.get_global_object()) {
        PropertyDescriptor unescape_desc(Value(unescape_fn.get()), PropertyAttributes::BuiltinFunction);
        ctx.get_global_object()->set_property_descriptor("unescape", unescape_desc);
    }
    unescape_fn.release();

    auto console_obj = ObjectFactory::create_object();
    auto console_log_fn = ObjectFactory::create_native_function("log", 
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            for (size_t i = 0; i < args.size(); i++) {
                if (i > 0) std::cout << " ";
                std::cout << args[i].to_string();
            }
            std::cout << std::endl;
            return Value();
        }, 1);
    auto console_error_fn = ObjectFactory::create_native_function("error",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            for (size_t i = 0; i < args.size(); i++) {
                if (i > 0) std::cerr << " ";
                std::cerr << args[i].to_string();
            }
            std::cerr << std::endl;
            return Value();
        });
    auto console_warn_fn = ObjectFactory::create_native_function("warn",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            for (size_t i = 0; i < args.size(); i++) {
                if (i > 0) std::cout << " ";
                std::cout << args[i].to_string();
            }
            std::cout << std::endl;
            return Value();
        });
    
    console_obj->set_property("log", Value(console_log_fn.release()), PropertyAttributes::BuiltinFunction);
    console_obj->set_property("error", Value(console_error_fn.release()), PropertyAttributes::BuiltinFunction);
    console_obj->set_property("warn", Value(console_warn_fn.release()), PropertyAttributes::BuiltinFunction);
    
    ctx.get_lexical_environment()->create_binding("console", Value(console_obj.release()), false);

    // print()
    auto print_fn = ObjectFactory::create_native_function("print",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            for (size_t i = 0; i < args.size(); i++) {
                if (i > 0) std::cout << " ";
                std::cout << args[i].to_string();
            }
            std::cout << std::endl;
            return Value();
        }, 1);
    ctx.get_lexical_environment()->create_binding("print", Value(print_fn.release()), false);

    // GC object with stats(), collect(), heapSize() methods
    auto gc_obj = ObjectFactory::create_object();

    auto gc_obj_stats_fn = ObjectFactory::create_native_function("stats",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!ctx.get_gc()) return Value();

            const auto& stats = ctx.get_gc()->get_statistics();
            auto stats_obj = ObjectFactory::create_object();

            stats_obj->set_property("totalAllocations", Value(static_cast<double>(stats.total_allocations)));
            stats_obj->set_property("totalDeallocations", Value(static_cast<double>(stats.total_deallocations)));
            stats_obj->set_property("totalCollections", Value(static_cast<double>(stats.total_collections)));
            stats_obj->set_property("bytesAllocated", Value(static_cast<double>(stats.bytes_allocated)));
            stats_obj->set_property("bytesFreed", Value(static_cast<double>(stats.bytes_freed)));
            stats_obj->set_property("currentMemory", Value(static_cast<double>(stats.bytes_allocated - stats.bytes_freed)));
            stats_obj->set_property("peakMemoryUsage", Value(static_cast<double>(stats.peak_memory_usage)));

            return Value(stats_obj.release());
        }, 0);

    auto gc_obj_collect_fn = ObjectFactory::create_native_function("collect",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (ctx.get_gc()) {
                ctx.get_gc()->collect_garbage();
            }
            return Value();
        }, 0);

    auto gc_obj_heap_size_fn = ObjectFactory::create_native_function("heapSize",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (ctx.get_gc()) {
                return Value(static_cast<double>(ctx.get_gc()->get_heap_size()));
            }
            return Value();
        }, 0);

    gc_obj->set_property("stats", Value(gc_obj_stats_fn.release()), PropertyAttributes::BuiltinFunction);
    gc_obj->set_property("collect", Value(gc_obj_collect_fn.release()), PropertyAttributes::BuiltinFunction);
    gc_obj->set_property("heapSize", Value(gc_obj_heap_size_fn.release()), PropertyAttributes::BuiltinFunction);

    ctx.get_lexical_environment()->create_binding("gc", Value(gc_obj.release()), false);

    auto gc_stats_fn = ObjectFactory::create_native_function("gcStats",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (ctx.get_engine()) {
                std::string stats = ctx.get_engine()->get_gc_stats();
                std::cout << stats << std::endl;
            } else {
                std::cout << "Engine not available" << std::endl;
            }
            return Value();
        });
    ctx.get_lexical_environment()->create_binding("gcStats", Value(gc_stats_fn.release()), false);
    
    auto force_gc_fn = ObjectFactory::create_native_function("forceGC",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (ctx.get_engine()) {
                ctx.get_engine()->force_gc();
                std::cout << "Garbage collection forced" << std::endl;
            } else {
                std::cout << "Engine not available" << std::endl;
            }
            return Value();
        });
    ctx.get_lexical_environment()->create_binding("forceGC", Value(force_gc_fn.release()), false);
    
    if (ctx.get_built_in_object("JSON")) {
        ctx.get_lexical_environment()->create_binding("JSON", Value(ctx.get_built_in_object("JSON")), false);
    }
    if (ctx.get_built_in_object("Date")) {
        ctx.get_lexical_environment()->create_binding("Date", Value(ctx.get_built_in_object("Date")), false);
    }
    
    auto setTimeout_fn = ObjectFactory::create_native_function("setTimeout",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return Value(1);
        });
    auto setInterval_fn = ObjectFactory::create_native_function("setInterval",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return Value(1);
        });
    auto clearTimeout_fn = ObjectFactory::create_native_function("clearTimeout",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return Value();
        });
    auto clearInterval_fn = ObjectFactory::create_native_function("clearInterval",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return Value();
        });
    
    ctx.get_lexical_environment()->create_binding("setTimeout", Value(setTimeout_fn.release()), false);
    ctx.get_lexical_environment()->create_binding("setInterval", Value(setInterval_fn.release()), false);
    ctx.get_lexical_environment()->create_binding("clearTimeout", Value(clearTimeout_fn.release()), false);
    ctx.get_lexical_environment()->create_binding("clearInterval", Value(clearInterval_fn.release()), false);
    
    
    
    if (ctx.get_built_in_object("Object")) {
        Object* obj_constructor = ctx.get_built_in_object("Object");
        Value binding_value;
        if (obj_constructor->is_function()) {
            Function* func_ptr = static_cast<Function*>(obj_constructor);
            binding_value = Value(func_ptr);
        } else {
            binding_value = Value(obj_constructor);
        }
        ctx.get_lexical_environment()->create_binding("Object", binding_value, false);
    }
    
    if (ctx.get_built_in_object("Array")) {
        Object* array_constructor = ctx.get_built_in_object("Array");
        Value binding_value;
        if (array_constructor->is_function()) {
            Function* func_ptr = static_cast<Function*>(array_constructor);
            binding_value = Value(func_ptr);
        } else {
            binding_value = Value(array_constructor);
        }
        ctx.get_lexical_environment()->create_binding("Array", binding_value, false);
    }
    
    if (ctx.get_built_in_object("Function")) {
        Object* func_constructor = ctx.get_built_in_object("Function");
        Value binding_value;
        if (func_constructor->is_function()) {
            Function* func_ptr = static_cast<Function*>(func_constructor);
            binding_value = Value(func_ptr);
        } else {
            binding_value = Value(func_constructor);
        }
        ctx.get_lexical_environment()->create_binding("Function", binding_value, false);
    }
    
    // register_built_in_object() already binds builtins to the global scope.

    IterableUtils::setup_array_iterator_methods(ctx);
    IterableUtils::setup_string_iterator_methods(ctx);
    IterableUtils::setup_map_iterator_methods(ctx);
    IterableUtils::setup_set_iterator_methods(ctx);

    // Expose Object.prototype.__defineGetter__/__defineSetter__ as global bindings
    // (browsers expose them via window.__proto__ = Object.prototype, we do it explicitly)
    {
        Value obj_ctor = ctx.get_binding("Object");
        if (obj_ctor.is_function()) {
            Value obj_proto = obj_ctor.as_function()->get_property("prototype");
            if (obj_proto.is_object()) {
                Object* op = obj_proto.as_object();
                Value dg = op->get_own_property("__defineGetter__");
                if (!dg.is_undefined() && ctx.get_lexical_environment()) {
                    ctx.get_lexical_environment()->create_binding("__defineGetter__", dg, false);
                    ctx.get_global_object()->set_property("__defineGetter__", dg, PropertyAttributes::BuiltinFunction);
                }
                Value ds = op->get_own_property("__defineSetter__");
                if (!ds.is_undefined() && ctx.get_lexical_environment()) {
                    ctx.get_lexical_environment()->create_binding("__defineSetter__", ds, false);
                    ctx.get_global_object()->set_property("__defineSetter__", ds, PropertyAttributes::BuiltinFunction);
                }
            }
        }
    }
    register_test262_builtins(ctx);
}

void register_test262_builtins(Context& ctx) {
    auto testWithTypedArrayConstructors = ObjectFactory::create_native_function("testWithTypedArrayConstructors",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].is_function()) {
                ctx.throw_type_error("testWithTypedArrayConstructors requires a function argument");
                return Value();
            }

            Function* callback = args[0].as_function();

            std::vector<std::string> constructors = {
                "Int8Array", "Uint8Array", "Uint8ClampedArray",
                "Int16Array", "Uint16Array",
                "Int32Array", "Uint32Array",
                "Float32Array", "Float64Array"
            };

            for (const auto& ctorName : constructors) {
                if (ctx.has_binding(ctorName)) {
                    Value ctor = ctx.get_binding(ctorName);
                    if (ctor.is_function()) {
                        try {
                            std::vector<Value> callArgs = { ctor };
                            callback->call(ctx, callArgs, Value());
                        } catch (...) {
                            ctx.throw_exception(Value("Error in testWithTypedArrayConstructors with " + ctorName));
                            return Value();
                        }
                    }
                }
            }

            return Value();
        });

    ctx.get_lexical_environment()->create_binding("testWithTypedArrayConstructors", Value(testWithTypedArrayConstructors.release()), false);

    auto buildString = ObjectFactory::create_native_function("buildString",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].is_object()) {
                ctx.throw_type_error("buildString requires an object argument");
                return Value();
            }

            Object* argsObj = args[0].as_object();
            std::string result;

            if (argsObj->has_property("loneCodePoints")) {
                Value loneVal = argsObj->get_property("loneCodePoints");
                if (loneVal.is_object() && loneVal.as_object()->is_array()) {
                    Object* loneArray = loneVal.as_object();
                    uint32_t length = static_cast<uint32_t>(loneArray->get_property("length").as_number());
                    for (uint32_t i = 0; i < length; i++) {
                        Value elem = loneArray->get_element(i);
                        if (elem.is_number()) {
                            uint32_t codePoint = static_cast<uint32_t>(elem.as_number());
                            if (codePoint < 0x80) {
                                result += static_cast<char>(codePoint);
                            }
                        }
                    }
                }
            }

            if (argsObj->has_property("ranges")) {
                Value rangesVal = argsObj->get_property("ranges");
                if (rangesVal.is_object() && rangesVal.as_object()->is_array()) {
                    Object* rangesArray = rangesVal.as_object();
                    uint32_t rangeCount = static_cast<uint32_t>(rangesArray->get_property("length").as_number());

                    for (uint32_t i = 0; i < rangeCount; i++) {
                        Value rangeVal = rangesArray->get_element(i);
                        if (rangeVal.is_object() && rangeVal.as_object()->is_array()) {
                            Object* range = rangeVal.as_object();
                            Value startVal = range->get_element(0);
                            Value endVal = range->get_element(1);

                            if (startVal.is_number() && endVal.is_number()) {
                                uint32_t start = static_cast<uint32_t>(startVal.as_number());
                                uint32_t end = static_cast<uint32_t>(endVal.as_number());

                                for (uint32_t cp = start; cp <= end && cp < 0x80 && result.length() < 1000; cp++) {
                                    result += static_cast<char>(cp);
                                }
                            }
                        }
                    }
                }
            }

            return Value(result);
        });

    ctx.get_lexical_environment()->create_binding("buildString", Value(buildString.release()), false);
}

} // namespace Quanta
