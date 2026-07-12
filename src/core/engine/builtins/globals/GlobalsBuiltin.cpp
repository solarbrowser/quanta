/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "quanta/core/engine/builtins/GlobalsBuiltin.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/engine/Engine.h"
#include <memory>
#include <functional>
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/Error.h"
#include "quanta/core/engine/CallStack.h"
#include "quanta/lexer/Lexer.h"
#include "quanta/parser/Parser.h"
#include "quanta/parser/AST.h"
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_set>
#include "quanta/core/runtime/Iterator.h"
#include "quanta/core/runtime/Promise.h"
#include "quanta/core/runtime/ArrayBuffer.h"
#include "quanta/core/modules/ModuleLoader.h"
#include <filesystem>
#include "quanta/core/runtime/BigInt.h"
#include "quanta/core/runtime/String.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/core/gc/Collector.h"
#include "quanta/core/gc/Heap.h"
#include "quanta/core/runtime/Async.h"

namespace Quanta {

// ToString(argument) for a URI global function: ToPrimitive with "string" hint, invoking
// user toString/valueOf and propagating exceptions, instead of the native stringifier.
static bool uri_arg_to_string(Context& ctx, const Value& v, std::string& out) {
    if (v.is_symbol()) { ctx.throw_type_error("Cannot convert Symbol to string"); return false; }
    if (v.is_object() || v.is_function()) {
        out = v.to_property_key();
        return !ctx.has_exception();
    }
    out = v.to_string();
    return true;
}

static bool is_uri_unescaped_uri(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           (c != 0 && strchr(";,/?:@&=+$-_.!~*'()#", c) != nullptr);
}
static bool is_uri_unescaped_uri_component(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           (c != 0 && strchr("-_.!~*'()", c) != nullptr);
}

// Encode ( string, unescaped ): combines a WTF-8 surrogate pair back into one code point
// (an unpaired surrogate throws) before percent-encoding its UTF-8 bytes.
static bool uri_encode(Context& ctx, const std::string& input, bool (*unescaped)(unsigned char), std::string& result) {
    result.reserve(input.size());
    size_t i = 0;
    while (i < input.size()) {
        unsigned char c = static_cast<unsigned char>(input[i]);
        if (c < 0x80) {
            if (unescaped(c)) { result += static_cast<char>(c); i++; continue; }
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X", c);
            result += hex;
            i++;
            continue;
        }
        size_t len = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
        if (i + len > input.size()) { ctx.throw_uri_error("URI malformed"); return false; }
        uint32_t cp = len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
        for (size_t k = 1; k < len; k++) cp = (cp << 6) | (static_cast<unsigned char>(input[i + k]) & 0x3F);
        i += len;
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            bool has_low = i + 3 <= input.size() && static_cast<unsigned char>(input[i]) == 0xED &&
                           static_cast<unsigned char>(input[i + 1]) >= 0xB0 && static_cast<unsigned char>(input[i + 1]) <= 0xBF;
            if (!has_low) { ctx.throw_uri_error("URI malformed"); return false; }
            uint32_t lo = 0xD000 | ((static_cast<unsigned char>(input[i + 1]) & 0x3F) << 6) | (static_cast<unsigned char>(input[i + 2]) & 0x3F);
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            i += 3;
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            ctx.throw_uri_error("URI malformed");
            return false;
        }
        char buf8[4];
        int n8;
        if (cp < 0x800) { buf8[0] = static_cast<char>(0xC0 | (cp >> 6)); buf8[1] = static_cast<char>(0x80 | (cp & 0x3F)); n8 = 2; }
        else if (cp < 0x10000) {
            buf8[0] = static_cast<char>(0xE0 | (cp >> 12));
            buf8[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            buf8[2] = static_cast<char>(0x80 | (cp & 0x3F));
            n8 = 3;
        } else {
            buf8[0] = static_cast<char>(0xF0 | (cp >> 18));
            buf8[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            buf8[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            buf8[3] = static_cast<char>(0x80 | (cp & 0x3F));
            n8 = 4;
        }
        for (int k = 0; k < n8; k++) {
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X", static_cast<unsigned char>(buf8[k]));
            result += hex;
        }
    }
    return true;
}

// Decode ( string, reserved ): validates UTF-8 continuation bytes and rejects a decoded
// surrogate (UTF-8 must never directly encode U+D800-U+DFFF).
static bool uri_decode(Context& ctx, const std::string& input, bool (*reserved)(unsigned char), std::string& result) {
    auto is_hex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); };
    auto hex_val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return c - 'A' + 10;
    };
    result.reserve(input.size());
    for (size_t i = 0; i < input.size(); i++) {
        if (input[i] != '%') { result += input[i]; continue; }
        if (i + 2 >= input.size() || !is_hex(input[i + 1]) || !is_hex(input[i + 2])) {
            ctx.throw_uri_error("URI malformed");
            return false;
        }
        int byte1 = hex_val(input[i + 1]) * 16 + hex_val(input[i + 2]);
        if (byte1 < 0x80) {
            if (reserved(static_cast<unsigned char>(byte1))) {
                result += input[i]; result += input[i + 1]; result += input[i + 2];
            } else {
                result += static_cast<char>(byte1);
            }
            i += 2;
            continue;
        }
        int num_bytes = (byte1 & 0xE0) == 0xC0 ? 2 : (byte1 & 0xF0) == 0xE0 ? 3 : (byte1 & 0xF8) == 0xF0 ? 4 : 0;
        if (num_bytes == 0) { ctx.throw_uri_error("URI malformed"); return false; }
        uint32_t cp = num_bytes == 2 ? (byte1 & 0x1F) : num_bytes == 3 ? (byte1 & 0x0F) : (byte1 & 0x07);
        std::string utf8;
        utf8 += static_cast<char>(byte1);
        for (int j = 1; j < num_bytes; j++) {
            size_t pos = i + j * 3;
            if (pos + 2 >= input.size() || input[pos] != '%' || !is_hex(input[pos + 1]) || !is_hex(input[pos + 2])) {
                ctx.throw_uri_error("URI malformed");
                return false;
            }
            int cb = hex_val(input[pos + 1]) * 16 + hex_val(input[pos + 2]);
            if ((cb & 0xC0) != 0x80) { ctx.throw_uri_error("URI malformed"); return false; }
            cp = (cp << 6) | (cb & 0x3F);
            utf8 += static_cast<char>(cb);
        }
        static const uint32_t min_cp[] = {0, 0, 0x80, 0x800, 0x10000};
        if (cp >= 0xD800 && cp <= 0xDFFF) { ctx.throw_uri_error("URI malformed"); return false; } // surrogate
        if (cp < min_cp[num_bytes]) { ctx.throw_uri_error("URI malformed"); return false; } // overlong encoding
        if (cp > 0x10FFFF) { ctx.throw_uri_error("URI malformed"); return false; } // out of Unicode range
        result += utf8;
        i += num_bytes * 3 - 1;
    }
    return true;
}

void register_global_builtins(Context& ctx) {
    if (!ctx.get_lexical_environment()) return;
    
    // js_ws_trim: trim all JS whitespace including Unicode, returns trimmed string.
    auto js_trim = [](const std::string& s) -> std::string {
        auto is_ws = [](const std::string& str, size_t i) -> size_t {
            unsigned char c = static_cast<unsigned char>(str[i]);
            if (std::isspace(c)) return 1;
            if (c == 0xC2 && i+1 < str.size() && (unsigned char)str[i+1] == 0xA0) return 2;
            if (i+2 < str.size()) {
                unsigned char c2 = (unsigned char)str[i+1], c3 = (unsigned char)str[i+2];
                if (c==0xEF && c2==0xBB && c3==0xBF) return 3;
                if (c==0xE1 && c2==0x9A && c3==0x80) return 3;
                if (c==0xE3 && c2==0x80 && c3==0x80) return 3;
                if (c==0xE2 && c2==0x80 && (c3>=0x80&&c3<=0x8A||c3==0xA8||c3==0xA9||c3==0xAF)) return 3;
                if (c==0xE2 && c2==0x81 && c3==0x9F) return 3;
            }
            return 0;
        };
        size_t start = 0, end = s.size();
        while (start < end) { size_t n = is_ws(s, start); if (!n) break; start += n; }
        while (end > start) {
            size_t n = 0;
            if (end>=3) n = is_ws(s, end-3); if (n==3) { end-=3; continue; }
            if (end>=2) n = is_ws(s, end-2); if (n==2) { end-=2; continue; }
            n = is_ws(s, end-1); if (n==1) { end-=1; continue; }
            break;
        }
        return s.substr(start, end-start);
    };

    auto parseInt_fn = ObjectFactory::create_native_function("parseInt",
        [js_trim](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value::nan();

            std::string str = js_trim(args[0].to_property_key());
            if (ctx.has_exception()) return Value::nan();

            size_t start = 0;
            if (start >= str.length()) return Value::nan();

            // Per spec: ToInt32(radix) then validate range; a radix of 0 behaves
            // exactly like an omitted radix (hex-prefix detection stays enabled).
            int radix = 10;
            bool radix_provided = args.size() > 1 && !args[1].is_undefined();
            bool radix_zero = false;
            if (radix_provided) {
                double r = args[1].to_number();
                if (ctx.has_exception()) return Value::nan();
                // ToInt32 wrapping per spec: handles overflow like 4294967298 → 2
                if (std::isnan(r) || r == 0.0 || std::isinf(r)) { radix = 10; radix_zero = true; }
                else {
                    int32_t ri = static_cast<int32_t>(static_cast<uint32_t>(static_cast<int64_t>(r) & 0xFFFFFFFF));
                    if (ri == 0) { radix = 10; radix_zero = true; }
                    else if (ri < 2 || ri > 36) return Value::nan();
                    else radix = ri;
                }
            }

            double sign = 1.0;
            if (str[start] == '+' || str[start] == '-') {
                if (str[start] == '-') sign = -1.0;
                ++start;
            }

            // Hex prefix strips only when radix is 16 or was left unspecified.
            bool default_radix = !radix_provided || radix_zero;
            if (start + 1 < str.length() && str[start] == '0' &&
                (str[start + 1] == 'x' || str[start + 1] == 'X') &&
                (radix == 16 || default_radix)) {
                radix = 16;
                start += 2;
            }

            // Digit-by-digit double accumulation: exact for values a 64-bit
            // integer parse would overflow on (e.g. parseInt("0x1" + "0"*17, 16)).
            auto digit_val = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'z') return c - 'a' + 10;
                if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
                return -1;
            };
            double result = 0.0;
            size_t digits = 0;
            while (start < str.length()) {
                int d = digit_val(str[start]);
                if (d < 0 || d >= radix) break;
                result = result * radix + d;
                ++start;
                ++digits;
            }
            if (digits == 0) return Value::nan();
            return Value(sign * result);
        }, 2);
    Function* parseInt_raw = parseInt_fn.get();
    ctx.get_lexical_environment()->create_binding("parseInt", Value(parseInt_fn.release()), true, true, false);

    auto parseFloat_fn = ObjectFactory::create_native_function("parseFloat",
        [js_trim](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value::nan();

            // ToString via the string-hinted ToPrimitive path (toString before valueOf).
            std::string str = js_trim(args[0].to_property_key());
            if (ctx.has_exception()) return Value::nan();

            if (str.empty()) return Value::nan();

            // Explicit case-sensitive check for "Infinity"/"+Infinity"/"-Infinity"
            if (str == "Infinity" || str == "+Infinity") return Value(std::numeric_limits<double>::infinity());
            if (str == "-Infinity") return Value(-std::numeric_limits<double>::infinity());
            // Prefix match for "Infinity" at start
            size_t off = (str[0]=='+' || str[0]=='-') ? 1 : 0;
            if (str.substr(off, 8) == "Infinity") {
                return Value(str[0]=='-' ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity());
            }

            char first_char = str[0];
            if (!std::isdigit(static_cast<unsigned char>(first_char)) && first_char != '.' &&
                first_char != '+' && first_char != '-') {
                return Value::nan();
            }

            // StrDecimalLiteral has no hex form: "0x1" parses as the prefix "0".
            if (off + 1 < str.length() && str[off] == '0' &&
                (str[off + 1] == 'x' || str[off + 1] == 'X')) {
                return Value(str[0] == '-' ? -0.0 : 0.0);
            }

            try {
                size_t pos;
                double result = std::stod(str, &pos);
                if (pos == 0) return Value::nan();
                if (std::isinf(result) && str.substr(0, pos) != "Infinity" &&
                    str.substr(0, pos) != "+Infinity" && str.substr(0, pos) != "-Infinity") {
                    return Value::nan();
                }
                return Value(result);
            } catch (...) {
                return Value::nan();
            }
        }, 1);
    Function* parseFloat_raw = parseFloat_fn.get();
    ctx.get_lexical_environment()->create_binding("parseFloat", Value(parseFloat_fn.release()), true, true, false);

    // ES6: Number.parseFloat and Number.parseInt must be the same objects as globals
    {
        Object* num_obj = ctx.get_built_in_object("Number");
        if (num_obj && num_obj->is_function()) {
            Function* num_ctor = static_cast<Function*>(num_obj);
            num_ctor->set_property("parseFloat", Value(parseFloat_raw), PropertyAttributes::BuiltinFunction);
            num_ctor->set_property("parseInt", Value(parseInt_raw), PropertyAttributes::BuiltinFunction);
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
    ctx.get_lexical_environment()->create_binding("isNaN", Value(isNaN_global_fn.release()), true, true, false);

    auto isFinite_global_fn = ObjectFactory::create_native_function("isFinite",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value(false);
            double num = args[0].to_number();
            return Value(std::isfinite(num));
        }, 1);
    ctx.get_lexical_environment()->create_binding("isFinite", Value(isFinite_global_fn.release()), true, true, false);

    auto eval_fn = ObjectFactory::create_native_function("eval",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value();
            if (!args[0].is_string()) return args[0];

            std::string code = args[0].to_string();
            if (code.empty()) return Value();

            Engine* engine = ctx.get_engine();
            if (!engine) return Value();

            bool strict = ctx.is_strict_mode();
            // Per spec, indirect eval (0,eval)('...') is never strict from the caller's context.
            // Only the eval source's own 'use strict' directive makes it strict.
            if (!ctx.is_direct_eval_call()) {
                strict = false;
            }

            try {
                // Parse with strict mode if calling context is strict (or eval source has 'use strict')
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
                parse_opts.in_eval_context = ctx.is_direct_eval_call();
                // new.target in eval is valid inside non-arrow function code
                parse_opts.eval_in_function_code = (ctx.get_type() == Context::Type::Function ||
                                                    ctx.get_type() == Context::Type::Eval) &&
                                                   !ctx.is_arrow_function_context();
                // super in eval is valid if we're inside a method/derived-class context (__super__ present)
                parse_opts.eval_in_method_code = ctx.has_binding("__super__");
                // Propagate class field initializer context so eval enforces ContainsArguments early error.
                // Only direct eval inherits this flag -- indirect eval is a fresh context per spec.
                parse_opts.in_class_field_init = ctx.is_direct_eval_call() && ctx.is_in_class_field_init();
                // Collect private names that are valid in this eval context.
                // Only names actually declared in the enclosing class are valid (AllPrivateNamesValid).
                if (ctx.has_binding("__eval_private_names__")) {
                    Value brands = ctx.get_binding("__eval_private_names__");
                    if (brands.is_object()) {
                        for (const auto& key : brands.as_object()->get_own_property_keys()) {
                            parse_opts.eval_private_names.insert(key);
                        }
                    }
                }
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

                auto collect_var_names = [](const Program* prog) {
                    std::vector<std::string> names;
                    if (!prog) return names;
                    std::function<void(const ASTNode*)> walk = [&](const ASTNode* nd) {
                        if (!nd) return;
                        using T = ASTNode::Type;
                        switch (nd->get_type()) {
                            case T::VARIABLE_DECLARATION: {
                                auto* vd = static_cast<const VariableDeclaration*>(nd);
                                if (vd->get_kind() == VariableDeclarator::Kind::VAR)
                                    for (const auto& d : vd->get_declarations())
                                        if (d->get_id()) names.push_back(d->get_id()->get_name());
                                break;
                            }
                            case T::FUNCTION_DECLARATION: {
                                auto* fd = static_cast<const FunctionDeclaration*>(nd);
                                if (fd->get_id()) names.push_back(fd->get_id()->get_name());
                                break;
                            }
                            case T::BLOCK_STATEMENT:
                                for (const auto& s : static_cast<const BlockStatement*>(nd)->get_statements()) walk(s.get());
                                break;
                            case T::IF_STATEMENT: {
                                auto* is = static_cast<const IfStatement*>(nd);
                                walk(is->get_consequent());
                                if (is->has_alternate()) walk(is->get_alternate());
                                break;
                            }
                            case T::FOR_STATEMENT: { auto* fs = static_cast<const ForStatement*>(nd); walk(fs->get_init()); walk(fs->get_body()); break; }
                            case T::FOR_IN_STATEMENT: walk(static_cast<const ForInStatement*>(nd)->get_body()); break;
                            case T::FOR_OF_STATEMENT: walk(static_cast<const ForOfStatement*>(nd)->get_body()); break;
                            case T::WHILE_STATEMENT: walk(static_cast<const WhileStatement*>(nd)->get_body()); break;
                            case T::DO_WHILE_STATEMENT: walk(static_cast<const DoWhileStatement*>(nd)->get_body()); break;
                            case T::LABELED_STATEMENT: walk(static_cast<const LabeledStatement*>(nd)->get_statement()); break;
                            case T::WITH_STATEMENT: walk(static_cast<const WithStatement*>(nd)->get_body()); break;
                            case T::TRY_STATEMENT: { auto* ts = static_cast<const TryStatement*>(nd); walk(ts->get_try_block()); walk(ts->get_catch_clause()); walk(ts->get_finally_block()); break; }
                            case T::CATCH_CLAUSE: walk(static_cast<const CatchClause*>(nd)->get_body()); break;
                            case T::SWITCH_STATEMENT: { auto* ss = static_cast<const SwitchStatement*>(nd); for (const auto& c : ss->get_cases()) walk(c.get()); break; }
                            case T::CASE_CLAUSE: { auto* cc = static_cast<const CaseClause*>(nd); for (const auto& s : cc->get_consequent()) walk(s.get()); break; }
                            default: break;
                        }
                    };
                    for (const auto& s : prog->get_statements()) walk(s.get());
                    return names;
                };

                // Eval is strict if the calling context is strict OR the eval source has 'use strict'.
                // program->is_strict() captures both: parse_opts.strict_mode starts from ctx strict,
                // and check_for_use_strict_directive() sets it true if the source has 'use strict'.
                if (!strict && program && program->is_strict()) {
                    strict = true;
                }

                if (strict) {
                    // ES5 10.4.2: Strict eval creates isolated variable environment.
                    // IMPORTANT: heap-allocate and add to survivor pool so that functions
                    // created inside eval can safely hold a pointer to this context after
                    // eval returns (stack-allocated context would be a dangling pointer).
                    auto var_names = collect_var_names(program.get());

                    auto eval_ctx_ptr = std::make_unique<Context>(engine, &ctx, Context::Type::Eval);
                    Context& eval_ctx = *eval_ctx_ptr;
                    eval_ctx.set_strict_mode(true);
                    auto eval_env = new Environment(
                        Environment::Type::Declarative, ctx.get_lexical_environment());
                    eval_ctx.set_lexical_environment(eval_env);
                    eval_ctx.set_variable_environment(eval_env);
                    // Native call machinery temporarily overwrites "this" in the calling env.
                    // Recover the caller's real "this" from the backup set before the overwrite.
                    Value caller_this_val;
                    if (ctx.is_direct_eval_call() && ctx.has_binding("__eval_caller_this__")) {
                        caller_this_val = ctx.get_binding("__eval_caller_this__");
                    } else {
                        caller_this_val = ctx.get_binding("this");
                    }
                    {
                        Object* this_obj = caller_this_val.is_object() ? caller_this_val.as_object()
                                         : caller_this_val.is_function() ? caller_this_val.as_function()
                                         : ctx.get_this_binding();
                        eval_ctx.set_this_binding(this_obj);
                        eval_env->create_binding("this", caller_this_val, true, false);
                    }

                    // Pre-create var bindings in isolated env so hoist doesn't find them in
                    // the outer scope chain and skip creation (which would cause them to leak)
                    for (const auto& vname : var_names) {
                        eval_env->create_binding(vname, Value(), true, false);
                    }

                    Value result = program->evaluate(eval_ctx);

                    bool has_exc = eval_ctx.has_exception();
                    Value exception = has_exc ? eval_ctx.get_exception() : Value();
                    if (has_exc) eval_ctx.clear_exception();

                    // Transfer to survivor pool so closures created inside can
                    // safely reference this context after eval returns
                    if (engine) engine->add_survivor_context(eval_ctx_ptr.release());

                    if (has_exc) {
                        ctx.throw_exception(exception);
                        return Value();
                    }

                    return result;
                } else {
                    // Non-strict eval: run in calling context's scope chain
                    auto var_names = collect_var_names(program.get());

                    // Indirect eval runs in global scope, so block-scope walk between
                    // lex_env and var_env does not apply; only direct eval checks calling context.
                    bool is_direct = ctx.is_direct_eval_call();
                    auto* lex_env = ctx.get_lexical_environment();
                    auto* var_env = ctx.get_variable_environment();

                    // var-vs-lexical-declaration collision is a SyntaxError regardless of direct/indirect; indirect eval checks the global env, not the caller's lex_env.
                    Environment* global_lex_env = (!is_direct && engine && engine->get_global_context())
                        ? engine->get_global_context()->get_lexical_environment()
                        : lex_env;
                    if (global_lex_env) {
                        for (const auto& vname : var_names) {
                            if (global_lex_env->has_lexical_declaration(vname)) {
                                ctx.throw_syntax_error("Variable '" + vname + "' has already been declared");
                                return Value();
                            }
                        }
                    }
                    if (is_direct) {
                        if (lex_env != var_env) {
                            Environment* env = lex_env;
                            while (env && env != var_env) {
                                if (env->get_type() != Environment::Type::Object) {
                                    for (const auto& vname : var_names) {
                                        if (env->has_own_binding(vname)) {
                                            ctx.throw_syntax_error("Variable '" + vname + "' has already been declared");
                                            return Value();
                                        }
                                    }
                                }
                                env = env->get_outer();
                            }
                        }
                    }

                    // When eval runs inside default parameter expressions, var declarations
                    // cannot shadow formal parameter names (non-simple params create separate scope)
                    if (ctx.is_in_param_eval()) {
                        const auto& param_names = ctx.get_eval_param_names();
                        for (const auto& vname : var_names) {
                            if (ctx.has_eval_arguments_conflict() && vname == "arguments") {
                                ctx.throw_syntax_error("Variable 'arguments' has already been declared");
                                return Value();
                            }
                            if (param_names.count(vname)) {
                                ctx.throw_syntax_error("Variable '" + vname + "' has already been declared");
                                return Value();
                            }
                        }
                    }

                    // EvalDeclarationInstantiation CanDeclareGlobalFunction pre-flight check.
                    // Must run before the eval body so bindings are never created on TypeError.
                    // For indirect eval, check the global var env (not the caller's).
                    Environment* check_var_env = var_env;
                    if (!is_direct && engine && engine->get_global_context()) {
                        check_var_env = engine->get_global_context()->get_variable_environment();
                    }
                    if (check_var_env && check_var_env->get_type() == Environment::Type::Object && check_var_env->get_binding_object()) {
                        Object* global_obj = check_var_env->get_binding_object();
                        std::unordered_set<std::string> checked_func_names;
                        for (auto it = program->get_statements().rbegin(); it != program->get_statements().rend(); ++it) {
                            const ASTNode* nd = it->get();
                            if (nd->get_type() != ASTNode::Type::FUNCTION_DECLARATION) continue;
                            auto* fd = static_cast<const FunctionDeclaration*>(nd);
                            if (!fd->get_id()) continue;
                            const std::string& fn = fd->get_id()->get_name();
                            if (checked_func_names.count(fn)) continue;
                            checked_func_names.insert(fn);
                            if (global_obj->has_own_property(fn)) {
                                PropertyDescriptor existing = global_obj->get_property_descriptor(fn);
                                if (!existing.is_configurable() && (!existing.is_writable() || !existing.is_enumerable())) {
                                    ctx.throw_type_error("Cannot declare function '" + fn + "'");
                                    return Value();
                                }
                            }
                        }
                    }

                    // Collect top-level lex declarations (let/const/class) for TDZ pre-instantiation
                    auto collect_lex_names = [](const Program* prog) {
                        std::vector<std::string> names;
                        if (!prog) return names;
                        for (const auto& s : prog->get_statements()) {
                            const ASTNode* nd = s.get();
                            if (nd->get_type() == ASTNode::Type::VARIABLE_DECLARATION) {
                                auto* vd = static_cast<const VariableDeclaration*>(nd);
                                if (vd->get_kind() != VariableDeclarator::Kind::VAR)
                                    for (const auto& d : vd->get_declarations())
                                        if (d->get_id()) names.push_back(d->get_id()->get_name());
                            } else if (nd->get_type() == ASTNode::Type::CLASS_DECLARATION) {
                                auto* cd = static_cast<const ClassDeclaration*>(nd);
                                if (cd->get_id()) names.push_back(cd->get_id()->get_name());
                            }
                        }
                        return names;
                    };

                    // Heap-allocate so functions created inside keep valid closure_context_
                    auto eval_ctx_ptr2 = std::make_unique<Context>(engine, &ctx, Context::Type::Eval);
                    Context& eval_ctx = *eval_ctx_ptr2;

                    // Indirect eval runs in the global scope; direct eval runs in calling scope.
                    Environment* outer_env = ctx.get_lexical_environment();
                    Environment* var_env_base = ctx.get_variable_environment();
                    if (!is_direct && engine && engine->get_global_context()) {
                        Context* global_ctx = engine->get_global_context();
                        outer_env = global_ctx->get_lexical_environment();
                        var_env_base = global_ctx->get_variable_environment();
                    }

                    // Spec 18.2.1 step 9a: eval's lexEnv is a NEW declarative env
                    auto* eval_lex_env = new Environment(
                        Environment::Type::Declarative, outer_env);

                    for (const auto& lname : collect_lex_names(program.get())) {
                        eval_lex_env->create_uninitialized_binding(lname);
                    }

                    eval_ctx.set_lexical_environment(eval_lex_env);
                    eval_ctx.set_variable_environment(var_env_base);
                    Object* this_binding = is_direct ? ctx.get_this_binding() : (engine && engine->get_global_context() ? engine->get_global_context()->get_this_binding() : ctx.get_this_binding());
                    eval_ctx.set_this_binding(this_binding);
                    eval_ctx.set_strict_mode(false);

                    Value result = program->evaluate(eval_ctx);

                    bool has_exc2 = eval_ctx.has_exception();
                    Value exc2 = has_exc2 ? eval_ctx.get_exception() : Value();
                    if (has_exc2) eval_ctx.clear_exception();

                    if (engine) engine->add_survivor_context(eval_ctx_ptr2.release());

                    if (has_exc2) {
                        ctx.throw_exception(exc2);
                        return Value();
                    }

                    return result;
                }
            } catch (const std::exception& e) {
                ctx.throw_syntax_error(std::string(e.what()));
                return Value();
            } catch (...) {
                ctx.throw_syntax_error("Unknown syntax error");
                return Value();
            }
        }, 1);
    ctx.get_lexical_environment()->create_binding("eval", Value(eval_fn.release()), true, true, false);

    ctx.get_lexical_environment()->create_binding("undefined", Value(), false, false, false);
    ctx.get_lexical_environment()->create_binding("null", Value::null(), false, false);
    
    if (ctx.get_global_object()) {
        ctx.get_lexical_environment()->create_binding("globalThis", Value(ctx.get_global_object()), true, true, false);
        ctx.get_lexical_environment()->create_binding("global", Value(ctx.get_global_object()), true);
        ctx.get_lexical_environment()->create_binding("window", Value(ctx.get_global_object()), true);
        ctx.get_lexical_environment()->create_binding("this", Value(ctx.get_global_object()), true);

        PropertyDescriptor global_ref_desc(Value(ctx.get_global_object()), PropertyAttributes::BuiltinFunction);
        ctx.get_global_object()->set_property_descriptor("globalThis", global_ref_desc);
        ctx.get_global_object()->set_property_descriptor("global", global_ref_desc);
        ctx.get_global_object()->set_property_descriptor("window", global_ref_desc);
        ctx.get_global_object()->set_property_descriptor("this", global_ref_desc);
    }

    // test262 host API ($262)
    {
        auto test262_host = ObjectFactory::create_object();

        auto detach_fn = ObjectFactory::create_native_function("detachArrayBuffer",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                if (args.empty() || !args[0].is_object() || !args[0].as_object()->is_array_buffer()) {
                    ctx.throw_type_error("detachArrayBuffer requires an ArrayBuffer argument");
                    return Value();
                }
                static_cast<ArrayBuffer*>(args[0].as_object())->detach();
                return Value();
            });
        test262_host->set_property("detachArrayBuffer", Value(detach_fn.release()), PropertyAttributes::BuiltinFunction);

        // createRealm: spawn a new Engine with its own intrinsics; return {global, eval, createRealm}
        using RealmFn = std::function<Value(Context&, const std::vector<Value>&)>;
        auto make_realm_fn = std::make_shared<RealmFn>();
        *make_realm_fn = [make_realm_fn](Context& caller_ctx, const std::vector<Value>&) -> Value {
            Engine* new_engine = new Engine(); // intentionally leaked -- consistent with engine memory model
            if (!new_engine) { caller_ctx.throw_type_error("createRealm: failed to create engine"); return Value(); }
            if (!new_engine->initialize()) { caller_ctx.throw_type_error("createRealm: failed to initialize engine"); return Value(); }

            Context* new_ctx = new_engine->get_global_context();
            Object* new_global = new_ctx ? new_ctx->get_global_object() : nullptr;
            if (!new_global) { caller_ctx.throw_type_error("createRealm: no global"); return Value(); }

            auto realm_obj = ObjectFactory::create_object();
            realm_obj->set_property("global", Value(new_global));

            auto eval_fn = ObjectFactory::create_native_function("eval",
                [new_engine](Context& ctx, const std::vector<Value>& args) -> Value {
                    if (args.empty()) return Value();
                    auto result = new_engine->evaluate(args[0].to_string());
                    if (!result.success) {
                        ctx.throw_exception(result.exception_value.is_undefined()
                            ? Value(result.error_message) : result.exception_value);
                        return Value();
                    }
                    return result.value;
                }, 1);
            realm_obj->set_property("eval", Value(eval_fn.release()), PropertyAttributes::BuiltinFunction);
            realm_obj->set_property("evalScript", realm_obj->get_property("eval"));

            auto sub_create = ObjectFactory::create_native_function("createRealm",
                [make_realm_fn](Context& ctx, const std::vector<Value>& args) -> Value {
                    return (*make_realm_fn)(ctx, args);
                }, 0);
            realm_obj->set_property("createRealm", Value(sub_create.release()), PropertyAttributes::BuiltinFunction);

            realm_obj->set_property("$262", Value(realm_obj.get()));
            return Value(realm_obj.release());
        };

        auto createRealm_fn = ObjectFactory::create_native_function("createRealm",
            [make_realm_fn](Context& ctx, const std::vector<Value>& args) -> Value {
                return (*make_realm_fn)(ctx, args);
            }, 0);
        test262_host->set_property("createRealm", Value(createRealm_fn.release()), PropertyAttributes::BuiltinFunction);

        // %AbstractModuleSource% (28.3): constructing it always throws; the @@toStringTag
        // getter reads a [[ModuleSourceClassName]] slot that no object carries yet.
        {
            auto ams_ctor = ObjectFactory::create_native_function("AbstractModuleSource",
                [](Context& ctx, const std::vector<Value>&) -> Value {
                    ctx.throw_type_error("%AbstractModuleSource% is not constructible");
                    return Value();
                }, 0);
            auto ams_proto = ObjectFactory::create_object();
            ams_proto->set_property("constructor", Value(ams_ctor.get()), PropertyAttributes::BuiltinFunction);
            Symbol* ams_tag_sym = Symbol::get_well_known(Symbol::TO_STRING_TAG);
            if (ams_tag_sym) {
                auto tag_getter = ObjectFactory::create_native_function("get [Symbol.toStringTag]",
                    [](Context&, const std::vector<Value>&) -> Value { return Value(); }, 0);
                PropertyDescriptor tag_desc;
                tag_desc.set_getter(tag_getter.release());
                tag_desc.set_enumerable(false);
                tag_desc.set_configurable(true);
                ams_proto->set_property_descriptor(ams_tag_sym->to_property_key(), tag_desc);
            }
            ams_ctor->set_property_descriptor("prototype",
                PropertyDescriptor(Value(ams_proto.release()), PropertyAttributes::None));
            test262_host->set_property("AbstractModuleSource", Value(ams_ctor.release()));
        }

        ctx.get_lexical_environment()->create_binding("$262", Value(test262_host.release()), true);
    }
    ctx.get_lexical_environment()->create_binding("true", Value(true), false, false);
    ctx.get_lexical_environment()->create_binding("false", Value(false), false, false);
    
    ctx.get_lexical_environment()->create_binding("NaN", Value::nan(), false, false, false);
    ctx.get_lexical_environment()->create_binding("Infinity", Value::positive_infinity(), false, false, false);

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
            std::string input;
            if (!args.empty() && !uri_arg_to_string(ctx, args[0], input)) return Value();
            if (args.empty()) input = "undefined";
            std::string result;
            if (!uri_encode(ctx, input, is_uri_unescaped_uri, result)) return Value();
            return Value(result);
        }, 1);
    ctx.get_lexical_environment()->create_binding("encodeURI", Value(encode_uri_fn.release()), true, true, false);

    auto decode_uri_fn = ObjectFactory::create_native_function("decodeURI",
        [is_uri_reserved](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string input;
            if (!args.empty() && !uri_arg_to_string(ctx, args[0], input)) return Value();
            if (args.empty()) input = "undefined";
            std::string result;
            if (!uri_decode(ctx, input, is_uri_reserved, result)) return Value();
            return Value(result);
        }, 1);
    ctx.get_lexical_environment()->create_binding("decodeURI", Value(decode_uri_fn.release()), true, true, false);

    auto encode_uri_component_fn = ObjectFactory::create_native_function("encodeURIComponent",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string input;
            if (!args.empty() && !uri_arg_to_string(ctx, args[0], input)) return Value();
            if (args.empty()) input = "undefined";
            std::string result;
            if (!uri_encode(ctx, input, is_uri_unescaped_uri_component, result)) return Value();
            return Value(result);
        }, 1);
    ctx.get_lexical_environment()->create_binding("encodeURIComponent", Value(encode_uri_component_fn.release()), true, true, false);

    auto decode_uri_component_fn = ObjectFactory::create_native_function("decodeURIComponent",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string input;
            if (!args.empty() && !uri_arg_to_string(ctx, args[0], input)) return Value();
            if (args.empty()) input = "undefined";
            std::string result;
            if (!uri_decode(ctx, input, [](unsigned char) { return false; }, result)) return Value();
            return Value(result);
        }, 1);
    ctx.get_lexical_environment()->create_binding("decodeURIComponent", Value(decode_uri_component_fn.release()), true, true, false);
    
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
            // ToIndex(bits): Symbol/BigInt → TypeError, negative → RangeError
            if (args[0].is_symbol() || args[0].is_bigint()) { ctx.throw_type_error("Cannot convert Symbol/BigInt to number"); return Value(); }
            double n_raw = args[0].to_number();
            if (ctx.has_exception()) return Value();
            // ToIndex: NaN → 0, then truncate towards 0, then check >= 0
            double n_d = std::isnan(n_raw) ? 0.0 : (n_raw < 0 ? std::ceil(n_raw) : std::floor(n_raw));
            if (n_d < 0 || n_d > 9007199254740991.0 || std::isinf(n_d)) { ctx.throw_range_error("Invalid width"); return Value(); }
            int64_t n = static_cast<int64_t>(n_d);
            // ToBigInt(bigint): convert if needed
            Value bv = args[1];
            if (!bv.is_bigint()) {
                if (bv.is_number() || bv.is_null() || bv.is_undefined() || bv.is_symbol()) {
                    ctx.throw_type_error("Cannot convert to BigInt");
                    return Value();
                }
                // Try valueOf for objects
                if (bv.is_object() || bv.is_function()) {
                    Object* obj = bv.is_function() ? static_cast<Object*>(bv.as_function()) : bv.as_object();
                    Value vof = obj->get_property("valueOf");
                    if (!ctx.has_exception() && vof.is_function()) {
                        bv = vof.as_function()->call(ctx, {}, bv);
                        if (ctx.has_exception()) return Value();
                    }
                }
                if (!bv.is_bigint()) { ctx.throw_type_error("Not a BigInt"); return Value(); }
            }
            if (n == 0) return Value(new Quanta::BigInt(0));
            if (n >= 64) {
                int64_t val = bv.as_bigint()->to_int64();
                return Value(new Quanta::BigInt(val));
            }
            int64_t val = bv.as_bigint()->to_int64();
            int64_t mod = 1LL << n;
            int64_t result = val & (mod - 1);
            if (result >= (mod >> 1)) result -= mod;
            return Value(new Quanta::BigInt(result));
        }, 2);
    bigint_fn->set_property("asIntN", Value(asIntN_fn.release()), PropertyAttributes::BuiltinFunction);

    auto asUintN_fn = ObjectFactory::create_native_function("asUintN",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() < 2) { ctx.throw_type_error("BigInt.asUintN requires 2 arguments"); return Value(); }
            if (args[0].is_symbol() || args[0].is_bigint()) { ctx.throw_type_error("Cannot convert Symbol/BigInt to number"); return Value(); }
            double n_raw2 = args[0].to_number();
            if (ctx.has_exception()) return Value();
            double n_d = std::isnan(n_raw2) ? 0.0 : (n_raw2 < 0 ? std::ceil(n_raw2) : std::floor(n_raw2));
            if (n_d < 0 || n_d > 9007199254740991.0 || std::isinf(n_d)) { ctx.throw_range_error("Invalid width"); return Value(); }
            int64_t n = static_cast<int64_t>(n_d);
            Value bv = args[1];
            if (!bv.is_bigint()) {
                if (bv.is_number() || bv.is_null() || bv.is_undefined() || bv.is_symbol()) {
                    ctx.throw_type_error("Cannot convert to BigInt");
                    return Value();
                }
                if (bv.is_object() || bv.is_function()) {
                    Object* obj = bv.is_function() ? static_cast<Object*>(bv.as_function()) : bv.as_object();
                    Value vof = obj->get_property("valueOf");
                    if (!ctx.has_exception() && vof.is_function()) {
                        bv = vof.as_function()->call(ctx, {}, bv);
                        if (ctx.has_exception()) return Value();
                    }
                }
                if (!bv.is_bigint()) { ctx.throw_type_error("Not a BigInt"); return Value(); }
            }
            if (n == 0) return Value(new Quanta::BigInt(0));
            if (n >= 64) {
                int64_t val = bv.as_bigint()->to_int64();
                return Value(new Quanta::BigInt(static_cast<int64_t>(static_cast<uint64_t>(val))));
            }
            int64_t val = bv.as_bigint()->to_int64();
            uint64_t mask = (1ULL << n) - 1;
            uint64_t result = static_cast<uint64_t>(val) & mask;
            return Value(new Quanta::BigInt(std::to_string(result)));
        }, 2);
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

    // Dynamic import() -- module loading with ES module namespace object
    auto import_fn = ObjectFactory::create_native_function("import",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            auto promise_obj = ObjectFactory::create_promise(&ctx);
            auto* promise = dynamic_cast<Quanta::Promise*>(promise_obj.get());
            if (!promise) return Value(promise_obj.release());

            // Coerce specifier with ToString() -- handles objects with custom toString()
            std::string specifier;
            if (!args.empty()) {
                Value sv = args[0];
                if (sv.is_object() || sv.is_function()) {
                    Object* obj = sv.is_object() ? sv.as_object() : static_cast<Object*>(sv.as_function());
                    Value ts = obj->get_property("toString");
                    if (ts.is_function()) {
                        Value res = ts.as_function()->call(ctx, {}, sv);
                        if (ctx.has_exception()) {
                            Value exc = ctx.get_exception();
                            ctx.clear_exception();
                            promise->reject(exc);
                            return Value(promise_obj.release());
                        }
                        specifier = res.to_string();
                    } else {
                        specifier = sv.to_string();
                    }
                } else {
                    specifier = sv.to_string();
                }
            }
            if (specifier.empty()) {
                promise->reject(Value(std::string("TypeError: import() requires a specifier")));
                return Value(promise_obj.release());
            }

            // Deferred to a microtask (HostLoadImportedModule is a job, not inline) -- otherwise the module's top-level side effects would run before the importer's remaining synchronous code.
            std::string current_file = ctx.get_current_filename();
            Engine* engine = ctx.get_engine();
            Promise* promise_ptr = promise;

            Context* queue_ctx = (engine && engine->get_global_context()) ? engine->get_global_context() : &ctx;
            queue_ctx->queue_microtask([specifier, current_file, engine, promise_ptr]() {
                std::string resolved;
                if ((specifier.length() >= 2 && specifier.substr(0, 2) == "./") ||
                    (specifier.length() >= 3 && specifier.substr(0, 3) == "../")) {
                    namespace fs = std::filesystem;
                    std::string base = fs::path(current_file).parent_path().string();
                    if (base.empty()) base = ".";
                    resolved = fs::weakly_canonical(fs::path(base) / specifier).string();
                } else {
                    resolved = specifier;
                }

                if (engine && engine->get_module_loader()) {
                    auto* loader = engine->get_module_loader();
                    Module* mod = loader->load_module(resolved, current_file);
                    if (mod) {
                        if (mod->has_thrown_exception()) {
                            promise_ptr->reject(mod->get_thrown_exception());
                            return;
                        }
                        // Spec 10.4.6: live-binding module namespace (same object each call)
                        Value ns_val = ModuleLoader::build_module_namespace(mod);
                        promise_ptr->fulfill(ns_val);
                        return;
                    }
                    if (loader->has_last_module_exception()) {
                        promise_ptr->reject(loader->get_last_module_exception());
                        return;
                    }
                }

                promise_ptr->reject(Value(std::string("Error: Cannot find module '" + specifier + "'")));
            }, {Value(promise_ptr)});

            return Value(promise_obj.release());
        }, 1);
    ctx.get_global_object()->set_property("import", Value(import_fn.release()));

    // import.source() -- spec EvaluateImportCall with phase=source:
    // Step 6: ToString(specifier) -- IfAbruptRejectPromise if it throws.
    // Step 9: HostLoadImportedModule -> GetModuleSource -> always SyntaxError for SourceTextModuleRecord.
    auto import_source_fn = ObjectFactory::create_native_function("__import_source__",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            auto promise_obj = ObjectFactory::create_promise(&ctx);
            auto* promise = dynamic_cast<Quanta::Promise*>(promise_obj.get());
            if (!promise) return Value(promise_obj.release());
            // Step 6: ToString(specifier) -- reject with its abrupt if it throws.
            // Use the same coercion path as the regular import() handler.
            if (!args.empty()) {
                Value sv = args[0];
                if (sv.is_object() || sv.is_function()) {
                    Object* obj = sv.is_object() ? sv.as_object() : static_cast<Object*>(sv.as_function());
                    Value ts = obj->get_property("toString");
                    if (ctx.has_exception()) {
                        Value exc = ctx.get_exception();
                        ctx.clear_exception();
                        promise->reject(exc);
                        return Value(promise_obj.release());
                    }
                    if (ts.is_function()) {
                        ts.as_function()->call(ctx, {}, sv);
                        if (ctx.has_exception()) {
                            Value exc = ctx.get_exception();
                            ctx.clear_exception();
                            promise->reject(exc);
                            return Value(promise_obj.release());
                        }
                    }
                }
            }
            // Step 9: GetModuleSource for SourceTextModuleRecord always throws SyntaxError
            auto err = Error::create_syntax_error("Module source phase imports are not supported for this module type");
            promise->reject(Value(err.release()));
            return Value(promise_obj.release());
        }, 1);
    ctx.get_global_object()->set_property("__import_source__", Value(import_source_fn.release()));

    // Class field init context flags -- set/clear in_class_field_init_ around each field initializer
    // so that direct eval inside a class field initializer enforces the ContainsArguments early error.
    auto cfi_enter_fn = ObjectFactory::create_native_function("__cfi_enter__",
        [](Context& ctx, const std::vector<Value>&) -> Value {
            ctx.set_in_class_field_init(true);
            return Value();
        }, 0);
    ctx.get_global_object()->set_property("__cfi_enter__", Value(cfi_enter_fn.release()));

    auto cfi_exit_fn = ObjectFactory::create_native_function("__cfi_exit__",
        [](Context& ctx, const std::vector<Value>&) -> Value {
            ctx.set_in_class_field_init(false);
            return Value();
        }, 0);
    ctx.get_global_object()->set_property("__cfi_exit__", Value(cfi_exit_fn.release()));

    // __pfadd__(obj, name [, value]): PrivateFieldAdd - adds a private field slot to obj.
    // Used by class field declarations so the slot exists before user assignments check it.
    auto pfadd_fn = ObjectFactory::create_native_function("__pfadd__",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() < 2 || !args[0].is_object()) return Value();
            Object* obj = args[0].as_object();
            std::string name = args[1].to_string();
            Value val = args.size() >= 3 ? args[2] : Value();
            // Qualify by declaring class so a base and derived class's same-named field don't collide into one slot.
            obj->add_private_field(resolve_private_storage_key(name, obj), val);
            return Value();
        }, 2);
    ctx.get_global_object()->set_property("__pfadd__", Value(pfadd_fn.release()));

    // __setfnname__(value, name): SetFunctionName -- used by class field initializers
    // (DefineField step 7) to name an otherwise-anonymous function/class expression
    // after the field it initializes. Only static/instance field values get this
    // treatment (not generic member assignment), so it's applied explicitly here
    // rather than folded into AssignmentExpression's identifier-only naming check.
    auto setfnname_fn = ObjectFactory::create_native_function("__setfnname__",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value();
            if (args.size() >= 2 && args[0].is_function()) {
                Function* fn = args[0].as_function();
                if (fn->get_name().empty() || fn->get_name() == "<arrow>") {
                    fn->set_name(args[1].to_string());
                }
            }
            return args[0];
        }, 2);
    ctx.get_global_object()->set_property("__setfnname__", Value(setfnname_fn.release()));

    // __deffield__(obj, name, value): DefineField step 9 for a PUBLIC (non-private) field --
    // CreateDataPropertyOrThrow defines an OWN data property directly and must NOT walk the
    // prototype chain for an inherited setter (unlike a normal `this.x = value` assignment).
    auto deffield_fn = ObjectFactory::create_native_function("__deffield__",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() < 3 || !args[0].is_object()) return Value();
            Object* obj = args[0].as_object();
            std::string key = args[1].to_string();
            PropertyDescriptor fdesc(args[2], static_cast<PropertyAttributes>(
                PropertyAttributes::Writable | PropertyAttributes::Enumerable | PropertyAttributes::Configurable));
            if (!obj->set_property_descriptor(key, fdesc)) {
                // A Proxy's defineProperty trap may have already thrown (e.g. invariant
                // violation, or the handler itself threw) -- don't clobber that with a
                // generic error if so.
                if (!ctx.has_exception()) {
                    ctx.throw_type_error("Cannot define field '" + key + "'");
                }
            }
            return Value();
        }, 3);
    ctx.get_global_object()->set_property("__deffield__", Value(deffield_fn.release()));

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
            (void)args;
            Heap* heap = ctx.get_engine() ? ctx.get_engine()->get_heap() : nullptr;
            auto stats_obj = ObjectFactory::create_object();
            if (heap) {
                Heap::Stats s = heap->stats();
                stats_obj->set_property("liveCells", Value(static_cast<double>(s.live_cells)));
                stats_obj->set_property("liveBytes", Value(static_cast<double>(s.live_bytes)));
                stats_obj->set_property("blocks", Value(static_cast<double>(s.block_count)));
                stats_obj->set_property("chunks", Value(static_cast<double>(s.chunk_count)));
                stats_obj->set_property("largeCells", Value(static_cast<double>(s.large_count)));
                stats_obj->set_property("largeBytes", Value(static_cast<double>(s.large_bytes)));
                stats_obj->set_property("lastMarked", Value(static_cast<double>(Collector::last_cycle().marked_cells)));
                stats_obj->set_property("lastSwept", Value(static_cast<double>(Collector::last_cycle().swept_cells)));
            }
            return Value(stats_obj.release());
        }, 0);

    auto gc_obj_collect_fn = ObjectFactory::create_native_function("collect",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx; (void)args;
            Collector::collect();
            return Value();
        }, 0);

    auto gc_obj_minor_fn = ObjectFactory::create_native_function("minor",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx; (void)args;
            Collector::collect_minor();
            return Value();
        }, 0);

    auto gc_obj_heap_size_fn = ObjectFactory::create_native_function("heapSize",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Heap* heap = ctx.get_engine() ? ctx.get_engine()->get_heap() : nullptr;
            if (!heap) return Value();
            Heap::Stats s = heap->stats();
            return Value(static_cast<double>(s.live_bytes + s.large_bytes));
        }, 0);

    gc_obj->set_property("stats", Value(gc_obj_stats_fn.release()), PropertyAttributes::BuiltinFunction);
    gc_obj->set_property("collect", Value(gc_obj_collect_fn.release()), PropertyAttributes::BuiltinFunction);
    gc_obj->set_property("minor", Value(gc_obj_minor_fn.release()), PropertyAttributes::BuiltinFunction);
    gc_obj->set_property("heapSize", Value(gc_obj_heap_size_fn.release()), PropertyAttributes::BuiltinFunction);

    ctx.get_lexical_environment()->create_binding("gc", Value(gc_obj.release()), false);


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
    
    auto schedule_timer_fn = [](Context& ctx, const std::vector<Value>& args, bool repeating) -> Value {
        if (args.empty() || !args[0].is_function()) {
            // Non-callable first arg: HTML-spec-leniency no-op rather than throw.
            return Value(static_cast<double>(0));
        }
        Function* cb = args[0].as_function();
        double delay = args.size() > 1 ? args[1].to_number() : 0.0;
        if (std::isnan(delay) || delay < 0) delay = 0.0;
        std::vector<Value> bound(args.size() > 2 ? args.begin() + 2 : args.end(), args.end());
        int64_t id = EventLoop::instance().schedule_timer(ctx, cb, std::move(bound), delay, repeating);
        return Value(static_cast<double>(id));
    };
    auto setTimeout_fn = ObjectFactory::create_native_function("setTimeout",
        [schedule_timer_fn](Context& ctx, const std::vector<Value>& args) -> Value {
            return schedule_timer_fn(ctx, args, false);
        });
    auto setInterval_fn = ObjectFactory::create_native_function("setInterval",
        [schedule_timer_fn](Context& ctx, const std::vector<Value>& args) -> Value {
            return schedule_timer_fn(ctx, args, true);
        });
    auto clear_timer_fn = [](Context& ctx, const std::vector<Value>& args) -> Value {
        if (!args.empty()) {
            EventLoop::instance().clear_timer(static_cast<int64_t>(args[0].to_number()));
        }
        return Value();
    };
    auto clearTimeout_fn = ObjectFactory::create_native_function("clearTimeout", clear_timer_fn);
    auto clearInterval_fn = ObjectFactory::create_native_function("clearInterval", clear_timer_fn);
    
    ctx.get_lexical_environment()->create_binding("setTimeout", Value(setTimeout_fn.release()), false);
    ctx.get_lexical_environment()->create_binding("setInterval", Value(setInterval_fn.release()), false);
    ctx.get_lexical_environment()->create_binding("clearTimeout", Value(clearTimeout_fn.release()), false);
    ctx.get_lexical_environment()->create_binding("clearInterval", Value(clearInterval_fn.release()), false);

    // setImmediate/clearImmediate: a zero-delay, one-shot timer (Node API, not in any web spec).
    auto setImmediate_fn = ObjectFactory::create_native_function("setImmediate",
        [schedule_timer_fn](Context& ctx, const std::vector<Value>& args) -> Value {
            std::vector<Value> shifted;
            shifted.push_back(args.empty() ? Value() : args[0]);
            shifted.push_back(Value(0.0));
            for (size_t i = 1; i < args.size(); i++) shifted.push_back(args[i]);
            return schedule_timer_fn(ctx, shifted, false);
        });
    auto clearImmediate_fn = ObjectFactory::create_native_function("clearImmediate", clear_timer_fn);
    ctx.get_lexical_environment()->create_binding("setImmediate", Value(setImmediate_fn.release()), false);
    ctx.get_lexical_environment()->create_binding("clearImmediate", Value(clearImmediate_fn.release()), false);

    // queueMicrotask: unlike setTimeout, a non-callable callback is a TypeError, not a silent no-op.
    // Exceptions are reported as uncaught, not propagated, since the caller's turn already ended.
    auto queueMicrotask_fn = ObjectFactory::create_native_function("queueMicrotask",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].is_function()) {
                ctx.throw_type_error("queueMicrotask: callback must be a function");
                return Value();
            }
            Function* cb = args[0].as_function();
            Context* call_ctx = &ctx;
            Engine* engine = ctx.get_engine();
            Context* queue_ctx = (engine && engine->get_global_context()) ? engine->get_global_context() : call_ctx;

            queue_ctx->queue_microtask([call_ctx, cb]() {
                cb->call(*call_ctx, {});
                if (call_ctx->has_exception()) {
                    Value exc = call_ctx->get_exception();
                    call_ctx->clear_exception();
                    std::cerr << "Uncaught (in queueMicrotask) " << exc.to_string() << std::endl;
                }
            }, {Value(cb)});
            return Value();
        }, 1);
    ctx.get_lexical_environment()->create_binding("queueMicrotask", Value(queueMicrotask_fn.release()), false);


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
