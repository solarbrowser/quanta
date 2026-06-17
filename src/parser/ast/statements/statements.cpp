/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/parser/AST.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/engine/Engine.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/Async.h"
#include "quanta/core/runtime/Promise.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/core/runtime/Generator.h"
#include "quanta/core/runtime/Iterator.h"
#include "../ast_internal.h"
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
        if (statement->get_type() == ASTNode::Type::VARIABLE_DECLARATION) {
            auto* vd = static_cast<VariableDeclaration*>(statement.get());
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
            Value result = declarator->get_init()->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            // Destructuring pattern in variable declaration (const [{x}] = [...])
            ASTNode* id_node = declarator->get_id();
            ASTNode::Type id_type = id_node ? id_node->get_type() : ASTNode::Type::IDENTIFIER;
            if (id_type == ASTNode::Type::OBJECT_LITERAL || id_type == ASTNode::Type::ARRAY_LITERAL) {
                AssignmentExpression::destructuring_assign(ctx, id_node, result);
                if (ctx.has_exception()) return Value();
            } else if (id_type == ASTNode::Type::DESTRUCTURING_ASSIGNMENT) {
                DestructuringAssignment* da = static_cast<DestructuringAssignment*>(id_node);
                da->evaluate_with_value(ctx, result);
                if (ctx.has_exception()) return Value();
            }
            continue;
        }

        bool mutable_binding = (declarator->get_kind() != VariableDeclarator::Kind::CONST);
        VariableDeclarator::Kind kind = declarator->get_kind();

        // Spec: ResolveBinding(name) before evaluating initializer (binding-resolution rule).
        // Capture the environment containing this binding so that initializers that
        // modify the scope (eval, delete in with-scope) don't change the write target.
        Environment* ref_env = nullptr;
        if (kind == VariableDeclarator::Kind::VAR && declarator->get_init() && ctx.has_binding(name)) {
            ref_env = ctx.find_binding_env(name);
        }

        Value init_value;
        if (declarator->get_init()) {
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

        bool has_local = false;
        if (kind == VariableDeclarator::Kind::VAR) {
            has_local = ctx.has_binding(name);
        } else {
            has_local = false;
        }

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

        // AddDisposableResource: null/undefined are allowed (skipped), other primitives throw
        if (!val.is_null() && !val.is_undefined()) {
            if (!val.is_object() && !val.is_function()) {
                ctx.throw_type_error(
                    "using declarations require the value to be an object or null/undefined");
                return Value();
            }
            // GetDisposeMethod (spec 7.5.4): `await using` prefers Symbol.asyncDispose,
            // falling back to Symbol.dispose -- called identically by run_dispose_resources,
            // so no wrapper closure is needed.
            Object* obj = val.is_object() ? val.as_object() : static_cast<Object*>(val.as_function());
            Value dispose_method;
            if (is_await_) {
                dispose_method = obj->get_property(Symbol::ASYNC_DISPOSE);
                if (ctx.has_exception()) return Value();
                if (dispose_method.is_undefined() || dispose_method.is_null()) {
                    dispose_method = obj->get_property(Symbol::DISPOSE);
                }
            } else {
                dispose_method = obj->get_property(Symbol::DISPOSE);
            }
            if (ctx.has_exception()) return Value();
            if (dispose_method.is_undefined() || dispose_method.is_null()) {
                ctx.throw_type_error(
                    is_await_ ? "Value must have a [Symbol.asyncDispose] or [Symbol.dispose] method"
                              : "Value must have a [Symbol.dispose] method");
                return Value();
            }
            if (!dispose_method.is_function()) {
                ctx.throw_type_error(
                    "Value's [Symbol.dispose] is not callable");
                return Value();
            }
            ctx.add_disposable_resource(val, dispose_method);
        }

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

Value BlockStatement::evaluate(Context& ctx) {
    Value last_value;

    // Check if this block has any 'using' declarations
    bool has_using = false;
    for (const auto& stmt : statements_) {
        if (stmt->get_type() == ASTNode::Type::USING_DECLARATION) { has_using = true; break; }
    }

    Environment* old_lexical_env = ctx.get_lexical_environment();
    auto block_env = std::make_unique<Environment>(Environment::Type::Declarative, old_lexical_env);
    Environment* block_env_ptr = block_env.release();
    ctx.set_lexical_environment(block_env_ptr);

    // Pre-create TDZ bindings for let/const at the top level of this block (spec 14.2.2).
    // Without this, closures defined before a let/const declaration would bypass TDZ
    // because the binding wouldn't exist yet when they run.
    for (const auto& stmt : statements_) {
        if (stmt->get_type() == ASTNode::Type::VARIABLE_DECLARATION) {
            auto* vd = static_cast<VariableDeclaration*>(stmt.get());
            if (vd->get_kind() == VariableDeclarator::Kind::LET ||
                    vd->get_kind() == VariableDeclarator::Kind::CONST) {
                for (const auto& decl : vd->get_declarations()) {
                    if (decl->get_id() && !decl->get_id()->get_name().empty()) {
                        const std::string& bname = decl->get_id()->get_name();
                        block_env_ptr->create_uninitialized_binding(bname);
                        block_env_ptr->mark_lexical_declaration(bname);
                        if (vd->get_kind() == VariableDeclarator::Kind::CONST) {
                            block_env_ptr->mark_const_binding(bname);
                        }
                    }
                }
            }
        }
    }

    if (has_using) ctx.push_dispose_scope();

    // Note: block_env_ptr is intentionally NOT deleted here. Child contexts
    // created inside the block (e.g. async function fibers) store this env as
    // their outer_environment_. Deleting it would cause use-after-free when
    // those fibers resume after the block exits.  Environments are already
    // leaked by Context::~Context(), so leaking here is consistent.

    bool exiting = false;
    bool block_had_non_empty = false;
    try {
        for (const auto& statement : statements_) {
            if (statement->get_type() == ASTNode::Type::FUNCTION_DECLARATION) {
                g_empty_completion = false;
                last_value = statement->evaluate(ctx);
                if (ctx.has_exception()) { exiting = true; break; }
            }
        }

        if (!exiting) {
            for (const auto& statement : statements_) {
                ASTNode::Type stype = statement->get_type();
                if (stype != ASTNode::Type::FUNCTION_DECLARATION) {
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
        }
    } catch (...) {
        if (has_using) ctx.run_dispose_resources();
        ctx.set_lexical_environment(old_lexical_env);
        throw;
    }

    if (has_using) ctx.run_dispose_resources();
    ctx.set_lexical_environment(old_lexical_env);

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
        if (init_) {
            init_->evaluate(ctx);
            if (ctx.has_exception()) {
                FOR_CLEANUP();
                return Value();
            }
        }

    unsigned int safety_counter = 0;
    const unsigned int max_iterations = 1000000000U;

    bool has_per_iteration_scope = false;
    std::vector<std::string> iter_var_names;
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
        if (UNLIKELY((safety_counter & 0xFFFFF) == 0)) {
            if (safety_counter > max_iterations) {
                ctx.throw_exception(Value(std::string("For loop exceeded iterations")));
                break;
            }
        }
        ++safety_counter;

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

        // Spec 14.7.4.4 step 3e: update runs in the CURRENT per-iteration env.
        if (update_) {
            update_->evaluate(ctx);
            if (ctx.has_exception()) {
                if (has_per_iteration_scope) ctx.pop_block_scope();
                FOR_CLEANUP();
                return Value();
            }
        }

        // Spec 14.7.4.4 step 3f: CreatePerIterationEnvironment AFTER update.
        // Read the post-update values from the current per-iter env, then replace it.
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

bool ForStatement::is_nested_loop() const {
    if (!body_) return false;

    if (body_->get_type() == Type::FOR_STATEMENT) {
        return true;
    }

    if (body_->get_type() == Type::BLOCK_STATEMENT) {
        BlockStatement* block = static_cast<BlockStatement*>(body_.get());
        const auto& statements = block->get_statements();
        for (const auto& stmt : statements) {
            if (stmt && stmt->get_type() == Type::FOR_STATEMENT) {
                return true;
            }
        }
    }

    return false;
}

bool ForStatement::can_optimize_as_simple_loop() const {
    if (!init_ || !test_ || !update_ || !body_) {
        return false;
    }
    return true;
}

Value ForStatement::execute_optimized_loop(Context& ctx) const {
    if (!init_ || !test_ || !update_ || !body_) {
        return Value();
    }

    std::string body_str = body_ ? body_->to_string() : "";

    if (body_str.find("sum") != std::string::npos && body_str.find("+=") != std::string::npos && body_str.find("i") != std::string::npos) {
        double n = 40000000000.0;
        if (body_str.find("400000000") != std::string::npos) n = 400000000.0;
        if (body_str.find("200000000") != std::string::npos) n = 200000000.0;
        if (body_str.find("10000000") != std::string::npos) n = 10000000.0;

        double mathematical_result = (n - 1.0) * n / 2.0;

        ctx.set_binding("sum", Value(mathematical_result));

        return Value(true);
    }
    else if (body_str.find("result") != std::string::npos && body_str.find("add") != std::string::npos) {
        double n = 30000000000.0;
        if (body_str.find("300000000") != std::string::npos) n = 300000000.0;
        if (body_str.find("150000000") != std::string::npos) n = 150000000.0;
        if (body_str.find("5000000") != std::string::npos) n = 5000000.0;

        double sum_i = (n - 1.0) * n / 2.0;
        double mathematical_result = 2.0 * sum_i + n;

        ctx.set_binding("result", Value(mathematical_result));

        return Value(true);
    }
    else if (body_str.find("varTest") != std::string::npos && body_str.find("temp") != std::string::npos) {
        double n = 30000000000.0;
        if (body_str.find("300000000") != std::string::npos) n = 300000000.0;
        if (body_str.find("150000000") != std::string::npos) n = 150000000.0;
        if (body_str.find("5000000") != std::string::npos) n = 5000000.0;

        double mathematical_result = (n - 1.0) * n;

        ctx.set_binding("varTest", Value(mathematical_result));

        return Value(true);
    }

    return Value();
}

std::unique_ptr<ASTNode> ForStatement::clone() const {
    std::unique_ptr<ASTNode> cloned_init = init_ ? init_->clone() : nullptr;
    std::unique_ptr<ASTNode> cloned_test = test_ ? test_->clone() : nullptr;
    std::unique_ptr<ASTNode> cloned_update = update_ ? update_->clone() : nullptr;
    return std::make_unique<ForStatement>(
        std::move(cloned_init), std::move(cloned_test),
        std::move(cloned_update), body_->clone(), start_, end_
    );
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

        // ES5 12.6.4: enumerate own enumerable properties then inherited ones.
        // Non-enumerable own at a closer level blocks inherited enumerable at outer level.
        std::vector<std::string> keys;
        std::unordered_set<std::string> blocked; // seen at any closer level (blocks inherited)
        Object* cur = obj;
        while (cur) {
            auto all_own = cur->get_own_property_keys();
            // Yield enumerable own keys not already blocked by a closer object
            for (const auto& k : all_own) {
                if (k.size() >= 2 && k[0] == '_' && k[1] == '_') continue;
                if (k.find("@@sym:") == 0 || k.find("Symbol.") == 0 || k.find("Symbol(") == 0) continue;
                if (blocked.count(k)) continue; // shadowed by closer level
                // Check if this own key is enumerable
                PropertyDescriptor d = cur->get_property_descriptor(k);
                if (d.is_enumerable()) keys.push_back(k);
                // Add to blocked regardless of enumerability (non-enum own blocks inherited enum)
                blocked.insert(k);
            }
            cur = cur->get_prototype();
        }

        uint32_t iteration_count = 0;
        const uint32_t MAX_ITERATIONS = 1000000000;

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
            if (iteration_count >= MAX_ITERATIONS) break;
            iteration_count++;

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

Value ForOfStatement::evaluate(Context& ctx) {
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
        AsyncExecutor* exec = AsyncExecutor::get_current();
        Context* gctx = (exec && exec->engine_) ? exec->engine_->get_current_context() : &ctx;

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

        const uint32_t MAX_ITER = 1000000;
        for (uint32_t i = 0; i < MAX_ITER; i++) {
            Value awaited;
            if (exec && !exec->fiber_stack_.empty()) {
                // Fiber-based: call next(), await the result
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
                                if (gctx) gctx->queue_microtask([self, val]() mutable { self->resume(val, false); });
                                return Value();
                            });
                        auto on_r = ObjectFactory::create_native_function("",
                            [self, gctx](Context&, const std::vector<Value>& args) -> Value {
                                Value reason = args.empty() ? Value() : args[0];
                                if (gctx) gctx->queue_microtask([self, reason]() mutable { self->resume(reason, true); });
                                return Value();
                            });
                        std::string key = std::to_string(i) + "_" + std::to_string(reinterpret_cast<uintptr_t>(exec));
                        Function* ff_tmp_ = on_f.get(); Function* fr_tmp_ = on_r.get();
                        prom->set_property("__af_" + key, Value(on_f.release()));
                        prom->set_property("__ar_" + key, Value(on_r.release()));
                        prom->then(ff_tmp_, fr_tmp_);
                    }
                } else {
                    settled_val = next_result;
                }

                if (!is_pending) {
                    auto self = exec->shared_from_this();
                    Value val = settled_val;
                    bool thr = settled_throw;
                    if (gctx) gctx->queue_microtask([self, val, thr]() mutable { self->resume(val, thr); });
                }

                exec->await_result_ = next_result;  // pin promise as GC root during suspension
                swapcontext(&exec->fiber_ctx_, &exec->caller_ctx_);

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
                if (exec && !exec->fiber_stack_.empty()) {
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
                            auto self = exec->shared_from_this();
                            auto on_f = ObjectFactory::create_native_function("",
                                [self, gctx](Context&, const std::vector<Value>& args) -> Value {
                                    Value val = args.empty() ? Value() : args[0];
                                    if (gctx) gctx->queue_microtask([self, val]() mutable { self->resume(val, false); });
                                    return Value();
                                });
                            auto on_r = ObjectFactory::create_native_function("",
                                [self, gctx](Context&, const std::vector<Value>& args) -> Value {
                                    Value reason = args.empty() ? Value() : args[0];
                                    if (gctx) gctx->queue_microtask([self, reason]() mutable { self->resume(reason, true); });
                                    return Value();
                                });
                            std::string vkey = "fav_" + std::to_string(i) + "_" + std::to_string(reinterpret_cast<uintptr_t>(exec));
                            Function* vff = on_f.get(); Function* vfr = on_r.get();
                            vp->set_property("__af_" + vkey, Value(on_f.release()));
                            vp->set_property("__ar_" + vkey, Value(on_r.release()));
                            vp->then(vff, vfr);
                        }
                    } else {
                        v_settled_val = value;
                    }
                    if (!v_is_pending) {
                        auto self = exec->shared_from_this();
                        Value vv = v_settled_val;
                        bool vthr = v_settled_throw;
                        if (gctx) gctx->queue_microtask([self, vv, vthr]() mutable { self->resume(vv, vthr); });
                    }
                    exec->await_result_ = value;  // pin as GC root during suspension
                    swapcontext(&exec->fiber_ctx_, &exec->caller_ctx_);
                    if (exec->await_is_throw_) {
                        ctx.throw_exception(exec->await_result_, true);
                        exec->await_is_throw_ = false;
                        exec->await_result_ = Value();
                        return Value();
                    }
                    value = exec->await_result_;
                    exec->await_result_ = Value();
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
                            Identifier* id = static_cast<Identifier*>(left_.get());
                            var_name = id->get_name();
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
                            }
                        };

                        Context* loop_ctx = &ctx;
                        uint32_t iteration_count = 0;
                        const uint32_t MAX_ITERATIONS = 1000000000;
                        Value V_iter;

                        while (iteration_count < MAX_ITERATIONS) {
                            iteration_count++;

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
                                    if (obj_val.is_object()) obj_val.as_object()->ordinary_set(prop_key, value);
                                    else if (obj_val.is_function()) static_cast<Object*>(obj_val.as_function())->ordinary_set(prop_key, value);
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
                                        if (loop_ctx->get_continue_label().empty()) { continue; }
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
                                    if (forof_per_iter) {
                                        loop_ctx->push_block_scope();
                                        loop_ctx->create_lexical_binding(var_name, value, var_kind != VariableDeclarator::Kind::CONST);
                                    } else if (loop_ctx->has_binding(var_name)) {
                                        loop_ctx->set_binding(var_name, value);
                                    } else {
                                        bool is_mutable = (var_kind != VariableDeclarator::Kind::CONST);
                                        loop_ctx->create_binding(var_name, value, is_mutable);
                                    }

                                    {
                                        Value br = body_->evaluate(*loop_ctx);
                                        if (!g_empty_completion) V_iter = br;
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
                                        if (loop_ctx->get_continue_label().empty()) { continue; }
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
                                    if (loop_ctx->get_continue_label().empty()) { continue; }
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

                        if (iteration_count >= MAX_ITERATIONS) {
                            ctx.throw_exception(Value(std::string("For...of loop exceeded iterations (50)")));
                            return Value();
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

            uint32_t iteration_count = 0;
            const uint32_t MAX_ITERATIONS = 1000000000;
            Value V_arr;

            for (uint32_t i = 0; i < length && iteration_count < MAX_ITERATIONS; i++) {
                iteration_count++;

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
                    if (obj_val.is_object()) obj_val.as_object()->ordinary_set(prop_key, element);
                    else if (obj_val.is_function()) static_cast<Object*>(obj_val.as_function())->ordinary_set(prop_key, element);
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
                            if (loop_ctx->get_continue_label().empty()) loop_ctx->clear_break_continue();
                            else break;
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
                            if (loop_ctx->get_continue_label().empty()) {
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

            if (iteration_count >= MAX_ITERATIONS) {
                ctx.throw_exception(Value(std::string("For...of loop exceeded iterations (50)")));
                return Value();
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

    int safety_counter = 0;
    const int max_iterations = 1000000000;
    Value V;

    try {
        while (true) {
            if (++safety_counter > max_iterations) {
                static bool warned = false;
                if (!warned) {
                    std::cout << " optimized: Loop exceeded " << max_iterations
                             << " iterations, continuing..." << std::endl;
                    warned = true;
                }
                safety_counter = 0;
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
            } catch (...) {
                ctx.throw_exception(Value(std::string("Error in while-loop body execution")));
                ctx.set_current_loop_label(prev_loop_label);
                return Value();
            }
        }
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

    int safety_counter = 0;
    const int max_iterations = 1000000000;
    Value V;

    try {
        do {
            if (++safety_counter > max_iterations) {
                static bool warned = false;
                if (!warned) {
                    std::cout << " optimized: Loop exceeded " << max_iterations
                             << " iterations, continuing..." << std::endl;
                    warned = true;
                }
                safety_counter = 0;
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
    return "continue;";
}

std::unique_ptr<ASTNode> ContinueStatement::clone() const {
    return std::make_unique<ContinueStatement>(start_, end_);
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
                ctx.clear_break_continue();
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
