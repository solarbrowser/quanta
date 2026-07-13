/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/parser/AST.h"
#include "quanta/core/gc/Collector.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/engine/Engine.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/Async.h"
#include "quanta/core/runtime/Promise.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/core/runtime/Generator.h"
#include "quanta/core/runtime/Iterator.h"
#include "quanta/core/runtime/ProxyReflect.h"
#include "../ast_internal.h"
#include "quanta/core/engine/CallStack.h"
#include <algorithm>
#include <sstream>
#include <iostream>
#include <unordered_set>

#ifdef __GNUC__
    #define LIKELY(x) __builtin_expect(!!(x), 1)
    #define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
    #define LIKELY(x) (x)
    #define UNLIKELY(x) (x)
#endif

namespace Quanta {

// Thread-local flag for "empty completion" propagation (spec UpdateEmpty semantics).
// Set to true by statements that produce empty completions (UsingDeclaration, VariableDeclaration).
// Cleared by statements that produce real completions (ExpressionStatement, etc.).
// BlockStatement and Program use this to implement UpdateEmpty.
thread_local bool g_empty_completion = false;

static bool is_anon_func_def(const ASTNode* node) {
    if (!node) return false;
    auto t = node->get_type();
    return t == ASTNode::Type::FUNCTION_EXPRESSION ||
           t == ASTNode::Type::ARROW_FUNCTION_EXPRESSION ||
           t == ASTNode::Type::ASYNC_FUNCTION_EXPRESSION ||
           t == ASTNode::Type::CLASS_DECLARATION;
}

Value ExpressionStatement::evaluate(Context& ctx) {
    Value result = expression_->evaluate(ctx);
    g_empty_completion = false;
    if (ctx.has_exception()) {
        return Value();
    }
    return result;
}

std::string ExpressionStatement::to_string() const {
    return expression_->to_string() + ";";
}

std::unique_ptr<ASTNode> ExpressionStatement::clone() const {
    return std::make_unique<ExpressionStatement>(expression_->clone(), start_, end_);
}


Value EmptyStatement::evaluate(Context& ctx) {
    return Value();
}

std::string EmptyStatement::to_string() const {
    return ";";
}

std::unique_ptr<ASTNode> EmptyStatement::clone() const {
    return std::make_unique<EmptyStatement>(start_, end_);
}


Value LabeledStatement::evaluate(Context& ctx) {
    ctx.set_next_statement_label(label_);
    Value result = statement_->evaluate(ctx);
    ctx.set_next_statement_label("");

    if (ctx.has_break() && ctx.get_break_label() == label_) {
        ctx.clear_break_continue();
    }
    if (ctx.has_continue() && ctx.get_continue_label() == label_) {
        ctx.clear_break_continue();
    }

    return result;
}

std::string LabeledStatement::to_string() const {
    return label_ + ": " + statement_->to_string();
}

std::unique_ptr<ASTNode> LabeledStatement::clone() const {
    return std::make_unique<LabeledStatement>(
        label_,
        statement_->clone(),
        start_,
        end_
    );
}


Value Program::evaluate(Context& ctx) {
    Object::current_context_ = &ctx;

    Value last_value;

    if (is_strict_) {
        ctx.set_strict_mode(true);
    }
    check_use_strict_directive(ctx);

    hoist_var_declarations(ctx);
    // Eval contexts have their lexical env set up by the caller (GlobalsBuiltin);
    // only hoist for top-level scripts and module code.
    if (ctx.get_type() != Context::Type::Eval) {
        hoist_lexical_declarations(ctx);
    }

    // Hoist function declarations AFTER pushing the script-level lexical env so
    // that function closures can access let/const bindings from the same script.
    for (const auto& statement : statements_) {
        if (statement->get_type() == ASTNode::Type::FUNCTION_DECLARATION) {
            last_value = statement->evaluate(ctx);
            if (ctx.has_exception()) {
                return Value();
            }
        }
    }

    for (const auto& statement : statements_) {
        if (statement->get_type() != ASTNode::Type::FUNCTION_DECLARATION) {
            Collector::safepoint();
            g_empty_completion = false;
            Value result = statement->evaluate(ctx);
            if (!g_empty_completion) last_value = result;
            if (ctx.has_exception()) {
                return Value();
            }
        }
    }

    return last_value;
}

void Program::hoist_var_declarations(Context& ctx) {
    for (const auto& statement : statements_) {
        scan_for_var_declarations(statement.get(), ctx);
    }
}

void Program::hoist_lexical_declarations(Context& ctx) {
    // Create a script-level declarative environment for let/const TDZ bindings.
    // ES6 spec: global let/const live in a separate declarative environment, not
    // on the global object. This allows TDZ to work for let/const declared later
    // in the script that are accessed before their declaration point.
    Environment* old_lex = ctx.get_lexical_environment();
    auto script_env = std::make_unique<Environment>(Environment::Type::Declarative, old_lex);
    Environment* script_env_ptr = script_env.release();
    script_env_ptr->mark_closure_boundary();
    ctx.set_lexical_environment(script_env_ptr);

    for (const auto& statement : statements_) {
        // `export let x`, `export const x`, `export class X {}` wrap the
        // real declaration -- the binding TDZ applies to it either way.
        const ASTNode* effective = statement.get();
        if (effective->get_type() == ASTNode::Type::EXPORT_STATEMENT) {
            const auto* ex = static_cast<const ExportStatement*>(effective);
            if (!ex->is_declaration_export()) continue;
            effective = ex->get_declaration();
            if (!effective) continue;
        }
        if (effective->get_type() == ASTNode::Type::VARIABLE_DECLARATION) {
            auto* vd = static_cast<const VariableDeclaration*>(effective);
            if (vd->get_kind() == VariableDeclarator::Kind::LET ||
                    vd->get_kind() == VariableDeclarator::Kind::CONST) {
                for (const auto& decl : vd->get_declarations()) {
                    if (decl->get_id() && !decl->get_id()->get_name().empty()) {
                        const std::string& bname = decl->get_id()->get_name();
                        bool is_const = (vd->get_kind() == VariableDeclarator::Kind::CONST);
                        script_env_ptr->create_uninitialized_binding(bname, !is_const);
                        script_env_ptr->mark_lexical_declaration(bname);
                        if (is_const) {
                            script_env_ptr->mark_const_binding(bname);
                        }
                    }
                }
            }
        } else if (effective->get_type() == ASTNode::Type::CLASS_DECLARATION) {
            // Lexical (TDZ until it runs) but always mutable, same as `let`.
            auto* cd = static_cast<const ClassDeclaration*>(effective);
            if (cd->get_id() && !cd->get_id()->get_name().empty()) {
                const std::string& bname = cd->get_id()->get_name();
                script_env_ptr->create_uninitialized_binding(bname, true);
                script_env_ptr->mark_lexical_declaration(bname);
            }
        }
    }
}

void Program::scan_for_var_declarations(ASTNode* node, Context& ctx) {
    if (!node) return;

    if (node->get_type() == ASTNode::Type::VARIABLE_DECLARATION) {
        VariableDeclaration* var_decl = static_cast<VariableDeclaration*>(node);

        for (const auto& declarator : var_decl->get_declarations()) {
            if (declarator->get_kind() == VariableDeclarator::Kind::VAR) {
                const std::string& name = declarator->get_id()->get_name();

                if (!ctx.has_binding(name)) {
                    ctx.create_var_binding(name, Value(), true);
                }
            }
        }
    }

    if (node->get_type() == ASTNode::Type::BLOCK_STATEMENT) {
        BlockStatement* block = static_cast<BlockStatement*>(node);
        for (const auto& stmt : block->get_statements()) {
            scan_for_var_declarations(stmt.get(), ctx);
        }
    }
    else if (node->get_type() == ASTNode::Type::IF_STATEMENT) {
        IfStatement* if_stmt = static_cast<IfStatement*>(node);
        scan_for_var_declarations(if_stmt->get_consequent(), ctx);
        if (if_stmt->get_alternate()) {
            scan_for_var_declarations(if_stmt->get_alternate(), ctx);
        }
    }
    else if (node->get_type() == ASTNode::Type::FOR_STATEMENT) {
        ForStatement* for_stmt = static_cast<ForStatement*>(node);
        if (for_stmt->get_init()) {
            scan_for_var_declarations(for_stmt->get_init(), ctx);
        }
        scan_for_var_declarations(for_stmt->get_body(), ctx);
    }
    else if (node->get_type() == ASTNode::Type::WHILE_STATEMENT) {
        WhileStatement* while_stmt = static_cast<WhileStatement*>(node);
        scan_for_var_declarations(while_stmt->get_body(), ctx);
    }
    else if (node->get_type() == ASTNode::Type::DO_WHILE_STATEMENT) {
        DoWhileStatement* do_stmt = static_cast<DoWhileStatement*>(node);
        scan_for_var_declarations(do_stmt->get_body(), ctx);
    }
    else if (node->get_type() == ASTNode::Type::WITH_STATEMENT) {
        WithStatement* with_stmt = static_cast<WithStatement*>(node);
        scan_for_var_declarations(with_stmt->get_body(), ctx);
    }
    else if (node->get_type() == ASTNode::Type::TRY_STATEMENT) {
        TryStatement* try_stmt = static_cast<TryStatement*>(node);
        scan_for_var_declarations(try_stmt->get_try_block(), ctx);
        if (try_stmt->get_catch_clause()) scan_for_var_declarations(try_stmt->get_catch_clause(), ctx);
        if (try_stmt->get_finally_block()) scan_for_var_declarations(try_stmt->get_finally_block(), ctx);
    }
    else if (node->get_type() == ASTNode::Type::SWITCH_STATEMENT) {
        SwitchStatement* sw = static_cast<SwitchStatement*>(node);
        for (const auto& c : sw->get_cases()) {
            for (const auto& s : static_cast<CaseClause*>(c.get())->get_consequent()) {
                scan_for_var_declarations(s.get(), ctx);
            }
        }
    }
    else if (node->get_type() == ASTNode::Type::LABELED_STATEMENT) {
        LabeledStatement* lbl = static_cast<LabeledStatement*>(node);
        scan_for_var_declarations(lbl->get_statement(), ctx);
    }
    else if (node->get_type() == ASTNode::Type::FOR_IN_STATEMENT) {
        ForInStatement* forin = static_cast<ForInStatement*>(node);
        if (forin->get_left()) scan_for_var_declarations(forin->get_left(), ctx);
        scan_for_var_declarations(forin->get_body(), ctx);
    }
    else if (node->get_type() == ASTNode::Type::FOR_OF_STATEMENT) {
        ForOfStatement* forof = static_cast<ForOfStatement*>(node);
        if (forof->get_left()) scan_for_var_declarations(forof->get_left(), ctx);
        scan_for_var_declarations(forof->get_body(), ctx);
    }
    else if (node->get_type() == ASTNode::Type::CATCH_CLAUSE) {
        CatchClause* cc = static_cast<CatchClause*>(node);
        scan_for_var_declarations(cc->get_body(), ctx);
    }
}

std::string Program::to_string() const {
    std::ostringstream oss;
    for (const auto& statement : statements_) {
        oss << statement->to_string() << "\n";
    }
    return oss.str();
}

std::unique_ptr<ASTNode> Program::clone() const {
    std::vector<std::unique_ptr<ASTNode>> cloned_statements;
    for (const auto& statement : statements_) {
        cloned_statements.push_back(statement->clone());
    }
    return std::make_unique<Program>(std::move(cloned_statements), start_, end_);
}

void Program::check_use_strict_directive(Context& ctx) {
    for (const auto& stmt : statements_) {
        if (stmt->get_type() != ASTNode::Type::EXPRESSION_STATEMENT) break;
        auto* expr_stmt = static_cast<ExpressionStatement*>(stmt.get());
        auto* expr = expr_stmt->get_expression();
        if (!expr || expr->get_type() != ASTNode::Type::STRING_LITERAL) break;
        auto* sl = static_cast<StringLiteral*>(expr);
        if (sl->get_value() == "use strict" && !sl->has_escapes()) {
            ctx.set_strict_mode(true);
            return;
        }
    }
}


Value VariableDeclarator::evaluate(Context& ctx) {
    (void)ctx;
    return Value();
}

std::string VariableDeclarator::to_string() const {
    std::string result = id_->get_name();
    if (init_) {
        result += " = " + init_->to_string();
    }
    return result;
}

std::unique_ptr<ASTNode> VariableDeclarator::clone() const {
    std::unique_ptr<ASTNode> cloned_init = init_ ? init_->clone() : nullptr;
    return std::make_unique<VariableDeclarator>(
        std::unique_ptr<Identifier>(static_cast<Identifier*>(id_->clone().release())),
        std::move(cloned_init), kind_, start_, end_
    );
}

std::string VariableDeclarator::kind_to_string(Kind kind) {
    switch (kind) {
        case Kind::VAR: return "var";
        case Kind::LET: return "let";
        case Kind::CONST: return "const";
        default: return "var";
    }
}


Value VariableDeclaration::evaluate(Context& ctx) {
    for (const auto& declarator : declarations_) {
        const std::string& name = declarator->get_id()->get_name();

        if (name.empty() && declarator->get_init()) {
            ASTNode* init_node = declarator->get_init();
            VariableDeclarator::Kind destr_kind = declarator->get_kind();
            bool is_lex_decl = (destr_kind == VariableDeclarator::Kind::LET ||
                                destr_kind == VariableDeclarator::Kind::CONST);
            bool is_const_decl = destr_kind == VariableDeclarator::Kind::CONST;

            // Evaluate the source and call evaluate_with_value directly
            // (instead of init_node->evaluate(), which always takes the
            // var-like default path) so let/const destructuring gets a
            // fresh block-scoped binding instead of leaking to the function.
            if (init_node->get_type() == ASTNode::Type::DESTRUCTURING_ASSIGNMENT) {
                auto* da = static_cast<DestructuringAssignment*>(init_node);
                Value src = da->get_source()->evaluate(ctx);
                if (ctx.has_exception()) return Value();
                da->evaluate_with_value(ctx, src, is_lex_decl, is_const_decl);
                if (ctx.has_exception()) return Value();
                continue;
            }

            Value result = init_node->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            ASTNode* id_node = declarator->get_id();
            ASTNode::Type id_type = id_node ? id_node->get_type() : ASTNode::Type::IDENTIFIER;

            // Pre-create lexical bindings so let/const destructuring lands in this block scope instead of leaking via set_binding's outer-scope walk.
            if (is_lex_decl) {
                std::function<void(ASTNode*)> collect_and_bind = [&](ASTNode* node) {
                    if (!node) return;
                    auto t = node->get_type();
                    if (t == ASTNode::Type::IDENTIFIER) {
                        auto* id = static_cast<Identifier*>(node);
                        if (!id->get_name().empty())
                            ctx.create_lexical_binding(id->get_name(), Value(),
                                destr_kind == VariableDeclarator::Kind::LET);
                    } else if (t == ASTNode::Type::ARRAY_LITERAL) {
                        for (auto& elem : static_cast<ArrayLiteral*>(node)->get_elements())
                            if (elem) collect_and_bind(elem.get());
                    } else if (t == ASTNode::Type::OBJECT_LITERAL) {
                        for (auto& prop : static_cast<ObjectLiteral*>(node)->get_properties())
                            if (prop && prop->value) collect_and_bind(prop->value.get());
                    } else if (t == ASTNode::Type::ASSIGNMENT_EXPRESSION) {
                        collect_and_bind(static_cast<AssignmentExpression*>(node)->get_left());
                    } else if (t == ASTNode::Type::SPREAD_ELEMENT) {
                        collect_and_bind(static_cast<SpreadElement*>(node)->get_argument());
                    }
                };
                collect_and_bind(id_node);
                if (ctx.has_exception()) return Value();
            }

            if (id_type == ASTNode::Type::OBJECT_LITERAL || id_type == ASTNode::Type::ARRAY_LITERAL) {
                AssignmentExpression::destructuring_assign(ctx, id_node, result);
                if (ctx.has_exception()) return Value();
            } else if (id_type == ASTNode::Type::DESTRUCTURING_ASSIGNMENT) {
                DestructuringAssignment* da = static_cast<DestructuringAssignment*>(id_node);
                da->evaluate_with_value(ctx, result, is_lex_decl, is_const_decl);
                if (ctx.has_exception()) return Value();
            }
            continue;
        }

        bool mutable_binding = (declarator->get_kind() != VariableDeclarator::Kind::CONST);
        VariableDeclarator::Kind kind = declarator->get_kind();

        // ResolveBinding(name) may pass through a shadowing `with` object on the way to this VariableEnvironment, but must stop there -- walking past it into an ancestor scope's own same-named var would wrongly treat an un-hoisted var (e.g. one a direct eval introduces on the fly) as already declared there instead of creating a local one here.
        Environment* var_env_here = ctx.get_variable_environment();
        auto find_bounded_binding_env = [&]() -> Environment* {
            Environment* env = ctx.get_lexical_environment();
            while (env) {
                if (env->has_own_binding(name)) return env;
                if (env == var_env_here) return nullptr;
                env = env->get_outer();
            }
            return nullptr;
        };

        // Spec: ResolveBinding(name) before evaluating initializer (binding-resolution rule).
        // Capture the environment containing this binding so that initializers that
        // modify the scope (eval, delete in with-scope) don't change the write target.
        Environment* ref_env = nullptr;
        if (kind == VariableDeclarator::Kind::VAR && declarator->get_init()) {
            ref_env = find_bounded_binding_env();
        }

        Value init_value;
        if (declarator->get_init()) {
            // NamedEvaluation: static initializers observe the class name via
            // this.name during evaluation, so it must be inferred beforehand.
            if (declarator->get_init()->get_type() == ASTNode::Type::CLASS_DECLARATION) {
                auto* cd = static_cast<ClassDeclaration*>(declarator->get_init());
                if (cd->is_expression() && cd->get_id() && cd->get_id()->get_name().empty()) {
                    cd->set_inferred_name(name);
                }
            }
            init_value = declarator->get_init()->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            if (init_value.is_function() && is_anon_func_def(declarator->get_init())) {
                Function* fn = init_value.as_function();
                if (fn->get_name().empty() || fn->get_name() == "<arrow>") {
                    fn->set_name(name);
                }
            }
        } else {
            init_value = Value();
        }

        // Re-checked after the initializer ran, since its side effects (e.g. a nested eval) may have just created the binding.
        bool has_local = kind == VariableDeclarator::Kind::VAR && find_bounded_binding_env() != nullptr;

        if (has_local) {
            if (kind == VariableDeclarator::Kind::VAR) {
                if (declarator->get_init()) {
                    if (ref_env && ref_env->get_type() == Environment::Type::Object &&
                        ref_env->get_binding_object()) {
                        // PutValue to original object env binding object (even if property was deleted)
                        ref_env->get_binding_object()->set_property(name, init_value);
                    } else {
                        ctx.set_binding(name, init_value);
                    }
                }
            } else {
                ctx.throw_exception(Value("SyntaxError: Identifier '" + name + "' has already been declared"));
                return Value();
            }
        } else {
            bool success = false;

            if (kind == VariableDeclarator::Kind::VAR) {
                success = ctx.create_var_binding(name, init_value, mutable_binding);
            } else {
                // If a pre-instantiated TDZ binding exists, initialize it instead of failing
                Environment* lex_env = ctx.get_lexical_environment();
                if (lex_env && lex_env->has_own_binding(name) && !lex_env->is_initialized_binding(name)) {
                    lex_env->initialize_binding(name, init_value);
                    lex_env->mark_lexical_declaration(name);
                    success = true;
                } else {
                    success = ctx.create_lexical_binding(name, init_value, mutable_binding);
                }
            }

            if (!success) {
                ctx.throw_exception(Value("Variable '" + name + "' already declared"));
                return Value();
            }
        }
    }

    g_empty_completion = true;
    return Value();
}

std::string VariableDeclaration::to_string() const {
    std::ostringstream oss;
    oss << VariableDeclarator::kind_to_string(kind_) << " ";
    for (size_t i = 0; i < declarations_.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << declarations_[i]->to_string();
    }
    oss << ";";
    return oss.str();
}

std::unique_ptr<ASTNode> VariableDeclaration::clone() const {
    std::vector<std::unique_ptr<VariableDeclarator>> cloned_declarations;
    for (const auto& decl : declarations_) {
        cloned_declarations.push_back(
            std::unique_ptr<VariableDeclarator>(static_cast<VariableDeclarator*>(decl->clone().release()))
        );
    }
    return std::make_unique<VariableDeclaration>(std::move(cloned_declarations), kind_, start_, end_);
}


// GetDisposeMethod + AddDisposableResource: registers `val` for disposal in the current dispose
// scope. null/undefined is a no-op for plain `using`, but still records a no-op resource for
// `await using`'s mandatory Await(undefined) tick.
static bool register_disposable_resource(Context& ctx, const Value& val, bool is_await) {
    if (val.is_null() || val.is_undefined()) {
        if (is_await) ctx.add_disposable_resource(Value(), Value(), true);
        return true;
    }
    if (!val.is_object() && !val.is_function()) {
        ctx.throw_type_error("using declarations require the value to be an object or null/undefined");
        return false;
    }
    Object* obj = val.is_object() ? val.as_object() : static_cast<Object*>(val.as_function());
    Value dispose_method;
    if (is_await) {
        dispose_method = obj->get_property(Symbol::ASYNC_DISPOSE);
        if (ctx.has_exception()) return false;
        if (dispose_method.is_undefined() || dispose_method.is_null()) {
            dispose_method = obj->get_property(Symbol::DISPOSE);
        }
    } else {
        dispose_method = obj->get_property(Symbol::DISPOSE);
    }
    if (ctx.has_exception()) return false;
    if (dispose_method.is_undefined() || dispose_method.is_null()) {
        ctx.throw_type_error(is_await ? "Value must have a [Symbol.asyncDispose] or [Symbol.dispose] method"
                                       : "Value must have a [Symbol.dispose] method");
        return false;
    }
    if (!dispose_method.is_function()) {
        ctx.throw_type_error("Value's [Symbol.dispose] is not callable");
        return false;
    }
    ctx.add_disposable_resource(val, dispose_method, is_await);
    return true;
}

Value UsingDeclaration::evaluate(Context& ctx) {
    for (const auto& binding : bindings_) {
        if (!binding.initializer) {
            ctx.throw_syntax_error("'using' declaration must have an initializer");
            return Value();
        }

        Value val = binding.initializer->evaluate(ctx);
        if (ctx.has_exception()) return Value();

        // NamedEvaluation: only for IsAnonymousFunctionDefinition nodes
        if (val.is_function() && is_anon_func_def(binding.initializer.get())) {
            Function* fn = val.as_function();
            if (fn->get_name().empty() || fn->get_name() == "<arrow>") {
                fn->set_name(binding.name);
            }
        }

        if (!register_disposable_resource(ctx, val, is_await_)) return Value();

        bool success = ctx.create_lexical_binding(binding.name, val, false);
        if (!success) {
            ctx.throw_exception(Value(std::string("Variable '") + binding.name + "' already declared"));
            return Value();
        }
    }
    g_empty_completion = true;
    return Value();
}

std::string UsingDeclaration::to_string() const {
    std::ostringstream oss;
    oss << (is_await_ ? "await using " : "using ");
    for (size_t i = 0; i < bindings_.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << bindings_[i].name;
        if (bindings_[i].initializer) oss << " = " << bindings_[i].initializer->to_string();
    }
    oss << ";";
    return oss.str();
}

std::unique_ptr<ASTNode> UsingDeclaration::clone() const {
    std::vector<UsingBinding> cloned;
    for (const auto& b : bindings_) {
        cloned.emplace_back(b.name, b.initializer ? b.initializer->clone() : nullptr);
    }
    return std::make_unique<UsingDeclaration>(std::move(cloned), is_await_, start_, end_);
}

void BlockStatement::check_use_strict_directive(Context& ctx) {
    // Scan the directive prologue -- all consecutive string-literal statements
    // at the top of the function body, not just the first one.
    for (const auto& stmt : statements_) {
        if (stmt->get_type() != ASTNode::Type::EXPRESSION_STATEMENT) break;
        auto* expr_stmt = static_cast<ExpressionStatement*>(stmt.get());
        auto* expr = expr_stmt->get_expression();
        if (!expr || expr->get_type() != ASTNode::Type::STRING_LITERAL) break;
        auto* sl = static_cast<StringLiteral*>(expr);
        // Per spec: directive is only valid when it has no escape sequences.
        if (sl->get_value() == "use strict" && !sl->has_escapes()) {
            ctx.set_strict_mode(true);
            return;
        }
    }
}

bool BlockStatement::needs_own_scope() const {
    if (needs_scope_ >= 0) return needs_scope_ != 0;
    bool needs = false;
    for (const auto& stmt : statements_) {
        switch (stmt->get_type()) {
            case ASTNode::Type::VARIABLE_DECLARATION: {
                auto* vd = static_cast<VariableDeclaration*>(stmt.get());
                if (vd->get_kind() != VariableDeclarator::Kind::VAR) needs = true;
                break;
            }
            // Function/class declarations bind lexically in the block; a labeled
            // statement can wrap a function declaration (Annex B) -- be conservative.
            case ASTNode::Type::FUNCTION_DECLARATION:
            case ASTNode::Type::CLASS_DECLARATION:
            case ASTNode::Type::USING_DECLARATION:
            case ASTNode::Type::LABELED_STATEMENT:
                needs = true;
                break;
            default:
                break;
        }
        if (needs) break;
    }
    needs_scope_ = needs ? 1 : 0;
    return needs;
}

Value BlockStatement::evaluate(Context& ctx) {
    Value last_value;

    // Check if this block has any 'using' declarations
    bool has_using = false;
    for (const auto& stmt : statements_) {
        if (stmt->get_type() == ASTNode::Type::USING_DECLARATION) { has_using = true; break; }
    }

    const bool own_scope = needs_own_scope();
    Environment* old_lexical_env = ctx.get_lexical_environment();
    Environment* block_env_ptr = old_lexical_env;
    if (own_scope) {
        auto block_env = std::make_unique<Environment>(Environment::Type::Declarative, old_lexical_env);
        block_env_ptr = block_env.release();
        ctx.set_lexical_environment(block_env_ptr);
    }

    // Pre-create TDZ bindings for let/const at the top level of this block (spec 14.2.2).
    // Without this, closures defined before a let/const declaration would bypass TDZ
    // because the binding wouldn't exist yet when they run.
    if (own_scope) {
    for (const auto& stmt : statements_) {
        if (stmt->get_type() == ASTNode::Type::VARIABLE_DECLARATION) {
            auto* vd = static_cast<VariableDeclaration*>(stmt.get());
            if (vd->get_kind() == VariableDeclarator::Kind::LET ||
                    vd->get_kind() == VariableDeclarator::Kind::CONST) {
                bool is_const_decl = vd->get_kind() == VariableDeclarator::Kind::CONST;
                for (const auto& decl : vd->get_declarations()) {
                    if (decl->get_id() && !decl->get_id()->get_name().empty()) {
                        const std::string& bname = decl->get_id()->get_name();
                        // Immutability must be set here: initialize_binding
                        // fills the value without touching the mutable flag.
                        block_env_ptr->create_uninitialized_binding(bname, !is_const_decl);
                        block_env_ptr->mark_lexical_declaration(bname);
                        if (is_const_decl) {
                            block_env_ptr->mark_const_binding(bname);
                        }
                    }
                }
            }
        }
    }
    }

    if (has_using) ctx.push_dispose_scope();

    bool exiting = false;
    bool block_had_non_empty = false;
    try {
        for (const auto& statement : statements_) {
            if (statement->get_type() == ASTNode::Type::FUNCTION_DECLARATION) {
                // Generator/async function declarations are block-scoped per spec, not hoisted.
                auto* fd = static_cast<FunctionDeclaration*>(statement.get());
                if (fd->is_generator() || fd->is_async()) continue;
                g_empty_completion = false;
                last_value = statement->evaluate(ctx);
                if (ctx.has_exception()) { exiting = true; break; }
            }
        }

        if (!exiting) {
            for (const auto& statement : statements_) {
                ASTNode::Type stype = statement->get_type();
                if (stype == ASTNode::Type::FUNCTION_DECLARATION) {
                    auto* fd = static_cast<FunctionDeclaration*>(statement.get());
                    if (!fd->is_generator() && !fd->is_async()) continue;
                }
                g_empty_completion = false;
                Value result = statement->evaluate(ctx);
                bool stmt_empty = g_empty_completion;
                if (!stmt_empty) {
                    last_value = result;
                    block_had_non_empty = true;
                }
                if (ctx.has_exception() || ctx.has_return_value() ||
                    ctx.has_break() || ctx.has_continue()) {
                    exiting = true;
                    break;
                }
            }
        }
    } catch (...) {
        if (has_using) ctx.run_dispose_resources();
        if (own_scope) {
            ctx.set_lexical_environment(old_lexical_env);
            if (!block_env_ptr->is_escaped()) Collector::release_env(block_env_ptr);
        }
        throw;
    }

    if (has_using) ctx.run_dispose_resources();
    if (own_scope) {
        ctx.set_lexical_environment(old_lexical_env);
        if (!block_env_ptr->is_escaped()) Collector::release_env(block_env_ptr);
    }

    if (exiting) {
        g_empty_completion = false;
        if (ctx.has_return_value()) return ctx.get_return_value();
        // break/continue: propagate last non-empty value (spec UpdateEmpty semantics)
        if (ctx.has_break() || ctx.has_continue()) return last_value;
        return Value();
    }
    // Signal empty completion if block had statements but none produced a real value
    g_empty_completion = (!statements_.empty() && !block_had_non_empty);
    return last_value;
}

std::string BlockStatement::to_string() const {
    std::ostringstream oss;
    oss << "{\n";
    for (const auto& statement : statements_) {
        oss << "  " << statement->to_string() << "\n";
    }
    oss << "}";
    return oss.str();
}

std::unique_ptr<ASTNode> BlockStatement::clone() const {
    std::vector<std::unique_ptr<ASTNode>> cloned_statements;
    for (const auto& statement : statements_) {
        cloned_statements.push_back(statement->clone());
    }
    return std::make_unique<BlockStatement>(std::move(cloned_statements), start_, end_);
}


Value IfStatement::evaluate(Context& ctx) {
    Value test_value = test_->evaluate(ctx);
    if (ctx.has_exception()) return Value();

    bool condition_result = test_value.to_boolean();
    if (condition_result) {
        Value result = consequent_->evaluate(ctx);
        if (ctx.has_return_value()) {
            return ctx.get_return_value();
        }
        if (ctx.has_break() || ctx.has_continue()) {
            return Value();
        }
        return result;
    } else if (alternate_) {
        Value result = alternate_->evaluate(ctx);
        if (ctx.has_return_value()) {
            return ctx.get_return_value();
        }
        if (ctx.has_break() || ctx.has_continue()) {
            return Value();
        }
        return result;
    }

    return Value();
}

std::string IfStatement::to_string() const {
    std::ostringstream oss;
    oss << "if (" << test_->to_string() << ") " << consequent_->to_string();
    if (alternate_) {
        oss << " else " << alternate_->to_string();
    }
    return oss.str();
}

std::unique_ptr<ASTNode> IfStatement::clone() const {
    std::unique_ptr<ASTNode> cloned_alternate = alternate_ ? alternate_->clone() : nullptr;
    return std::make_unique<IfStatement>(
        test_->clone(), consequent_->clone(), std::move(cloned_alternate), start_, end_
    );
}


Value ForStatement::evaluate(Context& ctx) {
    LoopDepthGuard guard;

    bool has_using_init = init_ && init_->get_type() == ASTNode::Type::USING_DECLARATION;

    if (!has_using_init && init_ && test_ && update_ && body_ && body_->get_type() == ASTNode::Type::EXPRESSION_STATEMENT) {
        ExpressionStatement* expr_stmt = static_cast<ExpressionStatement*>(body_.get());
        if (expr_stmt->get_expression() && expr_stmt->get_expression()->get_type() == ASTNode::Type::ASSIGNMENT_EXPRESSION) {
            AssignmentExpression* assign = static_cast<AssignmentExpression*>(expr_stmt->get_expression());
            if (assign->get_left() && assign->get_left()->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
                MemberExpression* member = static_cast<MemberExpression*>(assign->get_left());
                if (member->is_computed() && member->get_object()->get_type() == ASTNode::Type::IDENTIFIER) {
                    Identifier* arr_id = static_cast<Identifier*>(member->get_object());
                    Value arr_val = ctx.get_binding(arr_id->get_name());
                    if (arr_val.is_object() && arr_val.as_object()->is_array()) {
                        ctx.push_block_scope();
                        if (init_) init_->evaluate(ctx);

                        while (true) {
                            Collector::safepoint();
                            Value test_val = test_->evaluate(ctx);
                            if (!test_val.to_boolean()) break;

                            Value idx_val = member->get_property()->evaluate(ctx);
                            if (idx_val.is_number()) {
                                uint32_t idx = static_cast<uint32_t>(idx_val.as_number());
                                Value right_val = assign->get_right()->evaluate(ctx);
                                arr_val.as_object()->set_element(idx, right_val);
                            }

                            if (update_) update_->evaluate(ctx);
                        }

                        ctx.pop_block_scope();
                        decrement_loop_depth();
                        return Value();
                    }
                }
            }
        }
    }

    ctx.push_block_scope();
    if (has_using_init) ctx.push_dispose_scope();

    std::string this_loop_label = ctx.get_next_statement_label();
    ctx.set_next_statement_label("");

    std::string prev_loop_label = ctx.get_current_loop_label();
    ctx.set_current_loop_label(this_loop_label);

#define FOR_CLEANUP() \
    do { if (has_using_init) ctx.run_dispose_resources(); \
         ctx.set_current_loop_label(prev_loop_label); \
         ctx.pop_block_scope(); } while(0)

    Value result;
    Value V; // spec ForBodyEvaluation: V tracks last non-empty body completion
    try {
        bool has_per_iteration_scope = false;
        std::vector<std::string> iter_var_names;

        // Destructuring for-init (`for (let [x] = ...;;)`) is a bare AssignmentExpression, not a VariableDeclaration -- pre-create its lexical bindings before evaluating so they don't leak to the outer scope.
        if (init_ && (init_decl_kind_ == 1 || init_decl_kind_ == 2)) {
            ASTNode* destr_node = init_.get();
            if (destr_node->get_type() == ASTNode::Type::ASSIGNMENT_EXPRESSION) {
                destr_node = static_cast<AssignmentExpression*>(destr_node)->get_left();
            }
            if (auto* destr = dynamic_cast<DestructuringAssignment*>(destr_node)) {
                for (const auto& tgt : destr->get_targets()) {
                    if (!tgt || tgt->get_name().empty()) continue;
                    const std::string& tname = tgt->get_name();
                    bool is_key = false;
                    for (const auto& m : destr->get_property_mappings()) {
                        if (m.property_name == tname) { is_key = true; break; }
                    }
                    if (is_key) continue;
                    ctx.create_lexical_binding(tname, Value(), true);
                    if (init_decl_kind_ == 1) iter_var_names.push_back(tname);
                }
                has_per_iteration_scope = !iter_var_names.empty();
            }
        }

        if (init_) {
            init_->evaluate(ctx);
            if (ctx.has_exception()) {
                FOR_CLEANUP();
                return Value();
            }
        }

    if (init_ && init_->get_type() == Type::VARIABLE_DECLARATION) {
        auto* var_decl = static_cast<VariableDeclaration*>(init_.get());
        // Spec 14.7.4.2: only let (not const) gets per-iteration environments.
        // for (const i = 0; ...) has no per-iter scope -- update hits the original const
        // binding directly, causing TypeError as expected.
        if (var_decl->get_kind() == VariableDeclarator::Kind::LET) {
            has_per_iteration_scope = true;
            for (const auto& decl : var_decl->get_declarations()) {
                iter_var_names.push_back(decl->get_id()->get_name());
            }
        }
    }

    // Spec 14.7.4.4: CreatePerIterationEnvironment before the first test (initial).
    auto create_per_iter_env = [&]() {
        std::vector<Value> iter_values;
        for (const auto& vname : iter_var_names) {
            iter_values.push_back(ctx.get_binding(vname));
        }
        ctx.push_block_scope();
        for (size_t vi = 0; vi < iter_var_names.size(); vi++) {
            ctx.create_lexical_binding(iter_var_names[vi], iter_values[vi], true);
        }
    };

    if (has_per_iteration_scope) create_per_iter_env();

    while (true) {
        Collector::safepoint();

        if (test_) {
            Value test_value = test_->evaluate(ctx);
            if (ctx.has_exception()) {
                if (has_per_iteration_scope) ctx.pop_block_scope();
                FOR_CLEANUP();
                return Value();
            }
            if (!test_value.to_boolean()) {
                break;
            }
        }

        if (body_) {
            Value body_result = body_->evaluate(ctx);
            if (!g_empty_completion) V = body_result;

            if (ctx.has_exception()) {
                if (has_per_iteration_scope) ctx.pop_block_scope();
                FOR_CLEANUP();
                return Value();
            }

            if (ctx.has_break()) {
                if (ctx.get_break_label().empty()) {
                    ctx.clear_break_continue();
                    break;
                }
                break;
            }
            if (ctx.has_return_value()) {
                if (has_per_iteration_scope) ctx.pop_block_scope();
                FOR_CLEANUP();
                return ctx.get_return_value();
            }

            bool is_continue = ctx.has_continue();
            bool continue_matches = is_continue && (ctx.get_continue_label().empty() ||
                                    ctx.get_continue_label() == ctx.get_current_loop_label());
            if (is_continue && !continue_matches) {
                break;
            }
            if (is_continue && continue_matches) {
                ctx.clear_break_continue();
            }
        }

        // Spec 14.7.4.4 step e: CreatePerIterationEnvironment BEFORE the increment, so the
        // increment mutates the *new* environment -- not the one this round's body (and any
        // closures it created) captured by reference. Read the pre-increment values, fork.
        if (has_per_iteration_scope) {
            std::vector<Value> iter_values;
            for (const auto& vname : iter_var_names) {
                iter_values.push_back(ctx.get_binding(vname));
            }
            ctx.pop_block_scope();
            ctx.push_block_scope();
            for (size_t vi = 0; vi < iter_var_names.size(); vi++) {
                ctx.create_lexical_binding(iter_var_names[vi], iter_values[vi], true);
            }
        }

        // Spec 14.7.4.4 step f: increment runs in the freshly forked per-iteration env.
        if (update_) {
            update_->evaluate(ctx);
            if (ctx.has_exception()) {
                if (has_per_iteration_scope) ctx.pop_block_scope();
                FOR_CLEANUP();
                return Value();
            }
        }
    }
    if (has_per_iteration_scope) ctx.pop_block_scope();

        result = V;
    } catch (...) {
        if (has_using_init) ctx.run_dispose_resources();
        ctx.set_current_loop_label(prev_loop_label);
        ctx.pop_block_scope();
        decrement_loop_depth();
        throw;
    }

#undef FOR_CLEANUP

    if (has_using_init) ctx.run_dispose_resources();
    ctx.set_current_loop_label(prev_loop_label);
    ctx.pop_block_scope();
    decrement_loop_depth();
    g_empty_completion = false;
    return result;
}

std::string ForStatement::to_string() const {
    std::ostringstream oss;
    oss << "for (";
    if (init_) oss << init_->to_string();
    oss << "; ";
    if (test_) oss << test_->to_string();
    oss << "; ";
    if (update_) oss << update_->to_string();
    oss << ") " << body_->to_string();
    return oss.str();
}

std::unique_ptr<ASTNode> ForStatement::clone() const {
    std::unique_ptr<ASTNode> cloned_init = init_ ? init_->clone() : nullptr;
    std::unique_ptr<ASTNode> cloned_test = test_ ? test_->clone() : nullptr;
    std::unique_ptr<ASTNode> cloned_update = update_ ? update_->clone() : nullptr;
    return std::make_unique<ForStatement>(
        std::move(cloned_init), std::move(cloned_test),
        std::move(cloned_update), body_->clone(), start_, end_,
        init_decl_kind_
    );
}


// ES5 12.6.4: enumerate own enumerable properties then inherited ones.
// Non-enumerable own at a closer level blocks inherited enumerable at outer
// level. Shared with evaluate() below (which calls this instead of the old
// inline version) and with the VM's Op::CreateForInKeys.
bool ForInStatement::collect_keys(Context& ctx, Object* obj, std::vector<std::string>& out_keys) {
    std::unordered_set<std::string> blocked; // seen at any closer level (blocks inherited)
    Object* cur = obj;
    while (cur) {
        std::vector<std::string> all_own;
        if (cur->get_type() == Object::ObjectType::Proxy) {
            try {
                all_own = static_cast<Proxy*>(cur)->own_keys_trap();
            } catch (const std::runtime_error&) {
                if (!ctx.has_exception()) ctx.throw_type_error("'ownKeys' proxy invariant violated");
            }
            if (ctx.has_exception()) return false;
        } else {
            all_own = cur->get_own_property_keys();
        }
        // Yield enumerable own keys not already blocked by a closer object
        for (const auto& k : all_own) {
            if (k.size() >= 2 && k[0] == '_' && k[1] == '_') continue;
            if (k.find("@@sym:") == 0 || k.find("Symbol.") == 0 || k.find("Symbol(") == 0) continue;
            if (blocked.count(k)) continue; // shadowed by closer level
            // Check if this own key is enumerable
            PropertyDescriptor d = cur->get_property_descriptor(k);
            if (d.is_enumerable()) out_keys.push_back(k);
            // Add to blocked regardless of enumerability (non-enum own blocks inherited enum)
            blocked.insert(k);
        }
        cur = cur->get_prototype();
    }
    return true;
}

Value ForInStatement::evaluate(Context& ctx) {
    std::string this_loop_label = ctx.get_next_statement_label();
    ctx.set_next_statement_label("");
    std::string prev_loop_label = ctx.get_current_loop_label();
    ctx.set_current_loop_label(this_loop_label);

    // ES6 13.7.5.6: TDZ bindings for let/const before evaluating the object
    Environment* pre_obj_env = nullptr;
    if (left_->get_type() == Type::VARIABLE_DECLARATION) {
        auto* vd = static_cast<VariableDeclaration*>(left_.get());
        auto kind = (vd->declaration_count() > 0) ? vd->get_declarations()[0]->get_kind()
                                                   : VariableDeclarator::Kind::VAR;
        if (kind == VariableDeclarator::Kind::LET || kind == VariableDeclarator::Kind::CONST) {
            pre_obj_env = ctx.get_lexical_environment();
            auto tdz_env = std::make_unique<Environment>(Environment::Type::Declarative, pre_obj_env);
            Environment* tdz_ptr = tdz_env.release();
            ctx.set_lexical_environment(tdz_ptr);
            for (size_t di = 0; di < vd->declaration_count(); di++) {
                const auto& decl = vd->get_declarations()[di];
                if (decl->get_id()) tdz_ptr->create_uninitialized_binding(decl->get_id()->get_name());
            }
        } else if (kind == VariableDeclarator::Kind::VAR && vd->declaration_count() > 0) {
            auto* decl = vd->get_declarations()[0].get();
            std::string vname = decl->get_id()->get_name();
            Value init_val;
            if (decl->get_init()) {
                init_val = decl->get_init()->evaluate(ctx);
                if (ctx.has_exception()) {
                    ctx.set_current_loop_label(prev_loop_label);
                    return Value();
                }
            }
            if (!ctx.has_binding(vname)) ctx.create_binding(vname, init_val, true);
            else if (decl->get_init()) ctx.set_binding(vname, init_val);
        }
    } else if (left_->get_type() == Type::DESTRUCTURING_ASSIGNMENT &&
               (left_decl_kind_ == 1 || left_decl_kind_ == 2)) {
        // for (let/const [x, y] in obj) -- TDZ for bound names during object evaluation
        auto* da = static_cast<DestructuringAssignment*>(left_.get());
        pre_obj_env = ctx.get_lexical_environment();
        auto tdz_env = std::make_unique<Environment>(Environment::Type::Declarative, pre_obj_env);
        Environment* tdz_ptr = tdz_env.release();
        ctx.set_lexical_environment(tdz_ptr);
        for (const auto& target : da->get_targets()) {
            if (target && !target->get_name().empty())
                tdz_ptr->create_uninitialized_binding(target->get_name());
        }
        for (const auto& pm : da->get_property_mappings()) {
            if (!pm.variable_name.empty())
                tdz_ptr->create_uninitialized_binding(pm.variable_name);
        }
    }

    Value object = right_->evaluate(ctx);
    if (pre_obj_env) ctx.set_lexical_environment(pre_obj_env);
    if (ctx.has_exception()) {
        ctx.set_current_loop_label(prev_loop_label);
        return Value();
    }

    // Spec 13.7.5.6: null/undefined produce zero iterations (no error)
    if (object.is_null() || object.is_undefined()) {
        ctx.set_current_loop_label(prev_loop_label);
        return Value();
    }

    if (object.is_object_like()) {
        Object* obj = object.is_object() ? object.as_object() : object.as_function();

        std::string var_name;
        bool is_destructuring = false;

        bool is_member_lhs = false;

        if (left_->get_type() == Type::VARIABLE_DECLARATION) {
            VariableDeclaration* var_decl = static_cast<VariableDeclaration*>(left_.get());
            if (var_decl->declaration_count() > 0) {
                VariableDeclarator* declarator = var_decl->get_declarations()[0].get();
                var_name = declarator->get_id()->get_name();
            }
        } else if (left_->get_type() == Type::IDENTIFIER) {
            Identifier* id = static_cast<Identifier*>(left_.get());
            var_name = id->get_name();
        } else if (left_->get_type() == Type::DESTRUCTURING_ASSIGNMENT) {
            is_destructuring = true;
        } else if (left_->get_type() == Type::MEMBER_EXPRESSION ||
                   left_->get_type() == Type::ARRAY_LITERAL ||
                   left_->get_type() == Type::OBJECT_LITERAL) {
            is_member_lhs = true;
        }

        if (var_name.empty() && !is_destructuring && !is_member_lhs) {
            ctx.set_current_loop_label(prev_loop_label);
            ctx.throw_exception(Value(std::string("For...in: Invalid loop variable")));
            return Value();
        }

        std::vector<std::string> keys;
        if (!collect_keys(ctx, obj, keys)) {
            ctx.set_current_loop_label(prev_loop_label);
            return Value();
        }

        bool forin_per_iter = false;
        if (left_->get_type() == Type::VARIABLE_DECLARATION) {
            auto* vd = static_cast<VariableDeclaration*>(left_.get());
            if (vd->get_kind() == VariableDeclarator::Kind::LET ||
                vd->get_kind() == VariableDeclarator::Kind::CONST) {
                forin_per_iter = true;
            }
        }
        // for (let/const [x] in obj) also gets per-iteration scope
        bool forin_destr_per_iter = (is_destructuring && (left_decl_kind_ == 1 || left_decl_kind_ == 2));

        Value V; // completion value (spec ForIn/OfBodyEvaluation V)

        for (const auto& key : keys) {
            Collector::safepoint();

            // Skip properties deleted during enumeration (spec allows this).
            {
                bool still_exists = false;
                Object* cur_check = obj;
                while (cur_check) {
                    if (cur_check->has_own_property(key)) { still_exists = true; break; }
                    cur_check = cur_check->get_prototype();
                }
                if (!still_exists) continue;
            }

            if (forin_destr_per_iter) {
                auto* destr = static_cast<DestructuringAssignment*>(left_.get());
                ctx.push_block_scope();
                for (const auto& target : destr->get_targets()) {
                    if (!target || target->get_name().empty()) continue;
                    const std::string& tname = target->get_name();
                    bool is_key = false;
                    for (const auto& pm : destr->get_property_mappings()) {
                        if (pm.property_name == tname) { is_key = true; break; }
                    }
                    if (!is_key)
                        ctx.create_lexical_binding(tname, Value(), true);
                }
                for (const auto& pm : destr->get_property_mappings()) {
                    if (!pm.variable_name.empty())
                        ctx.create_lexical_binding(pm.variable_name, Value(), true);
                }
                destr->evaluate_with_value(ctx, Value(key));
                if (!ctx.has_exception()) {
                    Value body_result = body_->evaluate(ctx);
                    if (!body_result.is_undefined()) V = body_result;
                }
                ctx.pop_block_scope();
                if (ctx.has_exception()) { ctx.set_current_loop_label(prev_loop_label); return Value(); }
                if (ctx.has_break()) {
                    if (ctx.get_break_label().empty()) ctx.clear_break_continue();
                    break;
                }
                if (ctx.has_continue()) {
                    if (ctx.get_continue_label().empty() || ctx.get_continue_label() == this_loop_label)
                        ctx.clear_break_continue();
                    else break;
                    continue;
                }
                if (ctx.has_return_value()) { ctx.set_current_loop_label(prev_loop_label); return ctx.get_return_value(); }
                continue;
            } else if (is_destructuring) {
                auto* destr = static_cast<DestructuringAssignment*>(left_.get());
                destr->evaluate_with_value(ctx, Value(key));
                if (ctx.has_exception()) { ctx.set_current_loop_label(prev_loop_label); return Value(); }
            } else if (is_member_lhs) {
                AssignmentExpression::assign_to_target(ctx, left_.get(), Value(key));
                if (ctx.has_exception()) { ctx.set_current_loop_label(prev_loop_label); return Value(); }
            } else if (forin_per_iter) {
                ctx.push_block_scope();
                auto* vd2 = static_cast<VariableDeclaration*>(left_.get());
                bool is_mutable_iter = (vd2->get_kind() != VariableDeclarator::Kind::CONST);
                ctx.create_lexical_binding(var_name, Value(key), is_mutable_iter);
            } else {
                if (ctx.has_binding(var_name)) {
                    bool ok = ctx.set_binding(var_name, Value(key));
                    if (!ok && (ctx.is_strict_mode() || ctx.is_strict_const(var_name))) {
                        ctx.set_current_loop_label(prev_loop_label);
                        ctx.throw_type_error("Assignment to constant variable '" + var_name + "'");
                        return Value();
                    }
                } else {
                    ctx.create_binding(var_name, Value(key), true);
                }
            }

            Value result = body_->evaluate(ctx);

            if (forin_per_iter) {
                ctx.pop_block_scope();
            }

            // UpdateEmpty: BlockStatement returns last_value on break/continue, so
            // result already carries the right completion value
            if (!result.is_undefined()) V = result;

            if (ctx.has_exception()) {
                ctx.set_current_loop_label(prev_loop_label);
                return Value();
            }

            if (ctx.has_break()) {
                if (ctx.get_break_label().empty()) {
                    ctx.clear_break_continue();
                }
                break;
            }
            if (ctx.has_continue()) {
                if (ctx.get_continue_label().empty() ||
                        ctx.get_continue_label() == this_loop_label) {
                    ctx.clear_break_continue();
                    continue;
                }
                break;
            }

            if (ctx.has_return_value()) {
                ctx.set_current_loop_label(prev_loop_label);
                return ctx.get_return_value();
            }
        }

        ctx.set_current_loop_label(prev_loop_label);
        return V;
    } else {
        // Spec 13.7.5.11: primitives are ToObject'd -- number/boolean have no enumerable props.
        // Strings should iterate character indices, but that's handled via is_object_like for boxed strings.
        // For unboxed primitives (number, boolean, symbol), zero iterations, no error.
        ctx.set_current_loop_label(prev_loop_label);
        return Value();
    }
}

std::string ForInStatement::to_string() const {
    return "for (" + left_->to_string() + " in " + right_->to_string() + ") " + body_->to_string();
}

std::unique_ptr<ASTNode> ForInStatement::clone() const {
    return std::make_unique<ForInStatement>(left_->clone(), right_->clone(), body_->clone(), start_, end_, left_decl_kind_);
}

// Mirrors this function's own sync (non-await), non-string-fallback GetIterator
// steps below (see the `if (iterator_symbol && obj && ...)` block) -- the VM's
// Op::GetIterator calls this directly instead of duplicating the logic.
bool ForOfStatement::get_iterator(Context& ctx, const Value& iterable, Value& out_iterator, Value& out_next_fn) {
    if (!iterable.is_object() && !iterable.is_string() && !iterable.is_function()) {
        ctx.throw_type_error(iterable.to_string() + " is not iterable");
        return false;
    }
    Object* obj = nullptr;
    std::unique_ptr<Object> boxed_string;
    if (iterable.is_string()) {
        boxed_string = std::make_unique<Object>();
        boxed_string->set_property("length", Value(static_cast<double>(utf16_length(iterable.to_string()))));
        Symbol* iterator_symbol = Symbol::get_well_known(Symbol::ITERATOR);
        if (iterator_symbol) {
            std::string str_value = iterable.to_string();
            auto string_iterator_fn = ObjectFactory::create_native_function("@@iterator",
                [str_value](Context&, const std::vector<Value>&) -> Value {
                    auto iterator = std::make_unique<StringIterator>(str_value);
                    return Value(iterator.release());
                });
            boxed_string->set_property(iterator_symbol->to_property_key(), Value(string_iterator_fn.release()));
        }
        obj = boxed_string.get();
    } else {
        obj = iterable.is_function() ? static_cast<Object*>(iterable.as_function()) : iterable.as_object();
    }

    Symbol* iterator_symbol = Symbol::get_well_known(Symbol::ITERATOR);
    if (!iterator_symbol || !obj->has_property(iterator_symbol->to_property_key())) {
        ctx.throw_type_error("value is not iterable");
        return false;
    }
    Value iterator_method = obj->get_property(iterator_symbol->to_property_key());
    if (ctx.has_exception()) return false;
    if (!iterator_method.is_function()) {
        ctx.throw_type_error("value is not iterable");
        return false;
    }
    // obj (and boxed_string, if this is the string case) may be destroyed
    // once this function returns -- iterator_method is called with the
    // ORIGINAL iterable as receiver (matching the tree-walker: iter_fn->
    // call(ctx, {}, iterable), never `obj`), so nothing below reads obj again.
    Value iterator_val = iterator_method.as_function()->call(ctx, {}, iterable);
    if (ctx.has_exception()) return false;
    if (!iterator_val.is_object()) {
        ctx.throw_type_error("Result of the Symbol.iterator method is not an object");
        return false;
    }
    Value next_method = iterator_val.as_object()->get_property("next");
    if (ctx.has_exception()) return false;
    if (!next_method.is_function()) {
        ctx.throw_type_error("next method is not a function");
        return false;
    }
    out_iterator = iterator_val;
    out_next_fn = next_method;
    return true;
}

// Mirrors the tree-walker's per-iteration `next_fn->call` + done/value
// extraction (including the Object::current_context_ getter-exception
// rescue for a `done`/`value` accessor that throws through a different
// Context, e.g. a Proxy trap) below in the sync for-of loop.
bool ForOfStatement::iterator_step(Context& ctx, const Value& iterator, const Value& next_fn,
                                    bool& out_done, Value& out_value) {
    Value result = next_fn.as_function()->call(ctx, {}, iterator);
    // Per spec: if next() throws abruptly, do NOT close the iterator.
    if (ctx.has_exception()) return false;
    if (!result.is_object()) {
        ctx.throw_type_error("Iterator result is not an object");
        return false;
    }
    Object* result_obj = result.as_object();
    Value done = result_obj->get_property("done");
    if (!ctx.has_exception() && Object::current_context_ && Object::current_context_ != &ctx
            && Object::current_context_->has_exception()) {
        ctx.throw_exception(Object::current_context_->get_exception(), true);
        Object::current_context_->clear_exception();
    }
    if (ctx.has_exception()) return false;
    if (done.to_boolean()) {
        out_done = true;
        return true;
    }
    Value value = result_obj->get_property("value");
    if (!ctx.has_exception() && Object::current_context_ && Object::current_context_ != &ctx
            && Object::current_context_->has_exception()) {
        ctx.throw_exception(Object::current_context_->get_exception(), true);
        Object::current_context_->clear_exception();
    }
    if (ctx.has_exception()) return false;
    out_done = false;
    out_value = value;
    return true;
}

// Mirrors the tree-walker's close_iterator lambda below in the sync for-of
// loop, except the tree-walker calls it while ctx's OWN exception is still
// live (had_exception = ctx.has_exception()); the VM's CHECK_EXC already
// cleared ctx's exception before jumping to any handler, so it passes the
// pending value explicitly instead (is_pending/pending_exception) rather
// than relying on ctx still carrying it.
void ForOfStatement::iterator_close(Context& ctx, const Value& iterator, bool validate_result,
                                     bool is_pending, const Value& pending_exception) {
    if (!iterator.is_object()) {
        if (is_pending) ctx.throw_exception(pending_exception, true);
        return;
    }
    Value return_method = iterator.as_object()->get_property("return");
    bool inner_threw = ctx.has_exception();
    if (!inner_threw) {
        if (!return_method.is_undefined() && !return_method.is_null() && !return_method.is_function()) {
            ctx.throw_type_error("Iterator return method is not callable");
            inner_threw = true;
        } else if (return_method.is_function()) {
            Value result = return_method.as_function()->call(ctx, {}, iterator);
            inner_threw = ctx.has_exception();
            if (!inner_threw && validate_result && !result.is_object()) {
                ctx.throw_type_error("Iterator return() must return an Object");
                inner_threw = true;
            }
        }
    }
    (void)inner_threw;  // only relevant to decide whether return()'s own throw survives below
    if (is_pending) {
        if (ctx.has_exception()) ctx.clear_exception();
        ctx.throw_exception(pending_exception, true);
    }
    // else: any inner_threw exception (from a failed return()) is left as
    // the live ctx exception -- exactly the desired "break, but return()
    // itself failed" completion.
}

Value ForOfStatement::evaluate(Context& ctx) {
    // A continue targeting an outer construct must keep propagating past
    // this loop -- only one that's unlabeled or matches this loop's own
    // label (via a wrapping LabeledStatement) is ours to consume.
    std::string this_loop_label = ctx.get_next_statement_label();
    ctx.set_next_statement_label("");

    // ES6 13.7.5.6: ForDeclaration bound names are in TDZ when the iterable is evaluated.
    // Create a block scope with TDZ bindings before evaluating the iterable so that
    // `for (const x of [x])` sees x as TDZ, not any outer x.
    Environment* pre_iter_env = nullptr;
    if (left_->get_type() == ASTNode::Type::VARIABLE_DECLARATION) {
        auto* vd = static_cast<VariableDeclaration*>(left_.get());
        if (vd->declaration_count() > 0 &&
                (vd->get_declarations()[0]->get_kind() == VariableDeclarator::Kind::LET ||
                 vd->get_declarations()[0]->get_kind() == VariableDeclarator::Kind::CONST)) {
            pre_iter_env = ctx.get_lexical_environment();
            auto tdz_env = std::make_unique<Environment>(Environment::Type::Declarative, pre_iter_env);
            Environment* tdz_ptr = tdz_env.release();
            ctx.set_lexical_environment(tdz_ptr);
            for (size_t di = 0; di < vd->declaration_count(); di++) {
                const auto& decl = vd->get_declarations()[di];
                if (decl->get_id()) {
                    std::string bname = decl->get_id()->get_name();
                    if (!bname.empty())
                        tdz_ptr->create_uninitialized_binding(bname);
                }
            }
        }
    } else if (left_->get_type() == ASTNode::Type::DESTRUCTURING_ASSIGNMENT &&
               (left_decl_kind_ == 1 || left_decl_kind_ == 2)) {
        // for (let/const [x, y] of ...) -- TDZ for bound names during iterable evaluation
        auto* da = static_cast<DestructuringAssignment*>(left_.get());
        pre_iter_env = ctx.get_lexical_environment();
        auto tdz_env = std::make_unique<Environment>(Environment::Type::Declarative, pre_iter_env);
        Environment* tdz_ptr = tdz_env.release();
        ctx.set_lexical_environment(tdz_ptr);
        for (const auto& target : da->get_targets()) {
            if (target && !target->get_name().empty())
                tdz_ptr->create_uninitialized_binding(target->get_name());
        }
        for (const auto& pm : da->get_property_mappings()) {
            if (!pm.variable_name.empty())
                tdz_ptr->create_uninitialized_binding(pm.variable_name);
        }
    } else if (left_->get_type() == ASTNode::Type::USING_DECLARATION) {
        auto* ud = static_cast<UsingDeclaration*>(left_.get());
        pre_iter_env = ctx.get_lexical_environment();
        auto tdz_env = std::make_unique<Environment>(Environment::Type::Declarative, pre_iter_env);
        Environment* tdz_ptr = tdz_env.release();
        ctx.set_lexical_environment(tdz_ptr);
        for (const auto& b : ud->get_bindings())
            if (!b.name.empty()) tdz_ptr->create_uninitialized_binding(b.name);
    }

    Value iterable = right_->evaluate(ctx);
    if (ctx.has_exception()) {
        if (pre_iter_env) ctx.set_lexical_environment(pre_iter_env);
        return Value();
    }
    // Restore outer env -- the loop body will re-create the binding per iteration
    if (pre_iter_env) ctx.set_lexical_environment(pre_iter_env);

    if (is_await_) {
        // AsyncExecutor::current_ is thread-local and survives AsyncGenerator::enter_fiber's ucontext swap, so it can still point at an unrelated outer exec -- check the async-generator fiber first to avoid swapcontext-ing into the wrong coroutine.
        AsyncGenerator* async_gen = AsyncGenerator::get_current();
        bool in_async_gen_fiber = async_gen && !async_gen->fiber_stack_.empty();
        AsyncExecutor* exec = in_async_gen_fiber ? nullptr : AsyncExecutor::get_current();
        Context* gctx = in_async_gen_fiber
            ? (async_gen->get_outer_context() ? async_gen->get_outer_context() : async_gen->get_generator_context())
            : (exec && exec->engine_) ? exec->engine_->get_current_context() : &ctx;

        if (!iterable.is_object()) {
            ctx.throw_type_error("for-await-of: value is not iterable");
            return Value();
        }

        Object* iterable_obj = iterable.as_object();

        Value iter_method;
        bool used_async_iterator = false;
        Symbol* async_iter_sym = Symbol::get_well_known(Symbol::ASYNC_ITERATOR);
        if (async_iter_sym) {
            iter_method = iterable_obj->get_property(async_iter_sym->to_property_key());
            if (iter_method.is_function()) used_async_iterator = true;
        }
        if (!iter_method.is_function()) {
            Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
            if (iter_sym) {
                iter_method = iterable_obj->get_property(iter_sym->to_property_key());
            }
        }
        if (!iter_method.is_function()) {
            ctx.throw_exception(Value(std::string("for-await-of: object is not iterable")));
            return Value();
        }

        Value iterator_val = iter_method.as_function()->call(ctx, {}, iterable);
        if (ctx.has_exception()) return Value();
        if (!iterator_val.is_object()) {
            ctx.throw_exception(Value(std::string("for-await-of: iterator must be an object")));
            return Value();
        }
        Object* iterator_obj = iterator_val.as_object();

        std::string var_name;
        VariableDeclarator::Kind var_kind = VariableDeclarator::Kind::VAR;
        if (left_->get_type() == Type::VARIABLE_DECLARATION) {
            VariableDeclaration* var_decl = static_cast<VariableDeclaration*>(left_.get());
            if (var_decl->declaration_count() > 0) {
                VariableDeclarator* declarator = var_decl->get_declarations()[0].get();
                var_name = declarator->get_id()->get_name();
                var_kind = declarator->get_kind();
            }
        } else if (left_->get_type() == Type::IDENTIFIER) {
            Identifier* id = static_cast<Identifier*>(left_.get());
            var_name = id->get_name();
        } else if (left_->get_type() == Type::DESTRUCTURING_ASSIGNMENT) {
            var_name = "__destr__";
        } else if (left_->get_type() == Type::ARRAY_LITERAL ||
                   left_->get_type() == Type::OBJECT_LITERAL) {
            var_name = "__destr__";
        }
        if (var_name.empty()) {
            ctx.throw_exception(Value(std::string("for-await-of: invalid loop variable")));
            return Value();
        }

        // AsyncIteratorClose (spec 27.7.4): called when the loop exits via `break`
        // before the iterator naturally finished. GetMethod(iterator, "return") is
        // accessed (and any getter exception/non-callable value surfaces) even
        // though `return` is frequently absent and there is nothing further to call.
        auto close_async_iterator_on_break = [&ctx](Object* iter_obj) {
            Value return_method = iter_obj->get_property("return");
            if (ctx.has_exception()) return;
            if (return_method.is_null() || return_method.is_undefined()) return;
            if (!return_method.is_function()) {
                ctx.throw_type_error("iterator return method is not callable");
                return;
            }
            return_method.as_function()->call(ctx, {}, Value(iter_obj));
        };

        for (uint32_t i = 0;; i++) {
            Collector::safepoint();
            Value awaited;
            if (in_async_gen_fiber) {
                // Fiber-based (async generator's own fiber): call next(), await the result
                Value next_method_val = iterator_obj->get_property("next");
                if (!next_method_val.is_function()) {
                    ctx.throw_exception(Value(std::string("for-await-of: iterator has no next method")));
                    return Value();
                }
                Value next_result = next_method_val.as_function()->call(ctx, {}, iterator_val);
                if (ctx.has_exception()) return Value();

                bool is_pending = false;
                bool settled_throw = false;
                Value settled_val;

                if (AsyncUtils::is_promise(next_result)) {
                    Promise* prom = static_cast<Promise*>(next_result.as_object());
                    // PromiseResolve on an already-Promise value still Gets "constructor"
                    // (step 1a) -- side effect only, no subclass rewrap.
                    prom->get_property("constructor");
                    if (!ctx.has_exception() && Object::current_context_ && Object::current_context_ != &ctx
                            && Object::current_context_->has_exception()) {
                        ctx.throw_exception(Object::current_context_->get_exception(), true);
                        Object::current_context_->clear_exception();
                    }
                    if (ctx.has_exception()) return Value();
                    if (prom->get_state() == PromiseState::FULFILLED) {
                        settled_val = prom->get_value();
                    } else if (prom->get_state() == PromiseState::REJECTED) {
                        settled_val = prom->get_value();
                        settled_throw = true;
                    } else {
                        is_pending = true;
                        auto self = async_gen;
                        auto on_f = ObjectFactory::create_native_function("",
                            [self, gctx](Context&, const std::vector<Value>& args) -> Value {
                                Value val = args.empty() ? Value() : args[0];
                                self->resume_from_await(val, false);
                                return Value();
                            });
                        auto on_r = ObjectFactory::create_native_function("",
                            [self, gctx](Context&, const std::vector<Value>& args) -> Value {
                                Value reason = args.empty() ? Value() : args[0];
                                self->resume_from_await(reason, true);
                                return Value();
                            });
                        std::string key = std::to_string(i) + "_ag_" + std::to_string(reinterpret_cast<uintptr_t>(async_gen));
                        Function* ff_tmp_ = on_f.get(); Function* fr_tmp_ = on_r.get();
                        prom->set_property("__af_" + key, Value(on_f.release()));
                        prom->set_property("__ar_" + key, Value(on_r.release()));
                        prom->then(ff_tmp_, fr_tmp_);
                    }
                } else {
                    settled_val = next_result;
                }

                if (!is_pending) {
                    auto self = async_gen;
                    Value val = settled_val;
                    bool thr = settled_throw;
                    if (gctx) gctx->queue_microtask([self, val, thr]() mutable { self->resume_from_await(val, thr); }, {Value(self), val});
                }

                async_gen->await_result_ = next_result;  // pin promise as GC root during suspension
                async_gen->suspend_reason_ = AsyncGenerator::SuspendReason::Await;
                swapcontext(&async_gen->fiber_->fiber_ctx, &async_gen->fiber_->caller_ctx);
                Object::current_context_ = &ctx;

                if (async_gen->await_is_throw_) {
                    ctx.throw_exception(async_gen->await_result_, true);
                    async_gen->await_is_throw_ = false;
                    async_gen->await_result_ = Value();
                    return Value();
                }
                awaited = async_gen->await_result_;
                async_gen->await_result_ = Value();
            } else if (exec && !exec->fiber_stack_.empty()) {
                // Fiber-based: call next(), await the result
                Value next_method_val = iterator_obj->get_property("next");
                if (!next_method_val.is_function()) {
                    ctx.throw_exception(Value(std::string("for-await-of: iterator has no next method")));
                    return Value();
                }

                bool is_pending = false;
                bool settled_throw = false;
                Value settled_val;
                Value next_result;

                if (!used_async_iterator) {
                    // AsyncFromSyncIteratorContinuation: the sync iterator's `.value` is
                    // PromiseResolve'd and awaited first, then the resulting {value,done} is
                    // delivered through a SEPARATE promise that this loop's own Await(nextResult)
                    // awaits again -- two independent PromiseResolve calls, each its own tick.
                    // A getter invoked here may throw via Object::current_context_ instead of
                    // ctx -- rescue it.
                    auto rescue_getter_exception = [&ctx]() {
                        if (!ctx.has_exception() && Object::current_context_ && Object::current_context_ != &ctx
                                && Object::current_context_->has_exception()) {
                            ctx.throw_exception(Object::current_context_->get_exception(), true);
                            Object::current_context_->clear_exception();
                        }
                    };

                    Value sync_result = next_method_val.as_function()->call(ctx, {}, iterator_val);
                    rescue_getter_exception();
                    if (ctx.has_exception()) return Value();
                    if (!sync_result.is_object()) {
                        ctx.throw_type_error("for-await-of: iterator result must be an object");
                        return Value();
                    }
                    Value sync_done_val = sync_result.as_object()->get_property("done");
                    rescue_getter_exception();
                    if (ctx.has_exception()) return Value();
                    Value sync_value_val = sync_result.as_object()->get_property("value");
                    rescue_getter_exception();
                    if (ctx.has_exception()) return Value();
                    bool sync_done = sync_done_val.to_boolean();

                    auto nr_obj = ObjectFactory::create_promise(gctx);
                    Promise* next_result_promise = static_cast<Promise*>(nr_obj.release());

                    Promise* value_wrapper = nullptr;
                    if (AsyncUtils::is_promise(sync_value_val)) {
                        value_wrapper = static_cast<Promise*>(sync_value_val.as_object());
                        value_wrapper->get_property("constructor");
                        rescue_getter_exception();
                        if (ctx.has_exception()) {
                            Value err = ctx.get_exception();
                            ctx.clear_exception();
                            // Continuation step 6: an abrupt PromiseResolve with done false
                            // closes the sync iterator; close failures are swallowed.
                            if (!sync_done) {
                                Value close_fn = iterator_obj->get_property("return");
                                rescue_getter_exception();
                                if (!ctx.has_exception() && close_fn.is_function()) {
                                    close_fn.as_function()->call(ctx, {}, iterator_val);
                                    rescue_getter_exception();
                                }
                                ctx.clear_exception();
                            }
                            next_result_promise->reject(err);
                            value_wrapper = nullptr;
                        }
                    } else {
                        auto vw_obj = ObjectFactory::create_promise(gctx);
                        Promise* vw_raw = static_cast<Promise*>(vw_obj.get());
                        vw_raw->fulfill(sync_value_val);
                        value_wrapper = static_cast<Promise*>(vw_obj.release());
                    }

                    if (value_wrapper) {
                        auto unwrap_f = ObjectFactory::create_native_function("",
                            [next_result_promise, sync_done](Context&, const std::vector<Value>& args) -> Value {
                                Value val = args.empty() ? Value() : args[0];
                                auto res_obj = ObjectFactory::create_object();
                                res_obj->set_property("value", val);
                                res_obj->set_property("done", Value(sync_done));
                                next_result_promise->fulfill(Value(res_obj.release()));
                                return Value();
                            });
                        auto unwrap_r = ObjectFactory::create_native_function("",
                            [next_result_promise, sync_done, iterator_val, gctx](Context&, const std::vector<Value>& args) -> Value {
                                Value reason = args.empty() ? Value() : args[0];
                                // closeOnRejection: a rejected value closes the sync iterator
                                // before the rejection propagates; close failures are swallowed.
                                if (!sync_done && iterator_val.is_object() && gctx) {
                                    Value close_fn = iterator_val.as_object()->get_property("return");
                                    if (!gctx->has_exception() && close_fn.is_function()) {
                                        close_fn.as_function()->call(*gctx, {}, iterator_val);
                                    }
                                    gctx->clear_exception();
                                }
                                next_result_promise->reject(reason);
                                return Value();
                            });
                        std::string vw_key = "afsi_" + std::to_string(i) + "_" + std::to_string(reinterpret_cast<uintptr_t>(exec));
                        Function* uf = unwrap_f.get(); Function* ur = unwrap_r.get();
                        value_wrapper->set_property("__af_" + vw_key, Value(unwrap_f.release()));
                        value_wrapper->set_property("__ar_" + vw_key, Value(unwrap_r.release()));
                        value_wrapper->then(uf, ur);
                    }

                    // Outer Await(nextResultPromise) pays its own constructor lookup + tick,
                    // even when next_result_promise was just rejected directly above.
                    next_result_promise->get_property("constructor");
                    rescue_getter_exception();
                    if (ctx.has_exception()) return Value();

                    next_result = Value(next_result_promise);
                    is_pending = true;
                    auto self = exec->shared_from_this();
                    auto on_f2 = ObjectFactory::create_native_function("",
                        [self, gctx](Context&, const std::vector<Value>& args) -> Value {
                            Value val = args.empty() ? Value() : args[0];
                            self->resume(val, false);
                            return Value();
                        });
                    auto on_r2 = ObjectFactory::create_native_function("",
                        [self, gctx](Context&, const std::vector<Value>& args) -> Value {
                            Value reason = args.empty() ? Value() : args[0];
                            self->resume(reason, true);
                            return Value();
                        });
                    std::string nrkey = "afsi_outer_" + std::to_string(i) + "_" + std::to_string(reinterpret_cast<uintptr_t>(exec));
                    Function* ff2 = on_f2.get(); Function* fr2 = on_r2.get();
                    next_result_promise->set_property("__af_" + nrkey, Value(on_f2.release()));
                    next_result_promise->set_property("__ar_" + nrkey, Value(on_r2.release()));
                    next_result_promise->then(ff2, fr2);
                } else {
                next_result = next_method_val.as_function()->call(ctx, {}, iterator_val);
                if (ctx.has_exception()) return Value();
                if (AsyncUtils::is_promise(next_result)) {
                    Promise* prom = static_cast<Promise*>(next_result.as_object());
                    // PromiseResolve on an already-Promise value still Gets "constructor"
                    // (step 1a) -- side effect only, no subclass rewrap.
                    prom->get_property("constructor");
                    if (!ctx.has_exception() && Object::current_context_ && Object::current_context_ != &ctx
                            && Object::current_context_->has_exception()) {
                        ctx.throw_exception(Object::current_context_->get_exception(), true);
                        Object::current_context_->clear_exception();
                    }
                    if (ctx.has_exception()) return Value();
                    if (prom->get_state() == PromiseState::FULFILLED) {
                        settled_val = prom->get_value();
                    } else if (prom->get_state() == PromiseState::REJECTED) {
                        settled_val = prom->get_value();
                        settled_throw = true;
                    } else {
                        is_pending = true;
                        auto self = exec->shared_from_this();
                        auto on_f = ObjectFactory::create_native_function("",
                            [self, gctx](Context&, const std::vector<Value>& args) -> Value {
                                Value val = args.empty() ? Value() : args[0];
                                self->resume(val, false);
                                return Value();
                            });
                        auto on_r = ObjectFactory::create_native_function("",
                            [self, gctx](Context&, const std::vector<Value>& args) -> Value {
                                Value reason = args.empty() ? Value() : args[0];
                                self->resume(reason, true);
                                return Value();
                            });
                        std::string key = std::to_string(i) + "_" + std::to_string(reinterpret_cast<uintptr_t>(exec));
                        Function* ff_tmp_ = on_f.get(); Function* fr_tmp_ = on_r.get();
                        prom->set_property("__af_" + key, Value(on_f.release()));
                        prom->set_property("__ar_" + key, Value(on_r.release()));
                        prom->then(ff_tmp_, fr_tmp_);
                    }
                } else {
                    if (ctx.has_exception()) return Value();
                    settled_val = next_result;
                }
                }

                if (!is_pending) {
                    auto self = exec->shared_from_this();
                    Value val = settled_val;
                    bool thr = settled_throw;
                    if (gctx) gctx->queue_microtask([self, val, thr]() mutable { self->resume(val, thr); }, {val});
                }

                exec->await_result_ = next_result;  // pin promise as GC root during suspension
                swapcontext(&exec->fiber_->fiber_ctx, &exec->fiber_->caller_ctx);
                Object::current_context_ = &ctx;

                if (exec->await_is_throw_) {
                    ctx.throw_exception(exec->await_result_, true);
                    exec->await_is_throw_ = false;
                    exec->await_result_ = Value();
                    return Value();
                }
                awaited = exec->await_result_;
                exec->await_result_ = Value();
            } else {
                Value next_method_val2 = iterator_obj->get_property("next");
                if (!next_method_val2.is_function()) {
                    ctx.throw_exception(Value(std::string("for-await-of: iterator has no next method")));
                    return Value();
                }
                Value next_result2 = next_method_val2.as_function()->call(ctx, {}, iterator_val);
                if (ctx.has_exception()) return Value();
                if (AsyncUtils::is_promise(next_result2)) {
                    Promise* p = static_cast<Promise*>(next_result2.as_object());
                    if (p->get_state() == PromiseState::FULFILLED) {
                        awaited = p->get_value();
                    } else if (p->get_state() == PromiseState::REJECTED) {
                        ctx.throw_exception(p->get_value());
                        return Value();
                    } else {
                        ctx.throw_exception(Value(std::string("for-await-of: pending promise outside async context")));
                        return Value();
                    }
                } else {
                    awaited = next_result2;
                }
            }

            if (!awaited.is_object()) {
                ctx.throw_exception(Value(std::string("for-await-of: iterator result must be an object")));
                return Value();
            }
            Object* iter_result = awaited.as_object();
            Value done = iter_result->get_property("done");
            if (done.to_boolean()) break;
            Value value = iter_result->get_property("value");

            // CreateAsyncFromSyncIterator (spec 27.1.4.2/.3): for sync-only iterables,
            // each yielded value is PromiseResolve'd and Awaited -- e.g.
            // `for await (const x of [Promise.resolve(1)])` must yield `1`, not the Promise.
            if (!used_async_iterator) {
                if (in_async_gen_fiber) {
                    bool v_is_pending = false;
                    bool v_settled_throw = false;
                    Value v_settled_val;
                    if (AsyncUtils::is_promise(value)) {
                        Promise* vp = static_cast<Promise*>(value.as_object());
                        if (vp->get_state() == PromiseState::FULFILLED) {
                            v_settled_val = vp->get_value();
                        } else if (vp->get_state() == PromiseState::REJECTED) {
                            v_settled_val = vp->get_value();
                            v_settled_throw = true;
                        } else {
                            v_is_pending = true;
                            auto self = async_gen;
                            auto on_f = ObjectFactory::create_native_function("",
                                [self, gctx](Context&, const std::vector<Value>& args) -> Value {
                                    Value val = args.empty() ? Value() : args[0];
                                    self->resume_from_await(val, false);
                                    return Value();
                                });
                            auto on_r = ObjectFactory::create_native_function("",
                                [self, gctx](Context&, const std::vector<Value>& args) -> Value {
                                    Value reason = args.empty() ? Value() : args[0];
                                    self->resume_from_await(reason, true);
                                    return Value();
                                });
                            std::string vkey = "fav_" + std::to_string(i) + "_ag_" + std::to_string(reinterpret_cast<uintptr_t>(async_gen));
                            Function* vff = on_f.get(); Function* vfr = on_r.get();
                            vp->set_property("__af_" + vkey, Value(on_f.release()));
                            vp->set_property("__ar_" + vkey, Value(on_r.release()));
                            vp->then(vff, vfr);
                        }
                    } else {
                        v_settled_val = value;
                    }
                    if (!v_is_pending) {
                        auto self = async_gen;
                        Value vv = v_settled_val;
                        bool vthr = v_settled_throw;
                        if (gctx) gctx->queue_microtask([self, vv, vthr]() mutable { self->resume_from_await(vv, vthr); }, {Value(self), vv});
                    }
                    async_gen->await_result_ = value;  // pin as GC root during suspension
                    async_gen->suspend_reason_ = AsyncGenerator::SuspendReason::Await;
                    swapcontext(&async_gen->fiber_->fiber_ctx, &async_gen->fiber_->caller_ctx);
                    Object::current_context_ = &ctx;
                    if (async_gen->await_is_throw_) {
                        ctx.throw_exception(async_gen->await_result_, true);
                        async_gen->await_is_throw_ = false;
                        async_gen->await_result_ = Value();
                        return Value();
                    }
                    value = async_gen->await_result_;
                    async_gen->await_result_ = Value();
                } else if (exec && !exec->fiber_stack_.empty()) {
                    // `value` was already fully wrapped, awaited, and unwrapped by the
                    // AsyncFromSyncIteratorContinuation step above (next_result_promise's
                    // resolution) -- nothing further to await here.
                } else {
                    if (AsyncUtils::is_promise(value)) {
                        Promise* vp = static_cast<Promise*>(value.as_object());
                        if (vp->get_state() == PromiseState::FULFILLED) {
                            value = vp->get_value();
                        } else if (vp->get_state() == PromiseState::REJECTED) {
                            ctx.throw_exception(vp->get_value());
                            return Value();
                        } else {
                            ctx.throw_exception(Value(std::string("for-await-of: pending promise outside async context")));
                            return Value();
                        }
                    }
                }
            }

            if (var_name == "__destr__") {
                if (left_->get_type() == Type::DESTRUCTURING_ASSIGNMENT) {
                    auto* d = static_cast<DestructuringAssignment*>(left_.get());
                    d->evaluate_with_value(ctx, value);
                } else {
                    AssignmentExpression::destructuring_assign(ctx, left_.get(), value);
                }
                // A setter invoked during destructuring may throw via Object::current_context_ instead of ctx -- rescue it.
                if (!ctx.has_exception() && Object::current_context_ && Object::current_context_ != &ctx
                        && Object::current_context_->has_exception()) {
                    ctx.throw_exception(Object::current_context_->get_exception(), true);
                    Object::current_context_->clear_exception();
                }
                if (ctx.has_exception()) return Value();
                body_->evaluate(ctx);
                if (ctx.has_exception()) return Value();
                if (ctx.has_break()) {
                    ctx.clear_break_continue();
                    close_async_iterator_on_break(iterator_obj);
                    if (ctx.has_exception()) return Value();
                    break;
                }
                if (ctx.has_continue()) { ctx.clear_break_continue(); continue; }
                if (ctx.has_return_value()) return Value();
                continue;
            }

            bool per_iter = (var_kind == VariableDeclarator::Kind::LET || var_kind == VariableDeclarator::Kind::CONST);
            if (per_iter) {
                ctx.push_block_scope();
                ctx.create_lexical_binding(var_name, value, var_kind != VariableDeclarator::Kind::CONST);
            } else if (ctx.has_binding(var_name)) {
                ctx.set_binding(var_name, value);
            } else {
                ctx.create_binding(var_name, value, true);
            }

            body_->evaluate(ctx);
            if (per_iter) ctx.pop_block_scope();
            if (ctx.has_exception()) return Value();
            if (ctx.has_break()) {
                ctx.clear_break_continue();
                close_async_iterator_on_break(iterator_obj);
                if (ctx.has_exception()) return Value();
                break;
            }
            if (ctx.has_continue()) { ctx.clear_break_continue(); continue; }
            if (ctx.has_return_value()) return Value();
        }
        return Value();
    }

    if (!iterable.is_object() && !iterable.is_string() && !iterable.is_function()) {
        ctx.throw_type_error(std::string(iterable.to_string()) + " is not iterable");
        return Value();
    }

    if (iterable.is_object() || iterable.is_string()) {
        Object* obj = nullptr;

        std::unique_ptr<Object> boxed_string = nullptr;

        if (iterable.is_string()) {
            boxed_string = std::make_unique<Object>();
            boxed_string->set_property("length", Value(static_cast<double>(utf16_length(iterable.to_string()))));

            Symbol* iterator_symbol = Symbol::get_well_known(Symbol::ITERATOR);
            if (iterator_symbol) {
                std::string str_value = iterable.to_string();
                auto string_iterator_fn = ObjectFactory::create_native_function("@@iterator",
                    [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                        (void)ctx; (void)args;
                        auto iterator = std::make_unique<StringIterator>(str_value);
                        return Value(iterator.release());
                    });
                boxed_string->set_property(iterator_symbol->to_property_key(), Value(string_iterator_fn.release()));
            }
            obj = boxed_string.get();
        } else {
            obj = iterable.as_object();
        }

        Symbol* iterator_symbol = Symbol::get_well_known(Symbol::ITERATOR);
        if (iterator_symbol && obj && obj->has_property(iterator_symbol->to_property_key())) {
            Value iterator_method = obj->get_property(iterator_symbol->to_property_key());
            if (iterator_method.is_function()) {
                Function* iter_fn = iterator_method.as_function();
                Value iterator_obj = iter_fn->call(ctx, {}, iterable);

                if (iterator_obj.is_object()) {
                    Object* iterator = iterator_obj.as_object();
                    Value next_method = iterator->get_property("next");

                    if (next_method.is_function()) {
                        Function* next_fn = next_method.as_function();

                        std::string var_name;
                        VariableDeclarator::Kind var_kind = VariableDeclarator::Kind::LET;

                        if (left_->get_type() == Type::VARIABLE_DECLARATION) {
                            VariableDeclaration* var_decl = static_cast<VariableDeclaration*>(left_.get());
                            if (var_decl->declaration_count() > 0) {
                                VariableDeclarator* declarator = var_decl->get_declarations()[0].get();
                                var_name = declarator->get_id()->get_name();
                                var_kind = declarator->get_kind();
                            }
                        } else if (left_->get_type() == Type::USING_DECLARATION) {
                            UsingDeclaration* using_decl = static_cast<UsingDeclaration*>(left_.get());
                            if (!using_decl->get_bindings().empty()) {
                                var_name = using_decl->get_bindings()[0].name;
                            }
                            var_kind = VariableDeclarator::Kind::CONST; // using bindings are immutable
                        } else if (left_->get_type() == Type::IDENTIFIER) {
                            // A bare identifier (no let/const/var) references an existing binding --
                            // must write through to it, not shadow it with a fresh per-iteration one.
                            Identifier* id = static_cast<Identifier*>(left_.get());
                            var_name = id->get_name();
                            var_kind = VariableDeclarator::Kind::VAR;
                        } else if (left_->get_type() == Type::DESTRUCTURING_ASSIGNMENT) {
                            var_name = "__destructuring__";
                        } else if (left_->get_type() == Type::ARRAY_LITERAL ||
                                   left_->get_type() == Type::OBJECT_LITERAL) {
                            var_name = "__pattern__";
                        } else if (left_->get_type() == Type::MEMBER_EXPRESSION) {
                            var_name = "__member__";
                        }

                        if (var_name.empty()) {
                            ctx.throw_exception(Value(std::string("For...of: Invalid loop variable")));
                            return Value();
                        }

                        // close_iterator: calls iterator.return() per spec IteratorClose.
                        // validate_result: when called from normal completion (break), check return() returns Object.
                        auto close_iterator = [&iterator_obj, &ctx](bool validate_result = false) {
                            if (!iterator_obj.is_object()) return;
                            bool had_exception = ctx.has_exception();
                            Value saved_exception = had_exception ? ctx.get_exception() : Value();
                            if (had_exception) ctx.clear_exception();

                            Value return_method = iterator_obj.as_object()->get_property("return");
                            bool inner_threw = ctx.has_exception();
                            if (!inner_threw) {
                                if (!return_method.is_undefined() && !return_method.is_null() && !return_method.is_function()) {
                                    // GetMethod: non-callable return throws TypeError (spec 7.3.9 step 4)
                                    ctx.throw_type_error("Iterator return method is not callable");
                                    inner_threw = true;
                                } else if (return_method.is_function()) {
                                    Value result = return_method.as_function()->call(ctx, {}, iterator_obj);
                                    inner_threw = ctx.has_exception();
                                    if (!inner_threw && validate_result && !result.is_object()) {
                                        ctx.throw_type_error("Iterator return() must return an Object");
                                        return;
                                    }
                                }
                            }

                            if (had_exception) {
                                // Suppress inner error; restore original throw completion
                                if (ctx.has_exception()) ctx.clear_exception();
                                ctx.throw_exception(saved_exception, true);
                            } else if (ctx.has_exception() && ctx.has_return_value()) {
                                // Closing on a `return` completion and return() threw:
                                // the throw replaces the return (spec IteratorClose step 5,
                                // innerResult abrupt wins) -- otherwise Function::call would
                                // see the pending return value first and swallow the throw.
                                ctx.clear_return_value();
                            }
                        };

                        Context* loop_ctx = &ctx;
                        Value V_iter;

                        while (true) {
                            Collector::safepoint();
                            Value result = next_fn->call(ctx, {}, iterator_obj);

                            // Per spec: if next() throws abruptly, do NOT close the iterator.
                            if (ctx.has_exception()) { return Value(); }

                            // Per spec 7.4.2: iterator result must be an Object
                            if (!result.is_object()) {
                                ctx.throw_type_error("Iterator result is not an object");
                                return Value();
                            }

                            {
                                Object* result_obj = result.as_object();
                                Value done = result_obj->get_property("done");
                                // Propagate getter exception (may land in Object::current_context_)
                                if (!ctx.has_exception() && Object::current_context_ && Object::current_context_ != &ctx
                                        && Object::current_context_->has_exception()) {
                                    ctx.throw_exception(Object::current_context_->get_exception(), true);
                                    Object::current_context_->clear_exception();
                                }
                                // Per spec: done/value getter throws → do NOT close iterator
                                if (ctx.has_exception()) return Value();

                                if (done.to_boolean()) {
                                    break;
                                }

                                Value value = result_obj->get_property("value");
                                if (!ctx.has_exception() && Object::current_context_ && Object::current_context_ != &ctx
                                        && Object::current_context_->has_exception()) {
                                    ctx.throw_exception(Object::current_context_->get_exception(), true);
                                    Object::current_context_->clear_exception();
                                }
                                if (ctx.has_exception()) return Value();

                                if (left_->get_type() == Type::MEMBER_EXPRESSION) {
                                    MemberExpression* member = static_cast<MemberExpression*>(left_.get());
                                    Value obj_val = member->get_object()->evaluate(*loop_ctx);
                                    if (loop_ctx->has_exception()) { close_iterator(); return Value(); }
                                    std::string prop_key;
                                    if (member->is_computed()) {
                                        Value key_val = member->get_property()->evaluate(*loop_ctx);
                                        if (loop_ctx->has_exception()) { close_iterator(); return Value(); }
                                        prop_key = key_val.to_string();
                                    } else {
                                        Identifier* prop_id = static_cast<Identifier*>(member->get_property());
                                        prop_key = prop_id->get_name();
                                    }
                                    Object* target_obj = obj_val.is_object() ? obj_val.as_object()
                                                        : obj_val.is_function() ? static_cast<Object*>(obj_val.as_function())
                                                        : nullptr;
                                    if (target_obj && !prop_key.empty() && prop_key[0] == '#') {
                                        if (!private_brand_check(*loop_ctx, target_obj, prop_key, false)) {
                                            loop_ctx->throw_type_error("Cannot write private member " + prop_key + " to an object whose class did not declare it");
                                            close_iterator();
                                            return Value();
                                        }
                                        std::string qualified = resolve_private_storage_key(prop_key, target_obj);
                                        if (target_obj->has_private_slot(qualified)) prop_key = qualified;
                                    }
                                    if (target_obj) target_obj->ordinary_set(prop_key, value);
                                } else if (left_->get_type() == Type::ARRAY_LITERAL ||
                                           left_->get_type() == Type::OBJECT_LITERAL) {
                                    AssignmentExpression::destructuring_assign(*loop_ctx, left_.get(), value);
                                    if (loop_ctx->has_exception()) { close_iterator(); return Value(); }
                                } else if (left_->get_type() == Type::DESTRUCTURING_ASSIGNMENT &&
                                           (left_decl_kind_ == 1 || left_decl_kind_ == 2)) {
                                    // for (let/const [x, y] of ...) -- per-iteration lexical scope
                                    auto* da = static_cast<DestructuringAssignment*>(left_.get());
                                    loop_ctx->push_block_scope();
                                    // Pre-declare bindings so evaluate_with_value uses the inner scope.
                                    // Skip targets that are property keys (appear as property_name in
                                    // property_mappings) -- they are destructuring keys, not binding vars.
                                    for (const auto& target : da->get_targets()) {
                                        if (!target || target->get_name().empty()) continue;
                                        const std::string& tname = target->get_name();
                                        bool is_key = false;
                                        for (const auto& pm : da->get_property_mappings()) {
                                            if (pm.property_name == tname) { is_key = true; break; }
                                        }
                                        if (!is_key)
                                            loop_ctx->create_lexical_binding(tname, Value(), true);
                                    }
                                    for (const auto& pm : da->get_property_mappings()) {
                                        if (!pm.variable_name.empty())
                                            loop_ctx->create_lexical_binding(pm.variable_name, Value(), true);
                                    }
                                    da->evaluate_with_value(*loop_ctx, value);
                                    if (!loop_ctx->has_exception()) {
                                        Value br = body_->evaluate(*loop_ctx);
                                        if (!g_empty_completion) V_iter = br;
                                    }
                                    loop_ctx->pop_block_scope();
                                    if (loop_ctx->has_exception()) { close_iterator(); return Value(); }
                                    if (loop_ctx->has_break()) {
                                        close_iterator(true);
                                        if (loop_ctx->has_exception()) return Value();
                                        if (loop_ctx->get_break_label().empty()) loop_ctx->clear_break_continue();
                                        break;
                                    }
                                    if (loop_ctx->has_continue()) {
                                        if (loop_ctx->get_continue_label().empty() ||
                                            loop_ctx->get_continue_label() == this_loop_label) {
                                            loop_ctx->clear_break_continue();
                                            continue;
                                        }
                                        close_iterator();
                                        g_empty_completion = false;
                                        return V_iter;
                                    }
                                    if (loop_ctx->has_return_value()) { close_iterator(); return Value(); }
                                    continue;
                                } else if (left_->get_type() == Type::DESTRUCTURING_ASSIGNMENT) {
                                    DestructuringAssignment* destructuring = static_cast<DestructuringAssignment*>(left_.get());
                                    destructuring->evaluate_with_value(*loop_ctx, value);
                                    if (loop_ctx->has_exception()) { close_iterator(); return Value(); }
                                } else {
                                    bool forof_per_iter = (var_kind == VariableDeclarator::Kind::LET ||
                                                          var_kind == VariableDeclarator::Kind::CONST);
                                    bool is_using = (left_->get_type() == Type::USING_DECLARATION);
                                    bool using_is_await = is_using &&
                                        static_cast<UsingDeclaration*>(left_.get())->is_await();
                                    if (forof_per_iter) {
                                        loop_ctx->push_block_scope();
                                        loop_ctx->create_lexical_binding(var_name, value, var_kind != VariableDeclarator::Kind::CONST);
                                    } else if (loop_ctx->has_binding(var_name)) {
                                        loop_ctx->set_binding(var_name, value);
                                    } else {
                                        bool is_mutable = (var_kind != VariableDeclarator::Kind::CONST);
                                        loop_ctx->create_binding(var_name, value, is_mutable);
                                    }

                                    // ForIn/OfBodyEvaluation: a using/await using LHS opens a fresh
                                    // DisposeCapability for this iteration's environment, disposed
                                    // (possibly awaited) right after the body, before checking completion.
                                    if (is_using) {
                                        loop_ctx->push_dispose_scope();
                                        if (!register_disposable_resource(*loop_ctx, value, using_is_await)) {
                                            loop_ctx->run_dispose_resources();
                                            if (forof_per_iter) loop_ctx->pop_block_scope();
                                            close_iterator();
                                            return Value();
                                        }
                                    }

                                    {
                                        Value br = body_->evaluate(*loop_ctx);
                                        if (!g_empty_completion) V_iter = br;
                                    }

                                    if (is_using) {
                                        loop_ctx->run_dispose_resources();
                                    }

                                    if (forof_per_iter) {
                                        loop_ctx->pop_block_scope();
                                    }

                                    if (loop_ctx->has_exception()) {
                                        close_iterator();
                                        return Value();
                                    }

                                    if (loop_ctx->has_break()) {
                                        close_iterator(true);
                                        if (loop_ctx->has_exception()) return Value();
                                        if (loop_ctx->get_break_label().empty()) {
                                            loop_ctx->clear_break_continue();
                                        }
                                        break;
                                    }
                                    if (loop_ctx->has_continue()) {
                                        if (loop_ctx->get_continue_label().empty() ||
                                            loop_ctx->get_continue_label() == this_loop_label) {
                                            loop_ctx->clear_break_continue();
                                            continue;
                                        }
                                        close_iterator();
                                        g_empty_completion = false;
                                        return V_iter;
                                    }
                                    if (loop_ctx->has_return_value()) {
                                        close_iterator();
                                        return Value();
                                    }
                                    continue;
                                }

                                {
                                    Value br = body_->evaluate(*loop_ctx);
                                    if (!g_empty_completion) V_iter = br;
                                }
                                if (loop_ctx->has_exception()) {
                                    close_iterator();
                                    return Value();
                                }

                                if (loop_ctx->has_break()) {
                                    close_iterator(true);  // normal completion: validate return() result
                                    if (loop_ctx->has_exception()) return Value();
                                    if (loop_ctx->get_break_label().empty()) {
                                        loop_ctx->clear_break_continue();
                                    }
                                    break;
                                }
                                if (loop_ctx->has_continue()) {
                                    if (loop_ctx->get_continue_label().empty() ||
                                        loop_ctx->get_continue_label() == this_loop_label) {
                                        loop_ctx->clear_break_continue();
                                        continue;
                                    }
                                    close_iterator();
                                    g_empty_completion = false;
                                    return V_iter;
                                }
                                if (loop_ctx->has_return_value()) {
                                    close_iterator();
                                    return Value();
                                }
                            }
                        }

                        g_empty_completion = false;
                        return V_iter;
                    }
                }
            }
        }

        if (obj->get_type() == Object::ObjectType::Array) {
            uint32_t length = obj->get_length();

            if (length > 50) {
                ctx.throw_exception(Value(std::string("For...of: Array too large (>50 elements)")));
                return Value();
            }

            std::string var_name;
            VariableDeclarator::Kind var_kind = VariableDeclarator::Kind::LET;

            if (left_->get_type() == Type::VARIABLE_DECLARATION) {
                VariableDeclaration* var_decl = static_cast<VariableDeclaration*>(left_.get());
                if (var_decl->declaration_count() > 0) {
                    VariableDeclarator* declarator = var_decl->get_declarations()[0].get();
                    var_name = declarator->get_id()->get_name();
                    var_kind = declarator->get_kind();
                }
            } else if (left_->get_type() == Type::IDENTIFIER) {
                Identifier* id = static_cast<Identifier*>(left_.get());
                var_name = id->get_name();
            } else if (left_->get_type() == Type::DESTRUCTURING_ASSIGNMENT) {
                var_name = "__destructuring_temp__";
            } else if (left_->get_type() == Type::ARRAY_LITERAL ||
                       left_->get_type() == Type::OBJECT_LITERAL) {
                var_name = "__pattern__";
            } else if (left_->get_type() == Type::MEMBER_EXPRESSION) {
                var_name = "__member__";
            }

            if (var_name.empty()) {
                ctx.throw_exception(Value(std::string("For...of: Invalid loop variable")));
                return Value();
            }

            Context* loop_ctx = &ctx;

            Value V_arr;

            for (uint32_t i = 0; i < length; i++) {
                Collector::safepoint();

                Value element = obj->get_element(i);

                if (left_->get_type() == Type::MEMBER_EXPRESSION) {
                    MemberExpression* member = static_cast<MemberExpression*>(left_.get());
                    Value obj_val = member->get_object()->evaluate(*loop_ctx);
                    if (loop_ctx->has_exception()) return Value();
                    std::string prop_key;
                    if (member->is_computed()) {
                        Value key_val = member->get_property()->evaluate(*loop_ctx);
                        if (loop_ctx->has_exception()) return Value();
                        prop_key = key_val.to_string();
                    } else {
                        Identifier* prop_id = static_cast<Identifier*>(member->get_property());
                        prop_key = prop_id->get_name();
                    }
                    Object* target_obj = obj_val.is_object() ? obj_val.as_object()
                                        : obj_val.is_function() ? static_cast<Object*>(obj_val.as_function())
                                        : nullptr;
                    if (target_obj && !prop_key.empty() && prop_key[0] == '#') {
                        if (!private_brand_check(*loop_ctx, target_obj, prop_key, false)) {
                            loop_ctx->throw_type_error("Cannot write private member " + prop_key + " to an object whose class did not declare it");
                            return Value();
                        }
                        std::string qualified = resolve_private_storage_key(prop_key, target_obj);
                        if (target_obj->has_private_slot(qualified)) prop_key = qualified;
                    }
                    if (target_obj) target_obj->ordinary_set(prop_key, element);
                } else if (left_->get_type() == Type::ARRAY_LITERAL ||
                           left_->get_type() == Type::OBJECT_LITERAL) {
                    AssignmentExpression::destructuring_assign(*loop_ctx, left_.get(), element);
                    if (loop_ctx->has_exception()) return Value();
                } else if (left_->get_type() == Type::DESTRUCTURING_ASSIGNMENT) {
                    DestructuringAssignment* destructuring = static_cast<DestructuringAssignment*>(left_.get());
                    bool da_per_iter = (left_decl_kind_ == 1 || left_decl_kind_ == 2);
                    if (da_per_iter) {
                        loop_ctx->push_block_scope();
                        for (const auto& target : destructuring->get_targets()) {
                            if (!target || target->get_name().empty()) continue;
                            const std::string& tname = target->get_name();
                            bool is_key = false;
                            for (const auto& pm : destructuring->get_property_mappings()) {
                                if (pm.property_name == tname) { is_key = true; break; }
                            }
                            if (!is_key)
                                loop_ctx->create_lexical_binding(tname, Value(), true);
                        }
                        for (const auto& pm : destructuring->get_property_mappings()) {
                            if (!pm.variable_name.empty())
                                loop_ctx->create_lexical_binding(pm.variable_name, Value(), true);
                        }
                        destructuring->evaluate_with_value(*loop_ctx, element);
                        if (!loop_ctx->has_exception() && body_) {
                            Value result = body_->evaluate(*loop_ctx);
                            if (!g_empty_completion) V_arr = result;
                        }
                        loop_ctx->pop_block_scope();
                        if (loop_ctx->has_exception()) { ctx.throw_exception(loop_ctx->get_exception(), true); return Value(); }
                        if (loop_ctx->has_return_value()) { ctx.set_return_value(loop_ctx->get_return_value()); return Value(); }
                        if (loop_ctx->has_break()) {
                            if (loop_ctx->get_break_label().empty()) loop_ctx->clear_break_continue();
                            break;
                        }
                        if (loop_ctx->has_continue()) {
                            if (loop_ctx->get_continue_label().empty() ||
                                loop_ctx->get_continue_label() == this_loop_label) {
                                loop_ctx->clear_break_continue();
                            } else {
                                break;
                            }
                        }
                        continue;
                    }

                    std::unique_ptr<ASTNode> temp_literal;
                    Position dummy_pos(0, 0);

                    if (element.is_string()) {
                        temp_literal = std::make_unique<StringLiteral>(element.to_string(), dummy_pos, dummy_pos);
                    } else if (element.is_number()) {
                        temp_literal = std::make_unique<NumberLiteral>(element.to_number(), dummy_pos, dummy_pos);
                    } else if (element.is_boolean()) {
                        temp_literal = std::make_unique<BooleanLiteral>(element.to_boolean(), dummy_pos, dummy_pos);
                    } else if (element.is_null()) {
                        temp_literal = std::make_unique<NullLiteral>(dummy_pos, dummy_pos);
                    } else if (element.is_undefined()) {
                        temp_literal = std::make_unique<UndefinedLiteral>(dummy_pos, dummy_pos);
                    } else {
                        std::string temp_var = "__temp_destructure_" + std::to_string(i);
                        loop_ctx->create_binding(temp_var, element, true);
                        temp_literal = std::make_unique<Identifier>(temp_var, dummy_pos, dummy_pos);
                    }

                    destructuring->set_source(std::move(temp_literal));
                    destructuring->evaluate(*loop_ctx);
                } else {
                    bool forof_arr_per_iter = (var_kind == VariableDeclarator::Kind::LET ||
                                               var_kind == VariableDeclarator::Kind::CONST);
                    if (forof_arr_per_iter) {
                        loop_ctx->push_block_scope();
                        loop_ctx->create_lexical_binding(var_name, element, var_kind != VariableDeclarator::Kind::CONST);
                    } else if (loop_ctx->has_binding(var_name)) {
                        loop_ctx->set_binding(var_name, element);
                    } else {
                        loop_ctx->create_binding(var_name, element, true);
                    }

                    if (body_) {
                        Value result = body_->evaluate(*loop_ctx);
                        if (!g_empty_completion) V_arr = result;
                        if (forof_arr_per_iter) {
                            loop_ctx->pop_block_scope();
                        }
                        if (loop_ctx->has_exception()) {
                            ctx.throw_exception(loop_ctx->get_exception(), true);
                            return Value();
                        }
                        if (loop_ctx->has_return_value()) {
                            ctx.set_return_value(loop_ctx->get_return_value());
                            return Value();
                        }
                        if (loop_ctx->has_break()) {
                            if (loop_ctx->get_break_label().empty()) {
                                loop_ctx->clear_break_continue();
                            }
                            break;
                        }
                        if (loop_ctx->has_continue()) {
                            if (loop_ctx->get_continue_label().empty() ||
                                loop_ctx->get_continue_label() == this_loop_label) {
                                loop_ctx->clear_break_continue();
                            } else {
                                break; // labelled continue -- propagate up
                            }
                        }
                    } else if (forof_arr_per_iter) {
                        loop_ctx->pop_block_scope();
                    }
                }
            }

            g_empty_completion = false;
            return V_arr;
        } else {
            ctx.throw_type_error("object is not iterable");
            return Value();
        }
    } else {
        ctx.throw_type_error("For...of: value is not iterable");
        return Value();
    }

    return Value();
}

std::string ForOfStatement::to_string() const {
    std::ostringstream oss;
    if (is_await_) {
        oss << "for await (" << left_->to_string() << " of " << right_->to_string() << ") " << body_->to_string();
    } else {
        oss << "for (" << left_->to_string() << " of " << right_->to_string() << ") " << body_->to_string();
    }
    return oss.str();
}

std::unique_ptr<ASTNode> ForOfStatement::clone() const {
    return std::make_unique<ForOfStatement>(
        left_->clone(), right_->clone(), body_->clone(), is_await_, start_, end_, left_decl_kind_
    );
}


Value WhileStatement::evaluate(Context& ctx) {
    std::string this_loop_label = ctx.get_next_statement_label();
    ctx.set_next_statement_label("");

    std::string prev_loop_label = ctx.get_current_loop_label();
    ctx.set_current_loop_label(this_loop_label);

    Value V;

    try {
        while (true) {
            Collector::safepoint();

            Value test_value;
            try {
                test_value = test_->evaluate(ctx);
                if (ctx.has_exception()) {
                    ctx.set_current_loop_label(prev_loop_label);
                    return Value();
                }
            } catch (...) {
                ctx.set_current_loop_label(prev_loop_label);
                ctx.throw_exception(Value(std::string("Error evaluating while-loop condition")));
                return Value();
            }

            if (!test_value.to_boolean()) {
                break;
            }

            try {
                Value body_result = body_->evaluate(ctx);
                if (!g_empty_completion) V = body_result;
                if (ctx.has_exception()) {
                    ctx.set_current_loop_label(prev_loop_label);
                    return Value();
                }
                if (ctx.has_return_value()) {
                    ctx.set_current_loop_label(prev_loop_label);
                    return ctx.get_return_value();
                }

                if (ctx.has_break()) {
                    if (ctx.get_break_label().empty()) {
                        ctx.clear_break_continue();
                        break;
                    }
                    break;
                }
                if (ctx.has_continue()) {
                    if (ctx.get_continue_label().empty()) {
                        ctx.clear_break_continue();
                        continue;
                    }
                    if (ctx.get_continue_label() == ctx.get_current_loop_label()) {
                        ctx.clear_break_continue();
                        continue;
                    }
                    break;
                }
            } catch (const YieldException&) {
                throw;
            } catch (const GeneratorReturnException&) {
                ctx.set_current_loop_label(prev_loop_label);
                throw;
            } catch (...) {
                ctx.throw_exception(Value(std::string("Error in while-loop body execution")));
                ctx.set_current_loop_label(prev_loop_label);
                return Value();
            }
        }
    } catch (const YieldException&) {
        throw;
    } catch (const GeneratorReturnException&) {
        ctx.set_current_loop_label(prev_loop_label);
        throw;
    } catch (...) {
        ctx.throw_exception(Value(std::string("Fatal error in while-loop execution")));
        ctx.set_current_loop_label(prev_loop_label);
        return Value();
    }

    ctx.set_current_loop_label(prev_loop_label);
    g_empty_completion = false;
    return V;
}

std::string WhileStatement::to_string() const {
    return "while (" + test_->to_string() + ") " + body_->to_string();
}

std::unique_ptr<ASTNode> WhileStatement::clone() const {
    return std::make_unique<WhileStatement>(
        test_->clone(), body_->clone(), start_, end_
    );
}


Value DoWhileStatement::evaluate(Context& ctx) {
    std::string this_loop_label = ctx.get_next_statement_label();
    ctx.set_next_statement_label("");
    std::string prev_loop_label = ctx.get_current_loop_label();
    ctx.set_current_loop_label(this_loop_label);

    Value V;

    try {
        do {
            Collector::safepoint();

            try {
                Value body_result = body_->evaluate(ctx);
                if (!g_empty_completion) V = body_result;
                if (ctx.has_exception()) {
                    ctx.set_current_loop_label(prev_loop_label);
                    return Value();
                }
                if (ctx.has_return_value()) {
                    ctx.set_current_loop_label(prev_loop_label);
                    return ctx.get_return_value();
                }

                if (ctx.has_break()) {
                    if (ctx.get_break_label().empty()) {
                        ctx.clear_break_continue();
                    }
                    ctx.set_current_loop_label(prev_loop_label);
                    return V;
                }
                if (ctx.has_continue()) {
                    if (ctx.get_continue_label().empty() ||
                            ctx.get_continue_label() == this_loop_label) {
                        ctx.clear_break_continue();
                    } else {
                        ctx.set_current_loop_label(prev_loop_label);
                        return V;
                    }
                }

            } catch (const YieldException&) {
                throw;
            } catch (const GeneratorReturnException&) {
                ctx.set_current_loop_label(prev_loop_label);
                throw;
            } catch (...) {
                ctx.set_current_loop_label(prev_loop_label);
                ctx.throw_exception(Value(std::string("Error in do-while-loop body execution")));
                return Value();
            }

            Value test_value;
            try {
                test_value = test_->evaluate(ctx);
                if (ctx.has_exception()) {
                    ctx.set_current_loop_label(prev_loop_label);
                    return Value();
                }
            } catch (...) {
                ctx.set_current_loop_label(prev_loop_label);
                ctx.throw_exception(Value(std::string("Error evaluating do-while-loop condition")));
                return Value();
            }

            if (!test_value.to_boolean()) {
                break;
            }

        } while (true);

    } catch (const YieldException&) {
        throw;
    } catch (const GeneratorReturnException&) {
        ctx.set_current_loop_label(prev_loop_label);
        throw;
    } catch (...) {
        ctx.set_current_loop_label(prev_loop_label);
        ctx.throw_exception(Value(std::string("Fatal error in do-while-loop execution")));
        return Value();
    }

    ctx.set_current_loop_label(prev_loop_label);
    g_empty_completion = false;
    return V;
}

std::string DoWhileStatement::to_string() const {
    return "do " + body_->to_string() + " while (" + test_->to_string() + ")";
}

std::unique_ptr<ASTNode> DoWhileStatement::clone() const {
    return std::make_unique<DoWhileStatement>(
        body_->clone(), test_->clone(), start_, end_
    );
}


Value WithStatement::evaluate(Context& ctx) {
    if (ctx.is_strict_mode()) {
        ctx.throw_syntax_error("Strict mode code may not include a with statement");
        return Value();
    }

    Value obj_value = object_->evaluate(ctx);
    if (ctx.has_exception()) return Value();

    // ToObject: null/undefined throw TypeError, primitives get wrapper objects
    if (obj_value.is_null() || obj_value.is_undefined()) {
        ctx.throw_type_error("Cannot convert undefined or null to object in with statement");
        return Value();
    }

    Object* obj = nullptr;
    if (obj_value.is_object()) {
        obj = obj_value.as_object();
    } else if (obj_value.is_function()) {
        obj = obj_value.as_function();
    } else {
        // Number, String, Boolean, Symbol primitives -- create a wrapper object
        // The wrapper has no own properties so with(primitive) essentially adds nothing to scope
        auto wrapper = ObjectFactory::create_object();
        obj = wrapper.release();
    }
    if (!obj) {
        ctx.throw_type_error("with statement: failed to create object");
        return Value();
    }

    ctx.push_with_scope(obj);

    try {
        Value result = body_->evaluate(ctx);
        ctx.pop_with_scope();
        return result;
    } catch (...) {
        ctx.pop_with_scope();
        throw;
    }
}

std::string WithStatement::to_string() const {
    return "with (" + object_->to_string() + ") " + body_->to_string();
}

std::unique_ptr<ASTNode> WithStatement::clone() const {
    return std::make_unique<WithStatement>(
        object_->clone(), body_->clone(), start_, end_
    );
}


Value ReturnStatement::evaluate(Context& ctx) {
    Value return_value;

    if (has_argument()) {
        return_value = argument_->evaluate(ctx);
        if (ctx.has_exception()) return Value();

        // Only an async *generator*'s `return <expr>;` explicitly Awaits the value -- a plain
        // async function's return has no separate Await step.
        AsyncGenerator* async_gen = AsyncGenerator::get_current();
        if (async_gen && async_gen->get_generator_context() == &ctx) {
            Value awaited;
            bool threw = await_value(ctx, return_value, awaited);
            if (threw) { ctx.throw_exception(awaited, true); return Value(); }
            return_value = awaited;
        }
    } else {
        return_value = Value();
    }

    ctx.set_return_value(return_value);
    return return_value;
}

std::string ReturnStatement::to_string() const {
    std::ostringstream oss;
    oss << "return";
    if (has_argument()) {
        oss << " " << argument_->to_string();
    }
    oss << ";";
    return oss.str();
}

std::unique_ptr<ASTNode> ReturnStatement::clone() const {
    std::unique_ptr<ASTNode> cloned_argument = nullptr;
    if (has_argument()) {
        cloned_argument = argument_->clone();
    }

    return std::make_unique<ReturnStatement>(std::move(cloned_argument), start_, end_);
}


Value BreakStatement::evaluate(Context& ctx) {
    ctx.set_break(label_);
    g_empty_completion = true;
    return Value();
}

std::string BreakStatement::to_string() const {
    return label_.empty() ? "break;" : "break " + label_ + ";";
}

std::unique_ptr<ASTNode> BreakStatement::clone() const {
    return std::make_unique<BreakStatement>(start_, end_, label_);
}


Value ContinueStatement::evaluate(Context& ctx) {
    ctx.set_continue(label_);
    g_empty_completion = true;
    return Value();
}

std::string ContinueStatement::to_string() const {
    return label_.empty() ? "continue;" : "continue " + label_ + ";";
}

std::unique_ptr<ASTNode> ContinueStatement::clone() const {
    return std::make_unique<ContinueStatement>(start_, end_, label_);
}


Value TryStatement::evaluate(Context& ctx) {
    static int try_recursion_depth = 0;
    if (try_recursion_depth > 10) {
        return Value(std::string("Max try-catch recursion exceeded"));
    }

    try_recursion_depth++;

    Value result;
    Value exception_value;
    bool caught_exception = false;

    try {
        result = try_block_->evaluate(ctx);

        if (ctx.has_exception()) {
            caught_exception = true;
            exception_value = ctx.get_exception();
            ctx.clear_exception();
        }
    } catch (const YieldException&) {
        try_recursion_depth--;
        throw;
    } catch (const GeneratorReturnException&) {
        // Generator return: run finally (if any), then re-throw.
        // Return here to prevent the normal finally block below from running twice.
        try_recursion_depth--;
        if (!finally_block_) {
            throw;
        }
        finally_block_->evaluate(ctx);
        // If finally had an abrupt completion, it takes precedence over the return.
        if (ctx.has_exception() || ctx.has_return_value() || ctx.has_break() || ctx.has_continue()) {
            return Value(); // propagate finally's completion via ctx
        }
        throw; // finally completed normally -- re-throw the generator return
    } catch (const std::exception& e) {
        caught_exception = true;
        if (ctx.has_exception()) {
            exception_value = ctx.get_exception();
            ctx.clear_exception();
        } else {
            ctx.throw_exception(Value(std::string(e.what())));
            exception_value = ctx.get_exception();
            ctx.clear_exception();
        }
    } catch (...) {
        caught_exception = true;
        exception_value = Value(std::string("Error: Unknown error"));
    }

    bool catch_param_ok = true;
    if (caught_exception && catch_clause_) {
        CatchClause* catch_node = static_cast<CatchClause*>(catch_clause_.get());

        // Spec sec-runtime-semantics-catchclauseevaluation: create a new scope
        // for the catch parameter so it doesn't shadow or pollute the outer env.
        Environment* catch_old_env = ctx.get_lexical_environment();
        ctx.push_block_scope();

        if (!catch_node->get_parameter_name().empty()) {
            std::string param_name = catch_node->get_parameter_name();

            if (param_name == "__destr_pattern__" && catch_node->get_destructuring_pattern()) {
                auto* destr = static_cast<DestructuringAssignment*>(catch_node->get_destructuring_pattern());
                // Pre-create lexical bindings so destructuring writes to catch scope; skip targets that are only a nested pattern's outer property key (e.g. `w` in `{w: {x,y,z}}`).
                for (const auto& tgt : destr->get_targets()) {
                    if (!tgt || tgt->get_name().empty()) continue;
                    const std::string& tname = tgt->get_name();
                    bool is_key = false;
                    for (const auto& m : destr->get_property_mappings()) {
                        if (m.property_name == tname) { is_key = true; break; }
                    }
                    if (!is_key)
                        ctx.create_lexical_binding(tname, Value(), true);
                }
                destr->evaluate_with_value(ctx, exception_value);
                if (ctx.has_exception()) { catch_param_ok = false; }
            } else if (param_name.length() > 14 && param_name.substr(0, 14) == "__destr_array:") {
                std::string vars_str = param_name.substr(14);
                std::vector<std::string> var_names;
                std::string cur;
                for (char c : vars_str) {
                    if (c == ',') { if (!cur.empty()) { var_names.push_back(cur); cur.clear(); } }
                    else cur += c;
                }
                if (!cur.empty()) var_names.push_back(cur);

                if (exception_value.is_object()) {
                    Object* arr = exception_value.as_object();
                    for (size_t vi = 0; vi < var_names.size(); vi++) {
                        Value el = arr->get_element(static_cast<uint32_t>(vi));
                        ctx.create_lexical_binding(var_names[vi], el, true);
                    }
                }
            } else if (param_name.length() > 12 && param_name.substr(0, 12) == "__destr_obj:") {
                std::string vars_str = param_name.substr(12);
                std::vector<std::string> var_names;
                std::string cur;
                for (char c : vars_str) {
                    if (c == ',') { if (!cur.empty()) { var_names.push_back(cur); cur.clear(); } }
                    else cur += c;
                }
                if (!cur.empty()) var_names.push_back(cur);

                if (exception_value.is_object()) {
                    Object* obj = exception_value.as_object();
                    for (const auto& vn : var_names) {
                        Value val = obj->get_property(vn);
                        ctx.create_lexical_binding(vn, val, true);
                    }
                }
            } else {
                ctx.create_lexical_binding(param_name, exception_value, true);
            }
        }

        if (catch_param_ok) {
            try {
                result = catch_node->get_body()->evaluate(ctx);
            } catch (const YieldException&) {
                ctx.set_lexical_environment(catch_old_env);
                try_recursion_depth--;
                throw;
            } catch (const GeneratorReturnException&) {
                ctx.set_lexical_environment(catch_old_env);
                try_recursion_depth--;
                if (!finally_block_) throw;
                finally_block_->evaluate(ctx);
                if (ctx.has_exception() || ctx.has_return_value() || ctx.has_break() || ctx.has_continue()) {
                    return Value();
                }
                throw;
            } catch (const std::exception& e) {
                if (!ctx.has_exception()) {
                    ctx.throw_exception(Value(std::string(e.what())));
                }
            } catch (...) {
                if (!ctx.has_exception()) {
                    ctx.throw_exception(Value(std::string("Unknown error in catch block")));
                }
            }
        }

        ctx.set_lexical_environment(catch_old_env);
    }

    // No catch clause: restore the exception onto ctx so the finally save/restore logic below sees it.
    if (caught_exception && !catch_clause_ && !ctx.has_exception()) {
        ctx.throw_exception(exception_value, true);
    }

    if (finally_block_) {
        // Save abrupt completion from try/catch so finally can override it
        bool saved_break     = ctx.has_break();
        bool saved_continue  = ctx.has_continue();
        std::string saved_break_label    = ctx.get_break_label();
        std::string saved_continue_label = ctx.get_continue_label();
        bool saved_return    = ctx.has_return_value();
        Value saved_ret_val  = saved_return ? ctx.get_return_value() : Value();
        Value saved_exception = ctx.has_exception() ? ctx.get_exception() : Value();
        bool saved_has_exc   = ctx.has_exception();

        if (saved_break)    ctx.clear_break_continue();
        if (saved_continue) ctx.clear_break_continue();
        if (saved_return)   ctx.clear_return_value();
        if (saved_has_exc)  ctx.clear_exception();

        try {
            finally_block_->evaluate(ctx);
        } catch (const GeneratorReturnException&) {
            throw; // yield-in-finally resumed via .return(): propagate, don't swallow
        } catch (const std::exception& e) {
            std::cerr << "Finally block error: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "Finally block unknown error" << std::endl;
        }

        // If finally completed normally (no abrupt), restore try/catch's completion
        bool finally_abrupt = ctx.has_break() || ctx.has_continue() ||
                              ctx.has_return_value() || ctx.has_exception();
        if (!finally_abrupt) {
            if (saved_has_exc)  ctx.throw_exception(saved_exception, true);
            if (saved_return)   { ctx.set_return_value(saved_ret_val); }
            if (saved_break)    ctx.set_break(saved_break_label);
            if (saved_continue) ctx.set_continue(saved_continue_label);
        }
    } else if (ctx.has_exception() && !catch_param_ok) {
        // Destructuring threw -- let exception propagate (don't clear it)
    }

    try_recursion_depth--;
    return result;
}

std::string TryStatement::to_string() const {
    std::string result = "try " + try_block_->to_string();

    if (catch_clause_) {
        result += " " + catch_clause_->to_string();
    }

    if (finally_block_) {
        result += " finally " + finally_block_->to_string();
    }

    return result;
}

std::unique_ptr<ASTNode> TryStatement::clone() const {
    auto cloned_try = try_block_->clone();
    auto cloned_catch = catch_clause_ ? catch_clause_->clone() : nullptr;
    auto cloned_finally = finally_block_ ? finally_block_->clone() : nullptr;

    return std::make_unique<TryStatement>(
        std::move(cloned_try),
        std::move(cloned_catch),
        std::move(cloned_finally),
        start_, end_
    );
}

Value CatchClause::evaluate(Context& ctx) {
    return body_->evaluate(ctx);
}

std::string CatchClause::to_string() const {
    return "catch (" + parameter_name_ + ") " + body_->to_string();
}

std::unique_ptr<ASTNode> CatchClause::clone() const {
    auto c = std::make_unique<CatchClause>(parameter_name_, body_->clone(), start_, end_);
    if (destructuring_pattern_) c->set_destructuring_pattern(destructuring_pattern_->clone());
    return c;
}

Value ThrowStatement::evaluate(Context& ctx) {
    Value exception_value = expression_->evaluate(ctx);
    if (ctx.has_exception()) return Value();

    ctx.throw_exception(exception_value, true);
    return Value();
}

std::string ThrowStatement::to_string() const {
    return "throw " + expression_->to_string();
}

std::unique_ptr<ASTNode> ThrowStatement::clone() const {
    return std::make_unique<ThrowStatement>(expression_->clone(), start_, end_);
}

Value SwitchStatement::evaluate(Context& ctx) {
    std::string this_switch_label = ctx.get_next_statement_label();
    ctx.set_next_statement_label("");

    Value discriminant_value = discriminant_->evaluate(ctx);
    if (ctx.has_exception()) return Value();

    // Create block environment for switch (spec sec-switch-statement step 3-6)
    Environment* old_env = ctx.get_lexical_environment();
    auto block_env = std::make_unique<Environment>(Environment::Type::Declarative, old_env);
    Environment* block_env_ptr = block_env.release();
    ctx.set_lexical_environment(block_env_ptr);

    int matching_case_index = -1;
    int default_case_index = -1;

    for (size_t i = 0; i < cases_.size(); i++) {
        CaseClause* case_clause = static_cast<CaseClause*>(cases_[i].get());

        if (case_clause->is_default()) {
            default_case_index = static_cast<int>(i);
        } else {
            Value test_value = case_clause->get_test()->evaluate(ctx);
            if (ctx.has_exception()) { ctx.set_lexical_environment(old_env); return Value(); }

            if (discriminant_value.strict_equals(test_value)) {
                matching_case_index = static_cast<int>(i);
                break;
            }
        }
    }

    int start_index = -1;
    if (matching_case_index >= 0) {
        start_index = matching_case_index;
    } else if (default_case_index >= 0) {
        start_index = default_case_index;
    }

    if (start_index < 0) {
        ctx.set_lexical_environment(old_env);
        return Value();
    }

    Value V;

    for (size_t i = static_cast<size_t>(start_index); i < cases_.size(); i++) {
        CaseClause* case_clause = static_cast<CaseClause*>(cases_[i].get());

        for (const auto& stmt : case_clause->get_consequent()) {
            Value result = stmt->evaluate(ctx);
            if (!g_empty_completion) V = result;
            if (ctx.has_exception()) { ctx.set_lexical_environment(old_env); return Value(); }

            if (ctx.has_break()) {
                // Only a break targeting this switch itself (unlabeled, or
                // matching its own label) is ours to consume -- one aimed at
                // an outer construct must keep propagating.
                if (ctx.get_break_label().empty() || ctx.get_break_label() == this_switch_label) {
                    ctx.clear_break_continue();
                }
                g_empty_completion = false;
                ctx.set_lexical_environment(old_env);
                return V;
            }

            if (ctx.has_return_value()) {
                ctx.set_lexical_environment(old_env);
                return ctx.get_return_value();
            }

            if (ctx.has_continue()) {
                g_empty_completion = false;
                ctx.set_lexical_environment(old_env);
                return V;
            }
        }
    }

    g_empty_completion = false;
    ctx.set_lexical_environment(old_env);
    return V;
}

std::string SwitchStatement::to_string() const {
    std::string result = "switch (" + discriminant_->to_string() + ") {\n";

    for (const auto& case_node : cases_) {
        result += "  " + case_node->to_string() + "\n";
    }

    result += "}";
    return result;
}

std::unique_ptr<ASTNode> SwitchStatement::clone() const {
    std::vector<std::unique_ptr<ASTNode>> cloned_cases;
    for (const auto& case_node : cases_) {
        cloned_cases.push_back(case_node->clone());
    }

    return std::make_unique<SwitchStatement>(
        discriminant_->clone(),
        std::move(cloned_cases),
        start_, end_
    );
}

Value CaseClause::evaluate(Context& ctx) {
    Value result;
    for (const auto& stmt : consequent_) {
        result = stmt->evaluate(ctx);
        if (ctx.has_exception()) return Value();
    }
    return result;
}

std::string CaseClause::to_string() const {
    std::string result;

    if (is_default()) {
        result = "default:";
    } else {
        result = "case " + test_->to_string() + ":";
    }

    for (const auto& stmt : consequent_) {
        result += " " + stmt->to_string() + ";";
    }

    return result;
}

std::unique_ptr<ASTNode> CaseClause::clone() const {
    auto cloned_test = test_ ? test_->clone() : nullptr;

    std::vector<std::unique_ptr<ASTNode>> cloned_consequent;
    for (const auto& stmt : consequent_) {
        cloned_consequent.push_back(stmt->clone());
    }

    return std::make_unique<CaseClause>(
        std::move(cloned_test),
        std::move(cloned_consequent),
        start_, end_
    );
}

} // namespace Quanta
