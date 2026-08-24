/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/gc/FiberRegistry.h"
#include "quanta/core/engine/builtins/ObjectBuiltin.h"
#include <span>
#include "quanta/parser/AST.h"
#include "quanta/core/gc/Collector.h"
#include "quanta/core/vm/Interpreter.h"
#include "quanta/core/vm/BytecodeCompiler.h"
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
#include <optional>
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

// Installs a lexical environment for the length of a statement and hands it
// back on every exit, including an exception. The handback matches
// Context::pop_block_scope: an escaped environment is a survivor the collector
// still has to walk, an unescaped one is dead the moment its statement is done
// with it. Statements that build an environment by hand have to use this --
// restoring the previous pointer is not enough, the replacement leaks.
namespace {

class ScopedLexicalEnv {
public:
    ScopedLexicalEnv(Context& ctx, Environment* fresh)
        : ctx_(ctx), saved_(ctx.get_lexical_environment()), fresh_(fresh) {
        if (fresh_) ctx_.set_lexical_environment(fresh_);
    }

    ~ScopedLexicalEnv() { release(); }

    ScopedLexicalEnv(const ScopedLexicalEnv&) = delete;
    ScopedLexicalEnv& operator=(const ScopedLexicalEnv&) = delete;

    Environment* get() const { return fresh_; }

    void release() {
        if (!fresh_) return;
        ctx_.set_lexical_environment(saved_);
        if (!fresh_->is_escaped()) Collector::release_env(fresh_);
        else if (Engine* eng = ctx_.get_engine()) eng->add_survivor_environment(fresh_);
        fresh_ = nullptr;
    }

private:
    Context& ctx_;
    Environment* saved_;
    Environment* fresh_;
};

}  // namespace

static bool is_anon_func_def(const ASTNode* node) {
    if (!node) return false;
    auto t = node->get_type();
    return t == ASTNode::Type::FUNCTION_EXPRESSION ||
           t == ASTNode::Type::ARROW_FUNCTION_EXPRESSION ||
           t == ASTNode::Type::ASYNC_FUNCTION_EXPRESSION ||
           t == ASTNode::Type::CLASS_DECLARATION;
}



std::string ExpressionStatement::to_string() const {
    return expression_->to_string() + ";";
}

std::unique_ptr<ASTNode> ExpressionStatement::clone() const {
    return std::make_unique<ExpressionStatement>(expression_->clone(), start_, end_);
}




std::string EmptyStatement::to_string() const {
    return ";";
}

std::unique_ptr<ASTNode> EmptyStatement::clone() const {
    return std::make_unique<EmptyStatement>(start_, end_);
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


void hoist_lexical_declarations(Environment* env,
                                const std::vector<std::unique_ptr<ASTNode>>& statements);
ClosureTemplate closure_template_for(const ASTNode* literal);
Value declare_function(Context& ctx, const ClosureTemplate& tpl);

Value Program::evaluate(Context& ctx) {
    Object::current_context_ = &ctx;

    Value last_value;

    if (is_strict_) {
        ctx.set_strict_mode(true);
    }
    check_use_strict_directive(ctx);

    hoist_var_declarations(ctx);
    // A script gets a lexical environment of its own for let/const; an eval
    // already runs in one the caller made, so its bindings are created there
    // rather than behind a second scope. Either way they exist before the
    // first statement runs, which is what makes their TDZ observable and
    // their const-ness enforceable.
    if (ctx.get_type() != Context::Type::Eval) {
        hoist_lexical_declarations(ctx);
    } else if (Environment* eval_env = ctx.get_lexical_environment()) {
        Quanta::hoist_lexical_declarations(eval_env, statements_);
    }

    // Hoist function declarations AFTER pushing the script-level lexical env so
    // that function closures can access let/const bindings from the same script.
    for (const auto& statement : statements_) {
        if (statement->get_type() == ASTNode::Type::IMPORT_STATEMENT) continue;
        // `export function f(){}` hoists exactly as a bare one does; the
        // export record itself is kept later, where the statement runs.
        const ASTNode* fn = statement.get();
        if (fn->get_type() == ASTNode::Type::EXPORT_STATEMENT) {
            const auto* ex = static_cast<const ExportStatement*>(fn);
            fn = ex->is_declaration_export() ? ex->get_declaration() : nullptr;
        }
        if (fn && fn->get_type() == ASTNode::Type::FUNCTION_DECLARATION) {
            declare_function(ctx, closure_template_for(fn));
            if (ctx.has_exception()) {
                return Value();
            }
            continue;
        }
        // `export default function () {}` hoists too: the binding holds the
        // function before any statement runs, so a module that imports its own
        // default -- or reaches it round a cycle -- sees the function.
        if (statement->get_type() == ASTNode::Type::EXPORT_STATEMENT) {
            auto* ex = static_cast<ExportStatement*>(statement.get());
            ex->hoist_default(ctx);
            if (ctx.has_exception()) return Value();
        }
    }

    // Every import is instantiated before any statement runs, and after the
    // function declarations are in place: a module that imports a name it
    // exports itself resolves it from its own scope, which is where a
    // hoisted function already is.
    for (const auto& statement : statements_) {
        if (statement->get_type() != ASTNode::Type::IMPORT_STATEMENT) continue;
        statement->evaluate(ctx);
        if (ctx.has_exception()) return Value();
    }

    // Script tier: with hoisting done, the statement loop itself runs as
    // bytecode. An eval asks for the completion value the spec makes it
    // answer with; a script's own result is not observable.
    // A module body with a top-level await runs on a fiber of its own, so the
    // await suspends instead of draining the microtask queue where it stands.
    // The promise it settles is the module's completion.
    // Not from inside another fiber: a module reached that way has an importer
    // already running on the fiber below, and nothing yet knows how to hand
    // that importer a completion to wait on. Those still run where they stand.
    if (may_suspend_ && !AsyncExecutor::get_current()) {
        bool outer_with = false;
        for (Environment* e = ctx.get_lexical_environment(); e; e = e->get_outer()) {
            if (e->is_with_environment()) { outer_with = true; break; }
        }
        auto module_body = BytecodeCompiler::compile_module_body(statements_, outer_with);
        if (module_body) {
            ctx.mark_exposed_to_escape();
            auto promise_obj = ObjectFactory::create_promise(&ctx);
            Promise* promise_raw = static_cast<Promise*>(promise_obj.get());
            completion_promise_ = Value(promise_obj.release());
            auto executor = std::make_shared<AsyncExecutor>(
                std::move(module_body), &ctx, promise_raw, ctx.get_engine());
            executor->run();
            return Value();
        }
    }

    {
        const bool is_eval = ctx.get_type() == Context::Type::Eval;
        bool used_vm = false;
        Value vm_result = VM::run_script(statements_, ctx, used_vm, is_eval);
        if (used_vm) {
            if (ctx.has_exception()) return Value();
            return is_eval ? vm_result : last_value;
        }
    }

    // The statements are compiled or they do not run: there is no second
    // engine to hand them to.
    ctx.throw_type_error("Internal: script could not be compiled");
    return Value();
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

    // `export var x = 1` declares x here; the export wrapper is only the
    // record kept about it, exactly as the lexical hoist above treats it.
    if (node->get_type() == ASTNode::Type::EXPORT_STATEMENT) {
        auto* ex = static_cast<ExportStatement*>(node);
        if (ex->is_declaration_export() && ex->get_declaration()) {
            scan_for_var_declarations(const_cast<ASTNode*>(ex->get_declaration()), ctx);
        }
        return;
    }

    if (node->get_type() == ASTNode::Type::VARIABLE_DECLARATION) {
        VariableDeclaration* var_decl = static_cast<VariableDeclaration*>(node);

        for (const auto& declarator : var_decl->get_declarations()) {
            if (declarator->get_kind() == VariableDeclarator::Kind::VAR) {
                // `var {a, b} = o` has no name of its own on the declarator:
                // the names are the pattern's leaves, and each needs the same
                // hoisted binding a plain `var` gets.
                if (declarator->get_init() &&
                    declarator->get_init()->get_type() == ASTNode::Type::DESTRUCTURING_ASSIGNMENT) {
                    std::vector<std::string> bound;
                    static_cast<const DestructuringAssignment*>(declarator->get_init())
                        ->collect_bound_names(bound);
                    Environment* pattern_env = ctx.get_variable_environment();
                    for (const auto& bn : bound) {
                        if (bn.empty()) continue;
                        const bool have = ctx.get_type() == Context::Type::Eval && pattern_env
                                              ? pattern_env->has_own_binding(bn)
                                              : ctx.has_binding(bn);
                        if (!have) ctx.create_var_binding(bn, Value(), true);
                    }
                    continue;
                }
                const std::string& name = declarator->get_id()->get_name();

                // EvalDeclarationInstantiation asks whether varEnv itself
                // has the name, not whether anything up the chain does: a
                // direct eval's `var` belongs to the calling function even
                // when an outer scope already binds the same name. A chain
                // walk skipped the creation and the write then travelled out
                // to that outer binding.
                Environment* var_env = ctx.get_variable_environment();
                const bool exists = ctx.get_type() == Context::Type::Eval && var_env
                                        ? var_env->has_own_binding(name)
                                        : ctx.has_binding(name);
                if (!exists) {
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
// GetMethod(@@dispose) and the scope registration a `using` performs, shared
// with the compiled Op::RegisterDisposable.
bool register_disposable_resource(Context& ctx, const Value& val, bool is_await) {
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
    if (has_use_strict_directive()) ctx.set_strict_mode(true);
}

bool BlockStatement::has_use_strict_directive() const {
    if (use_strict_cached_ >= 0) return use_strict_cached_ != 0;
    // Scan the directive prologue -- all consecutive string-literal statements
    // at the top of the function body, not just the first one.
    bool found = false;
    for (const auto& stmt : statements_) {
        if (stmt->get_type() != ASTNode::Type::EXPRESSION_STATEMENT) break;
        auto* expr_stmt = static_cast<ExpressionStatement*>(stmt.get());
        auto* expr = expr_stmt->get_expression();
        if (!expr || expr->get_type() != ASTNode::Type::STRING_LITERAL) break;
        auto* sl = static_cast<StringLiteral*>(expr);
        // Per spec: directive is only valid when it has no escape sequences.
        if (sl->get_value() == "use strict" && !sl->has_escapes()) {
            found = true;
            break;
        }
    }
    use_strict_cached_ = found ? 1 : 0;
    return found;
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


// Pre-creates the dead-zone bindings for the let/const declared directly in a
// scope, before any of its statements run (spec 14.2.2). Without this a
// closure created earlier in the scope would read straight past the dead zone,
// and an assignment would create some other binding entirely. Shared because a
// switch's case block is one scope spread across its clauses and needs exactly
// the same treatment as an ordinary block.
void hoist_lexical_declarations(Environment* env,
                                const std::vector<std::unique_ptr<ASTNode>>& statements) {
    for (const auto& stmt : statements) {
        if (!stmt || stmt->get_type() != ASTNode::Type::VARIABLE_DECLARATION) continue;
        auto* vd = static_cast<VariableDeclaration*>(stmt.get());
        if (vd->get_kind() != VariableDeclarator::Kind::LET &&
            vd->get_kind() != VariableDeclarator::Kind::CONST) continue;
        bool is_const_decl = vd->get_kind() == VariableDeclarator::Kind::CONST;
        for (const auto& decl : vd->get_declarations()) {
            if (!decl->get_id() || decl->get_id()->get_name().empty()) continue;
            const std::string& bname = decl->get_id()->get_name();
            // Immutability must be set here: initialize_binding fills the value
            // without touching the mutable flag.
            env->create_uninitialized_binding(bname, !is_const_decl);
            env->mark_lexical_declaration(bname);
            if (is_const_decl) env->mark_const_binding(bname);
        }
    }
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
    // A dense array whose iteration nothing has redefined is walked by index,
    // which is what %ArrayIteratorPrototype%.next does anyway -- it reads
    // a[i] and hands back {value, done}. Skipping it skips the iterator
    // object, a result object per step, and the call that makes them. The
    // protector is what says nothing has redefined it; the pair is marked by
    // handing back a NUMBER where a next function would be, which the step
    // below tests for.
    if (iterable.is_object() && Object::array_iterator_protector_intact()) {
        Object* a = iterable.as_object();
        if (a->get_type() == Object::ObjectType::Array && a->has_only_dense_elements()) {
            out_iterator = iterable;
            out_next_fn = Value(0.0);
            return true;
        }
    }
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
                [str_value](Context&, std::span<const Value>, Value receiver) -> Value {
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
    // GetIterator only READS `next` (spec 7.4.2 step 4); whether it can be
    // called is discovered when it is called, which is what lets everything
    // between the two -- a suspension, an IteratorClose -- happen first.
    Value next_method = iterator_val.as_object()->get_property("next");
    if (ctx.has_exception()) return false;
    // The index form above marks itself by handing back a number, so one the
    // iterator itself supplied has to be flattened first: the step only needs
    // to know it cannot be called, not what it was.
    if (!next_method.is_function()) next_method = Value();
    out_iterator = iterator_val;
    out_next_fn = next_method;
    return true;
}

// Mirrors the tree-walker's per-iteration `next_fn->call` + done/value
// extraction (including the Object::current_context_ getter-exception
// rescue for a `done`/`value` accessor that throws through a different
// Context, e.g. a Proxy trap) below in the sync for-of loop.
bool ForOfStatement::iterator_step(Context& ctx, const Value& iterator, Value& next_fn,
                                    bool& out_done, Value& out_value) {
    // Index form, set up by get_iterator above. Re-checked every step rather
    // than trusted from the start: the body can redefine the iteration or make
    // the array sparse, and either has to stop this from answering.
    if (next_fn.is_number()) {
        Object* a = iterator.as_object();
        const double di = next_fn.as_number();
        const uint32_t i = static_cast<uint32_t>(di);
        // Nothing is re-checked against the protector here: GetIterator reads
        // `next` once and the loop keeps that one, so redefining array
        // iteration after the loop started cannot reach it -- exactly as it
        // cannot reach an iterator object that was already made.
        if (LIKELY(a->has_only_dense_elements())) {
            if (i >= a->element_count()) { out_done = true; return true; }
            out_value = a->get_element(i);
        } else {
            // Made sparse while the loop was running. The iterator this stands
            // in for reads a[i] against the current length and yields
            // undefined for a hole, so this does the same rather than refusing.
            const double len = a->get_property("length").to_number();
            if (!(di < len)) { out_done = true; return true; }
            out_value = a->get_property(std::to_string(i));
        }
        out_done = false;
        next_fn = Value(di + 1.0);
        return true;
    }
    if (!next_fn.is_function()) {
        ctx.throw_type_error("next method is not a function");
        return false;
    }
    Value result = next_fn.as_function()->call(ctx, {}, iterator);
    // Per spec: if next() throws abruptly, do NOT close the iterator.
    if (ctx.has_exception()) return false;
    if (!result.is_object()) {
        ctx.throw_type_error("Iterator result is not an object");
        return false;
    }
    Object* result_obj = result.as_object();
    // Neither read sits at a bytecode site, so no inline cache ever sees them:
    // every step of every for-of pays the general lookup twice. A plain
    // {value, done} result answers both straight from its own shape slots.
    // Whatever the shortcut cannot serve -- an accessor, an inherited done, a
    // proxy, an absent key -- falls through to the general path unchanged,
    // which is also why a slot hit needs none of the exception plumbing below.
    static const std::string kDone = "done";
    static const std::string kValue = "value";

    Value done;
    if (!result_obj->try_read_own_data_slot(kDone, done)) {
        done = result_obj->get_property(kDone);
        if (!ctx.has_exception() && Object::current_context_ && Object::current_context_ != &ctx
                && Object::current_context_->has_exception()) {
            ctx.throw_exception(Object::current_context_->get_exception(), true);
            Object::current_context_->clear_exception();
        }
        if (ctx.has_exception()) return false;
    }
    if (done.to_boolean()) {
        out_done = true;
        return true;
    }
    Value value;
    if (!result_obj->try_read_own_data_slot(kValue, value)) {
        value = result_obj->get_property(kValue);
        if (!ctx.has_exception() && Object::current_context_ && Object::current_context_ != &ctx
                && Object::current_context_->has_exception()) {
            ctx.throw_exception(Object::current_context_->get_exception(), true);
            Object::current_context_->clear_exception();
        }
        if (ctx.has_exception()) return false;
    }
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

// The three steps `for await...of` is made of, shared by
// ForOfStatement::evaluate and the compiled loop so the two cannot drift.
// A getter reached here can throw through Object::current_context_ rather than
// ctx, which would otherwise be lost.
static void rescue_getter_exception(Context& ctx) {
    if (!ctx.has_exception() && Object::current_context_ && Object::current_context_ != &ctx
            && Object::current_context_->has_exception()) {
        ctx.throw_exception(Object::current_context_->get_exception(), true);
        Object::current_context_->clear_exception();
    }
}

// The context a promise made for this Await belongs to: the running fiber's,
// so the job it queues is ordered against everything else on that queue.
static Context* async_promise_context(Context& ctx) {
    AsyncGenerator* async_gen = AsyncGenerator::get_current();
    if (async_gen && async_gen->fiber_->co != nullptr) {
        return async_gen->get_outer_context() ? async_gen->get_outer_context()
                                              : async_gen->get_generator_context();
    }
    AsyncExecutor* exec = AsyncExecutor::get_current();
    if (exec && exec->fiber_->co != nullptr) {
        return exec->engine_ ? exec->engine_->get_current_context() : exec->exec_context_;
    }
    return &ctx;
}

// closeOnRejection: an abrupt continuation closes the sync iterator before the
// rejection propagates, and a failure while closing is dropped in its favour.
static void close_sync_iterator_quietly(Context& ctx, const Value& iterator, bool is_done) {
    if (is_done || !iterator.is_object()) return;
    Value close_fn = iterator.as_object()->get_property("return");
    if (!ctx.has_exception() && close_fn.is_function()) {
        close_fn.as_function()->call(ctx, {}, iterator);
    }
    ctx.clear_exception();
}

// GetIterator(obj, async): @@asyncIterator when the object has one, otherwise
// the sync iterator, whose results then go through
// AsyncFromSyncIteratorContinuation -- which is what `from_sync_out` reports.
Value get_async_iterator(Context& ctx, const Value& iterable, Value& next_fn_out, bool& from_sync_out) {
    from_sync_out = true;
    if (iterable.is_object()) {
        Object* iterable_obj = iterable.as_object();
        if (Symbol* async_sym = Symbol::get_well_known(Symbol::ASYNC_ITERATOR)) {
            Value iter_method = iterable_obj->get_property(async_sym->to_property_key());
            rescue_getter_exception(ctx);
            if (ctx.has_exception()) return Value();
            if (iter_method.is_function()) {
                Value iterator = iter_method.as_function()->call(ctx, {}, iterable);
                rescue_getter_exception(ctx);
                if (ctx.has_exception()) return Value();
                if (!iterator.is_object()) {
                    ctx.throw_type_error("for-await-of: iterator is not an object");
                    return Value();
                }
                next_fn_out = iterator.as_object()->get_property("next");
                rescue_getter_exception(ctx);
                if (ctx.has_exception()) return Value();
                if (!next_fn_out.is_function()) {
                    ctx.throw_type_error("for-await-of: iterator has no next method");
                    return Value();
                }
                from_sync_out = false;
                return iterator;
            }
        }
    }

    // No @@asyncIterator: the sync iterator, obtained exactly as a plain
    // for-of obtains it, so a boxed string and the dense-array index form both
    // work here too.
    Value iterator;
    if (!ForOfStatement::get_iterator(ctx, iterable, iterator, next_fn_out)) return Value();
    return iterator;
}

// AsyncIteratorClose without a pending completion: call return() and Await it.
void async_iterator_close(Context& ctx, const Value& iterator) {
    if (!iterator.is_object()) return;
    Value return_method = iterator.as_object()->get_property("return");
    rescue_getter_exception(ctx);
    if (ctx.has_exception()) return;
    if (return_method.is_null() || return_method.is_undefined()) return;
    if (!return_method.is_function()) {
        ctx.throw_type_error("iterator return method is not callable");
        return;
    }
    Value result = return_method.as_function()->call(ctx, {}, iterator);
    if (ctx.has_exception()) return;
    Value awaited;
    if (await_value(ctx, result, awaited)) { ctx.throw_exception(awaited, true); return; }
    if (!awaited.is_object()) {
        ctx.throw_type_error("iterator return method returned a non-object");
    }
}

// One step. Answers true when the iterator is done, in which case value_out is
// untouched; on an abrupt completion it answers true with ctx holding the
// exception, so a caller that checks ctx sees both the same way. next_fn is
// mutable because the sync dense-array form carries its index there.
bool async_iterator_step(Context& ctx, const Value& iterator, Value& next_fn,
                         bool from_sync, Value& value_out) {
    if (!from_sync) {
        if (!next_fn.is_function()) {
            ctx.throw_type_error("next method is not a function");
            return false;
        }
        Value result = next_fn.as_function()->call(ctx, {}, iterator);
        rescue_getter_exception(ctx);
        if (ctx.has_exception()) return true;
        Value awaited;
        if (await_value(ctx, result, awaited)) { ctx.throw_exception(awaited, true); return true; }
        if (!awaited.is_object()) {
            ctx.throw_type_error("for-await-of: iterator result must be an object");
            return true;
        }
        Value done = awaited.as_object()->get_property("done");
        rescue_getter_exception(ctx);
        if (ctx.has_exception()) return true;
        if (done.to_boolean()) return true;
        value_out = awaited.as_object()->get_property("value");
        rescue_getter_exception(ctx);
        return ctx.has_exception();
    }

    // AsyncFromSyncIteratorContinuation: the sync result's `value` is
    // PromiseResolve'd, and the {value, done} that produces comes back through
    // a promise of the continuation's own, which the loop Awaits in turn. Two
    // PromiseResolve calls, so two `constructor` reads and two ticks, and a
    // rejection travels through the second promise rather than around it.
    bool is_done = false;
    Value raw_value;
    if (!ForOfStatement::iterator_step(ctx, iterator, next_fn, is_done, raw_value)) return true;

    Context* gctx = async_promise_context(ctx);
    auto nr_obj = ObjectFactory::create_promise(gctx);
    Promise* next_result = static_cast<Promise*>(nr_obj.release());

    Promise* value_wrapper = nullptr;
    if (AsyncUtils::is_promise(raw_value)) {
        value_wrapper = static_cast<Promise*>(raw_value.as_object());
        value_wrapper->get_property("constructor");
        rescue_getter_exception(ctx);
        if (ctx.has_exception()) {
            Value err = ctx.get_exception();
            ctx.clear_exception();
            close_sync_iterator_quietly(ctx, iterator, is_done);
            next_result->reject(err);
            value_wrapper = nullptr;
        }
    } else {
        auto vw_obj = ObjectFactory::create_promise(gctx);
        static_cast<Promise*>(vw_obj.get())->fulfill(raw_value);
        value_wrapper = static_cast<Promise*>(vw_obj.release());
    }

    if (value_wrapper) {
        auto unwrap_f = ObjectFactory::create_native_function("",
            [next_result, is_done](Context&, std::span<const Value> args, Value receiver) -> Value {
                Value val = args.empty() ? Value() : args[0];
                auto res_obj = ObjectFactory::create_object();
                res_obj->set_property("value", val);
                res_obj->set_property("done", Value(is_done));
                next_result->fulfill(Value(res_obj.release()));
                return Value();
            });
        auto unwrap_r = ObjectFactory::create_native_function("",
            [next_result, is_done, iterator, gctx](Context&, std::span<const Value> args, Value receiver) -> Value {
                Value reason = args.empty() ? Value() : args[0];
                if (gctx) close_sync_iterator_quietly(*gctx, iterator, is_done);
                next_result->reject(reason);
                return Value();
            });
        static thread_local size_t afsi_ctr = 0;
        const std::string key = "afsi_" + std::to_string(afsi_ctr++);
        Function* uf = unwrap_f.get();
        Function* ur = unwrap_r.get();
        value_wrapper->set_internal_slot("__af_" + key, Value(unwrap_f.release()));
        value_wrapper->set_internal_slot("__ar_" + key, Value(unwrap_r.release()));
        value_wrapper->then(uf, ur);
    }

    Value settled;
    if (await_value(ctx, Value(next_result), settled)) { ctx.throw_exception(settled, true); return true; }
    if (!settled.is_object()) {
        ctx.throw_type_error("for-await-of: iterator result must be an object");
        return true;
    }
    Value settled_done = settled.as_object()->get_property("done");
    rescue_getter_exception(ctx);
    if (ctx.has_exception()) return true;
    if (settled_done.to_boolean()) return true;
    value_out = settled.as_object()->get_property("value");
    rescue_getter_exception(ctx);
    return ctx.has_exception();
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




std::string WhileStatement::to_string() const {
    return "while (" + test_->to_string() + ") " + body_->to_string();
}

std::unique_ptr<ASTNode> WhileStatement::clone() const {
    return std::make_unique<WhileStatement>(
        test_->clone(), body_->clone(), start_, end_
    );
}




std::string DoWhileStatement::to_string() const {
    return "do " + body_->to_string() + " while (" + test_->to_string() + ")";
}

std::unique_ptr<ASTNode> DoWhileStatement::clone() const {
    return std::make_unique<DoWhileStatement>(
        body_->clone(), test_->clone(), start_, end_
    );
}


// Entering a `with`: the strict-mode refusal, ToObject, and the scope push,
// shared with the compiled Op::PushWithEnv so the two cannot drift. Answers
// false when it threw.
bool perform_with_push(Context& ctx, const Value& obj_value) {
    if (ctx.is_strict_mode()) {
        ctx.throw_syntax_error("Strict mode code may not include a with statement");
        return false;
    }
    // ToObject: null and undefined throw, and a primitive is boxed into the
    // wrapper its own prototype gives it -- `with ("s")` sees `length`, which
    // an empty object stood in for before.
    if (obj_value.is_null() || obj_value.is_undefined()) {
        ctx.throw_type_error("Cannot convert undefined or null to object in with statement");
        return false;
    }
    Object* obj = to_object_or_throw(ctx, obj_value);
    if (!obj || ctx.has_exception()) {
        if (!ctx.has_exception()) ctx.throw_type_error("with statement: failed to create object");
        return false;
    }

    ctx.push_with_scope(obj);
    return true;
}



std::string WithStatement::to_string() const {
    return "with (" + object_->to_string() + ") " + body_->to_string();
}

std::unique_ptr<ASTNode> WithStatement::clone() const {
    return std::make_unique<WithStatement>(
        object_->clone(), body_->clone(), start_, end_
    );
}


// What `return` does once its value is in hand, shared by
// ReturnStatement::evaluate and Op::SettleReturn so the two cannot drift.
// Only an async *generator*'s `return <expr>` Awaits: a plain async function's
// return has no Await step, and a bare `return;` has nothing to Await, which
// costs one fewer microtask tick. Recording the value is what tells
// `return undefined` apart from falling off the end.
Value perform_return_completion(Context& ctx, Value return_value, bool has_argument,
                                bool do_record) {
    AsyncGenerator* async_gen = AsyncGenerator::get_current();
    if (has_argument && async_gen && async_gen->get_generator_context() == &ctx) {
        Value awaited;
        bool threw = await_value(ctx, return_value, awaited);
        if (threw) { ctx.throw_exception(awaited, true); return Value(); }
        return_value = awaited;
    }
    // Recording is what a finally can still take back: a `break` inside one
    // cancels the pending return, and a value written here before the finally
    // ran would outlive the completion it belonged to.
    if (do_record) ctx.set_return_value(return_value);
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




std::string BreakStatement::to_string() const {
    return label_.empty() ? "break;" : "break " + label_ + ";";
}

std::unique_ptr<ASTNode> BreakStatement::clone() const {
    return std::make_unique<BreakStatement>(start_, end_, label_);
}




std::string ContinueStatement::to_string() const {
    return label_.empty() ? "continue;" : "continue " + label_ + ";";
}

std::unique_ptr<ASTNode> ContinueStatement::clone() const {
    return std::make_unique<ContinueStatement>(start_, end_, label_);
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



std::string CatchClause::to_string() const {
    return "catch (" + parameter_name_ + ") " + body_->to_string();
}

std::unique_ptr<ASTNode> CatchClause::clone() const {
    auto c = std::make_unique<CatchClause>(parameter_name_, body_->clone(), start_, end_);
    if (destructuring_pattern_) c->set_destructuring_pattern(destructuring_pattern_->clone());
    return c;
}



std::string ThrowStatement::to_string() const {
    return "throw " + expression_->to_string();
}

std::unique_ptr<ASTNode> ThrowStatement::clone() const {
    return std::make_unique<ThrowStatement>(expression_->clone(), start_, end_);
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
