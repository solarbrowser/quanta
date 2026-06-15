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
#include <unordered_set>
#include "quanta/core/runtime/Iterator.h"
#include "quanta/core/runtime/Promise.h"
#include "quanta/core/modules/ModuleLoader.h"
#include <filesystem>
#include "quanta/core/runtime/BigInt.h"
#include "quanta/core/runtime/String.h"
#include "quanta/core/runtime/Symbol.h"

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
    ctx.get_lexical_environment()->create_binding("parseInt", Value(parseInt_fn.release()), true);

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
    ctx.get_lexical_environment()->create_binding("parseFloat", Value(parseFloat_fn.release()), true);

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
    ctx.get_lexical_environment()->create_binding("isNaN", Value(isNaN_global_fn.release()), true);

    auto isFinite_global_fn = ObjectFactory::create_native_function("isFinite",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value(false);
            double num = args[0].to_number();
            return Value(std::isfinite(num));
        }, 1);
    ctx.get_lexical_environment()->create_binding("isFinite", Value(isFinite_global_fn.release()), true);

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
                    eval_ctx.set_this_binding(ctx.get_this_binding());

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
                    if (is_direct) {
                        if (lex_env) {
                            for (const auto& vname : var_names) {
                                if (lex_env->has_lexical_declaration(vname)) {
                                    ctx.throw_syntax_error("Variable '" + vname + "' has already been declared");
                                    return Value();
                                }
                            }
                        }
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

                    // When eval runs inside default parameter expressions, var arguments conflicts
                    // with the implicit arguments binding (or an explicit arguments parameter)
                    if (ctx.is_in_param_eval() && ctx.has_eval_arguments_conflict()) {
                        for (const auto& vname : var_names) {
                            if (vname == "arguments") {
                                ctx.throw_syntax_error("Variable 'arguments' has already been declared");
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
    ctx.get_lexical_environment()->create_binding("eval", Value(eval_fn.release()), true);

    ctx.get_lexical_environment()->create_binding("undefined", Value(), false, false);
    ctx.get_lexical_environment()->create_binding("null", Value::null(), false, false);
    
    if (ctx.get_global_object()) {
        ctx.get_lexical_environment()->create_binding("globalThis", Value(ctx.get_global_object()), true);
        ctx.get_lexical_environment()->create_binding("global", Value(ctx.get_global_object()), true);
        ctx.get_lexical_environment()->create_binding("window", Value(ctx.get_global_object()), true);
        ctx.get_lexical_environment()->create_binding("this", Value(ctx.get_global_object()), true);

        PropertyDescriptor global_ref_desc(Value(ctx.get_global_object()), PropertyAttributes::BuiltinFunction);
        ctx.get_global_object()->set_property_descriptor("globalThis", global_ref_desc);
        ctx.get_global_object()->set_property_descriptor("global", global_ref_desc);
        ctx.get_global_object()->set_property_descriptor("window", global_ref_desc);
        ctx.get_global_object()->set_property_descriptor("this", global_ref_desc);
    }
    ctx.get_lexical_environment()->create_binding("true", Value(true), false, false);
    ctx.get_lexical_environment()->create_binding("false", Value(false), false, false);
    
    ctx.get_lexical_environment()->create_binding("NaN", Value::nan(), false, false);
    ctx.get_lexical_environment()->create_binding("Infinity", Value::positive_infinity(), false, false);

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
    ctx.get_lexical_environment()->create_binding("encodeURI", Value(encode_uri_fn.release()), true);

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
    ctx.get_lexical_environment()->create_binding("decodeURI", Value(decode_uri_fn.release()), true);

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
    ctx.get_lexical_environment()->create_binding("encodeURIComponent", Value(encode_uri_component_fn.release()), true);

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
    ctx.get_lexical_environment()->create_binding("decodeURIComponent", Value(decode_uri_component_fn.release()), true);
    
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

            // Resolve path relative to current file
            std::string current_file = ctx.get_current_filename();
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

            Engine* engine = ctx.get_engine();
            if (engine && engine->get_module_loader()) {
                auto* loader = engine->get_module_loader();
                Module* mod = loader->load_module(resolved, current_file);
                if (mod) {
                    if (mod->has_thrown_exception()) {
                        promise->reject(mod->get_thrown_exception());
                        return Value(promise_obj.release());
                    }
                    // Spec 10.4.6: live-binding module namespace (same object each call)
                    Value ns_val = ModuleLoader::build_module_namespace(mod);
                    promise->fulfill(ns_val);
                    return Value(promise_obj.release());
                }
                if (loader->has_last_module_exception()) {
                    promise->reject(loader->get_last_module_exception());
                    return Value(promise_obj.release());
                }
            }

            promise->reject(Value(std::string("Error: Cannot find module '" + specifier + "'")));
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

    // __pfadd__(obj, name [, value]): PrivateFieldAdd - adds a private field slot to obj.
    // Used by class field declarations so the slot exists before user assignments check it.
    auto pfadd_fn = ObjectFactory::create_native_function("__pfadd__",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() < 2 || !args[0].is_object()) return Value();
            Object* obj = args[0].as_object();
            std::string name = args[1].to_string();
            Value val = args.size() >= 3 ? args[2] : Value();
            obj->add_private_field(name, val);
            return Value();
        }, 2);
    ctx.get_global_object()->set_property("__pfadd__", Value(pfadd_fn.release()));

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
