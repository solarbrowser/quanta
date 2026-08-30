/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/gc/FiberRegistry.h"
#include <span>
#include "quanta/parser/AST.h"
#include "quanta/parser/ScriptUnit.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/engine/Engine.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/Async.h"
#include "quanta/core/runtime/Promise.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/core/runtime/Generator.h"
#include "quanta/core/runtime/ProxyReflect.h"
#include "quanta/core/modules/ModuleLoader.h"
#include "quanta/core/engine/CallStack.h"
#include "quanta/core/vm/BytecodeCompiler.h"
#include "quanta/core/vm/Interpreter.h"
#include "quanta/core/gc/Collector.h"
#include <sstream>
#include <set>
#include <unordered_set>
#include <cctype>

namespace Quanta {

// Shared with statements.cpp for "empty completion" tracking (eval completion value).
extern thread_local bool g_empty_completion;

// Spec SetFunctionName prefix: convert a property key to the "get key" / "set key" form.
static std::string accessor_function_name(const std::string& prop_key, const std::string& prefix) {
    if (prop_key.find("@@sym:") == 0 || prop_key.find("Symbol.") == 0) {
        Symbol* sym = Symbol::find_by_property_key(prop_key);
        std::string desc = (sym && sym->get_has_description()) ? sym->get_description() : "";
        return prefix + (desc.empty() ? "" : "[" + desc + "]");
    }
    return prefix + prop_key;
}

// Spec 7.1.1 ToPrimitive + 7.1.19 ToPropertyKey for computed class/object keys.
// Unlike Value::to_property_key(), this throws TypeError when @@toPrimitive is
// non-callable or when OrdinaryToPrimitive finds neither toString nor valueOf.
static std::string computed_key_to_property_key(Context& ctx, const Value& val) {
    return val.to_property_key_strict(ctx);
}

// Detects a literal eval() call anywhere in this function's own body (stopping at nested function/class boundaries); such functions skip closure capture/write-back below, since eval can introduce a same-named var our static analysis can't see.
static bool contains_direct_eval(ASTNode* node) {
    if (!node) return false;
    switch (node->get_type()) {
        case ASTNode::Type::FUNCTION_EXPRESSION:
        case ASTNode::Type::FUNCTION_DECLARATION:
        case ASTNode::Type::ARROW_FUNCTION_EXPRESSION:
        case ASTNode::Type::ASYNC_FUNCTION_EXPRESSION:
        case ASTNode::Type::METHOD_DEFINITION:
        case ASTNode::Type::CLASS_DECLARATION:
        case ASTNode::Type::CLASS_STATIC_BLOCK:
            return false;
        case ASTNode::Type::CALL_EXPRESSION: {
            auto* call = static_cast<CallExpression*>(node);
            if (call->get_callee()->get_type() == ASTNode::Type::IDENTIFIER &&
                static_cast<Identifier*>(call->get_callee())->get_name() == "eval") {
                return true;
            }
            if (contains_direct_eval(call->get_callee())) return true;
            for (const auto& arg : call->get_arguments()) {
                if (contains_direct_eval(arg.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::BINARY_EXPRESSION: {
            auto* b = static_cast<BinaryExpression*>(node);
            return contains_direct_eval(b->get_left()) || contains_direct_eval(b->get_right());
        }
        case ASTNode::Type::UNARY_EXPRESSION:
            return contains_direct_eval(static_cast<UnaryExpression*>(node)->get_operand());
        case ASTNode::Type::ASSIGNMENT_EXPRESSION: {
            auto* a = static_cast<AssignmentExpression*>(node);
            return contains_direct_eval(a->get_left()) || contains_direct_eval(a->get_right());
        }
        case ASTNode::Type::CONDITIONAL_EXPRESSION: {
            auto* c = static_cast<ConditionalExpression*>(node);
            return contains_direct_eval(c->get_test()) || contains_direct_eval(c->get_consequent()) || contains_direct_eval(c->get_alternate());
        }
        case ASTNode::Type::MEMBER_EXPRESSION: {
            auto* m = static_cast<MemberExpression*>(node);
            if (contains_direct_eval(m->get_object())) return true;
            return m->is_computed() && contains_direct_eval(m->get_property());
        }
        case ASTNode::Type::OPTIONAL_CHAINING_EXPRESSION: {
            auto* m = static_cast<OptionalChainingExpression*>(node);
            if (contains_direct_eval(m->get_object())) return true;
            return m->is_computed() && contains_direct_eval(m->get_property());
        }
        case ASTNode::Type::NULLISH_COALESCING_EXPRESSION: {
            auto* n = static_cast<NullishCoalescingExpression*>(node);
            return contains_direct_eval(n->get_left()) || contains_direct_eval(n->get_right());
        }
        case ASTNode::Type::NEW_EXPRESSION: {
            auto* n = static_cast<NewExpression*>(node);
            if (contains_direct_eval(n->get_constructor())) return true;
            for (const auto& arg : n->get_arguments()) {
                if (contains_direct_eval(arg.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::OBJECT_LITERAL: {
            for (const auto& prop : static_cast<ObjectLiteral*>(node)->get_properties()) {
                if (prop->computed && contains_direct_eval(prop->key.get())) return true;
                if (contains_direct_eval(prop->value.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::ARRAY_LITERAL: {
            for (const auto& el : static_cast<ArrayLiteral*>(node)->get_elements()) {
                if (contains_direct_eval(el.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::TEMPLATE_LITERAL: {
            for (const auto& el : static_cast<TemplateLiteral*>(node)->get_elements()) {
                if (el.type == TemplateLiteral::Element::Type::EXPRESSION && contains_direct_eval(el.expression.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::SPREAD_ELEMENT:
            return contains_direct_eval(static_cast<SpreadElement*>(node)->get_argument());
        case ASTNode::Type::AWAIT_EXPRESSION:
            return contains_direct_eval(static_cast<AwaitExpression*>(node)->get_argument());
        case ASTNode::Type::YIELD_EXPRESSION:
            return contains_direct_eval(static_cast<YieldExpression*>(node)->get_argument());
        case ASTNode::Type::EXPRESSION_STATEMENT:
            return contains_direct_eval(static_cast<ExpressionStatement*>(node)->get_expression());
        case ASTNode::Type::VARIABLE_DECLARATION: {
            for (const auto& d : static_cast<VariableDeclaration*>(node)->get_declarations()) {
                if (contains_direct_eval(d->get_init())) return true;
            }
            return false;
        }
        case ASTNode::Type::BLOCK_STATEMENT: {
            for (const auto& s : static_cast<BlockStatement*>(node)->get_statements()) {
                if (contains_direct_eval(s.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::IF_STATEMENT: {
            auto* i = static_cast<IfStatement*>(node);
            return contains_direct_eval(i->get_test()) || contains_direct_eval(i->get_consequent()) ||
                   (i->get_alternate() && contains_direct_eval(i->get_alternate()));
        }
        case ASTNode::Type::FOR_STATEMENT: {
            auto* f = static_cast<ForStatement*>(node);
            return contains_direct_eval(f->get_init()) || contains_direct_eval(f->get_test()) ||
                   contains_direct_eval(f->get_update()) || contains_direct_eval(f->get_body());
        }
        case ASTNode::Type::FOR_IN_STATEMENT: {
            auto* f = static_cast<ForInStatement*>(node);
            return contains_direct_eval(f->get_right()) || contains_direct_eval(f->get_body());
        }
        case ASTNode::Type::FOR_OF_STATEMENT: {
            auto* f = static_cast<ForOfStatement*>(node);
            return contains_direct_eval(f->get_right()) || contains_direct_eval(f->get_body());
        }
        case ASTNode::Type::WHILE_STATEMENT: {
            auto* w = static_cast<WhileStatement*>(node);
            return contains_direct_eval(w->get_test()) || contains_direct_eval(w->get_body());
        }
        case ASTNode::Type::DO_WHILE_STATEMENT: {
            auto* w = static_cast<DoWhileStatement*>(node);
            return contains_direct_eval(w->get_test()) || contains_direct_eval(w->get_body());
        }
        case ASTNode::Type::WITH_STATEMENT:
            return contains_direct_eval(static_cast<WithStatement*>(node)->get_body());
        case ASTNode::Type::RETURN_STATEMENT: {
            auto* r = static_cast<ReturnStatement*>(node);
            return r->has_argument() && contains_direct_eval(r->get_argument());
        }
        case ASTNode::Type::THROW_STATEMENT:
            return contains_direct_eval(static_cast<ThrowStatement*>(node)->get_expression());
        case ASTNode::Type::LABELED_STATEMENT:
            return contains_direct_eval(static_cast<LabeledStatement*>(node)->get_statement());
        case ASTNode::Type::TRY_STATEMENT: {
            auto* t = static_cast<TryStatement*>(node);
            if (contains_direct_eval(t->get_try_block())) return true;
            if (t->get_catch_clause() && contains_direct_eval(t->get_catch_clause())) return true;
            return t->get_finally_block() && contains_direct_eval(t->get_finally_block());
        }
        case ASTNode::Type::CATCH_CLAUSE:
            return contains_direct_eval(static_cast<CatchClause*>(node)->get_body());
        case ASTNode::Type::SWITCH_STATEMENT: {
            auto* s = static_cast<SwitchStatement*>(node);
            if (contains_direct_eval(s->get_discriminant())) return true;
            for (const auto& c : s->get_cases()) {
                if (contains_direct_eval(c.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::CASE_CLAUSE: {
            auto* c = static_cast<CaseClause*>(node);
            if (c->get_test() && contains_direct_eval(c->get_test())) return true;
            for (const auto& s : c->get_consequent()) {
                if (contains_direct_eval(s.get())) return true;
            }
            return false;
        }
        default:
            return false;
    }
}

// Builds (once per decl site) the executable every instance of this literal
// shares: a durable clone of the body and parameters, plus the source text
// toString() reports. Callers pass the node's own cached_executable_ slot.
// Same choice ensure_shared_executable makes, for the method and constructor
// literals that reach their executable by another route: lend the body when the
// literal knows which tree owns it, copy it when it does not. Copying is what
// leaves the copy's own nested literals unowned, so every site that skips this
// spreads copying to everything underneath it.
static void install_literal_body(FunctionExecutable* exe, ASTNode* value_node) {
    if (std::getenv("QUANTA_EXE_COUNT")) {
        static size_t n = 0;
        if ((++n % 500) == 0) std::fprintf(stderr, "[ilb] install_literal_body=%zu\n", n);
    }
    if (value_node && value_node->get_type() == ASTNode::Type::FUNCTION_EXPRESSION) {
        auto* fe = static_cast<FunctionExpression*>(value_node);
        if (ScriptUnit* unit = fe->owning_unit()) {
            exe->borrow_body(ExecutableRef<ScriptUnit>(unit), fe->get_body());
            return;
        }
    }
    exe->adopt_body(value_node && value_node->get_type() == ASTNode::Type::FUNCTION_EXPRESSION
                        ? (static_cast<FunctionExpression*>(value_node)->get_body()
                               ? static_cast<FunctionExpression*>(value_node)->get_body()->clone()
                               : nullptr)
                        : nullptr);
}

static ExecutableRef<FunctionExecutable> ensure_shared_executable(
        const ExecutableRef<FunctionExecutable>& cached,
        const ASTNode* body,
        const std::vector<std::unique_ptr<Parameter>>& params,
        uint32_t src_start,
        uint32_t src_end,
        ScriptUnit* owning_unit = nullptr) {
    if (cached) return cached;
    if (std::getenv("QUANTA_EXE_COUNT")) {
        static size_t n = 0;
        if ((++n % 500) == 0) std::fprintf(stderr, "[ese] ensure_shared_executable=%zu\n", n);
    }
    ExecutableRef<FunctionExecutable> exe = make_executable_ref();
    // A literal that knows which tree it came from lends its body out and the
    // executable keeps that tree alive; one that does not (any entry point not
    // yet handing out units) still takes a copy, which is what kept a borrowed
    // subtree from outliving its parse before units existed.
    if (owning_unit) {
        exe->borrow_body(ExecutableRef<ScriptUnit>(owning_unit),
                         const_cast<ASTNode*>(body));
    } else {
        exe->adopt_body(body ? body->clone() : nullptr);
    }
    for (const auto& param : params) {
        exe->parameter_objects.push_back(
            std::unique_ptr<Parameter>(static_cast<Parameter*>(param->clone().release())));
        exe->parameters.push_back(exe->parameter_objects.back()->get_name()->get_name());
    }
    // Cut from the unit's source here rather than at the call site: this runs
    // once per declaration site, while the call site runs on every closure
    // instantiation and would materialize the same bytes each time.
    if (owning_unit && src_end > src_start) {
        exe->set_source_range(src_start, src_end);
    }
    return exe;
}

// ES6 `length`: parameters before the first rest or defaulted one.
static uint32_t spec_length_of(const FunctionExecutable& exe) {
    uint32_t n = 0;
    for (const auto& p : exe.parameter_objects) {
        if (p->is_rest() || p->has_default()) break;
        n++;
    }
    return n;
}

bool BlockStatement::has_direct_eval_cached() const {
    if (contains_eval_cached_ >= 0) return contains_eval_cached_ != 0;
    bool found = contains_direct_eval(const_cast<BlockStatement*>(this));
    contains_eval_cached_ = found ? 1 : 0;
    return found;
}

// The source-fixed half: reads the literal node once and caches the shared
// executable on it. Cheap enough to call per instantiation (every input is
// itself already cached on the node), so no template needs storing there --
// the compiler calls it once and parks the result in the chunk instead.
//
// needs_outer_env below is not one rule: each form pinned the closure
// environment differently before this was shared, and those differences are
// reproduced rather than unified.
// Reads the two body-derived flags an instantiation needs, computing them
// from the body the first time. Cached on the literal so they survive the
// body being dropped -- see set_body_facts.
template <typename Literal>
static void load_body_facts(const Literal* lit, bool& is_strict, bool& has_eval) {
    if (lit->body_strict_state() < 0) {
        const ASTNode* body = lit->get_body();
        bool strict = false, eval = false;
        if (!body && lit->has_body_token_range()) {
            // A body stepped over rather than read: what it would have said is
            // on the unit, written down the first time it was read.
            if (ScriptUnit* unit = lit->owning_unit()) {
                if (const BodyScopeInfo* info = unit->scope_info_at(lit->body_source_first())) {
                    strict = info->body_strict;
                    // `eval` named anywhere in the body, which is more than a
                    // direct call to it and errs the safe way.
                    eval = info->eval_anywhere;
                }
            }
            lit->set_body_facts(strict, eval);
            is_strict = strict;
            has_eval = eval;
            return;
        }
        if (body && body->get_type() == ASTNode::Type::BLOCK_STATEMENT) {
            const auto* block = static_cast<const BlockStatement*>(body);
            strict = block->has_use_strict_directive();
            eval = block->has_direct_eval_cached();
        } else if (body) {
            eval = contains_direct_eval(const_cast<ASTNode*>(body));
        }
        lit->set_body_facts(strict, eval);
    }
    is_strict = lit->body_strict_state() == 1;
    has_eval = lit->body_direct_eval_state() == 1;
}

// Hands the executable the token range instead of the body, and drops the
// subtree. Runs only after every analysis this literal caches has been taken
// (strict, direct-eval, needs-outer-env, spec length), which is why it sits at
// the end of each branch rather than inside ensure_shared_executable.
//
// Leaf only, and the reason is cost rather than safety: releasing a body that
// holds inner literals works (the rebuild happens at most once, so no node is
// ever orphaned), but it means re-parsing every body inside it on the next
// call. Measured on a bundle, that bought a further 17MB of live tree for
// about 5% of cycles -- the leaves alone give 39MB for no measurable time.
template <typename Literal>
static void defer_leaf_body(const Literal* lit, FunctionExecutable* exe, bool fresh,
                            bool strict, bool is_generator, bool is_async) {
    // Only the instantiation that built the executable may do this. A later one
    // would reach an executable that has since materialized its body and
    // compiled from it, and dropping that tree leaves the chunk's own AST
    // references (ast_nodes) pointing at freed nodes.
    if (!fresh) return;
    if (!exe || !lit->has_body_token_range()) return;
    ScriptUnit* unit = lit->owning_unit();
    if (!unit || !unit->can_reparse_bodies()) return;
    if (!lit->get_body()) {
        // Stepped over while its enclosing body was read back: there is no
        // subtree to let go of, only a range to remember -- and no node to
        // read that range off, so the literal hands it over itself.
        const BodyScopeInfo* info = unit->scope_info_at(lit->body_source_first());
        if (!info || info->body_end <= lit->body_source_first()) return;
        // The line and column are the literal's own: a frame reports where
        // the function was written, and the body opens on the same line as
        // often as not. The offset has to be the body's, since that is what a
        // record of it is keyed on.
        Position body_start = lit->get_start();
        body_start.offset = lit->body_source_first();
        exe->set_body_range(body_start, info->body_end);
        exe->defer_body(ExecutableRef<ScriptUnit>(unit), strict, is_generator, is_async);
        return;
    }
    // Stepping over a body inside one being read back is still being brought
    // up: with it off, only a leaf body is let go of, which is what the engine
    // has always done.
    static const bool lazy_inner = std::getenv("QUANTA_LAZY_INNER") != nullptr;
    if (!lazy_inner && !lit->body_is_leaf()) return;
    // A concise arrow body is an expression: there is no `{` to read back to,
    // so one that still has its body keeps it.
    if (lit->get_body() && lit->get_body()->get_type() != ASTNode::Type::BLOCK_STATEMENT) return;

    exe->defer_body(ExecutableRef<ScriptUnit>(unit), strict, is_generator, is_async);
    const_cast<Literal*>(lit)->release_body();
}

ClosureTemplate closure_template_for(const ASTNode* literal) {
    ClosureTemplate tpl;
    switch (literal->get_type()) {
        case ASTNode::Type::FUNCTION_EXPRESSION: {
            const auto* fe = static_cast<const FunctionExpression*>(literal);
            tpl.form = ClosureTemplate::Form::FunctionExpr;
            tpl.name = fe->is_named() ? fe->get_id()->get_name() : "";
            tpl.is_generator = fe->is_generator();
            tpl.is_async = fe->is_async();
            tpl.is_method_shorthand = fe->is_method_shorthand();
            tpl.needs_self_binding = fe->is_named() && !fe->is_decl_form();
            load_body_facts(fe, tpl.body_is_strict, tpl.has_direct_eval);
            tpl.declared_length = static_cast<uint32_t>(fe->get_cached_spec_length());
            if (fe->is_generator()) {
                tpl.needs_outer_env = true;  // suspendable body: not analyzed, keep the pin
            } else {
                if (fe->get_needs_outer_env_state() < 0) {
                    // A body stepped over cannot be asked whether it reaches
                    // outward, so it is taken to.
                    fe->set_needs_outer_env_state(
                        !fe->get_body() ||
                        closure_needs_outer_environment(fe->get_params(), fe->get_body(),
                                                        /*is_arrow=*/false) ? 1 : 0);
                }
                tpl.needs_outer_env = fe->get_needs_outer_env_state() != 0;
            }
            const bool fe_fresh = !fe->get_cached_executable();
            tpl.executable = ensure_shared_executable(fe->get_cached_executable(), fe->get_body(),
                                                      fe->get_params(), fe->source_start(), fe->source_end(),
                                                      fe->owning_unit());
            if (fe_fresh) fe->set_cached_executable(tpl.executable);
            defer_leaf_body(fe, tpl.executable.get(), fe_fresh, tpl.body_is_strict,
                            fe->is_generator(), fe->is_async());
            break;
        }
        case ASTNode::Type::FUNCTION_DECLARATION: {
            const auto* fd = static_cast<const FunctionDeclaration*>(literal);
            tpl.form = ClosureTemplate::Form::Declaration;
            tpl.name = fd->get_id()->get_name();
            tpl.is_generator = fd->is_generator();
            tpl.is_async = fd->is_async();
            // Only the plain branch ever pinned the environment.
            tpl.needs_outer_env = !fd->is_generator() && !fd->is_async();
            load_body_facts(fd, tpl.body_is_strict, tpl.has_direct_eval);
            const bool fd_fresh = !fd->get_cached_executable();
            tpl.executable = ensure_shared_executable(fd->get_cached_executable(), fd->get_body(),
                                                      fd->get_params(), fd->source_start(), fd->source_end(),
                                                      fd->owning_unit());
            if (fd_fresh) fd->set_cached_executable(tpl.executable);
            defer_leaf_body(fd, tpl.executable.get(), fd_fresh, tpl.body_is_strict,
                            fd->is_generator(), fd->is_async());
            tpl.declared_length = spec_length_of(*tpl.executable);
            break;
        }
        case ASTNode::Type::ARROW_FUNCTION_EXPRESSION: {
            const auto* ar = static_cast<const ArrowFunctionExpression*>(literal);
            tpl.form = ClosureTemplate::Form::Arrow;
            tpl.is_async = ar->is_async();
            tpl.is_arrow = true;
            // Only the sync branch ever pinned the environment.
            tpl.needs_outer_env = !ar->is_async();
            load_body_facts(ar, tpl.body_is_strict, tpl.has_direct_eval);
            tpl.executable = ensure_shared_executable(ar->get_cached_executable(), ar->get_body(),
                                                      ar->get_params(), ar->source_start(), ar->source_end(),
                                                      ar->owning_unit());
            const bool ar_fresh = !ar->get_cached_executable();
            if (ar_fresh) ar->set_cached_executable(tpl.executable);
            // An arrow's body is never let go of on its own. One stepped over
            // while its enclosing body was read back has no subtree to begin
            // with, and only that case needs telling where to find itself.
            if (!ar->get_body()) {
                defer_leaf_body(ar, tpl.executable.get(), ar_fresh, tpl.body_is_strict,
                                /*is_generator=*/false, ar->is_async());
            }
            tpl.declared_length = spec_length_of(*tpl.executable);
            break;
        }
        case ASTNode::Type::ASYNC_FUNCTION_EXPRESSION: {
            const auto* af = static_cast<const AsyncFunctionExpression*>(literal);
            tpl.form = ClosureTemplate::Form::AsyncFunctionExpr;
            tpl.name = af->get_id() ? af->get_id()->get_name() : "";
            tpl.is_async = true;
            tpl.is_arrow = af->is_arrow();
            tpl.needs_self_binding = af->get_id() && !af->is_arrow() && !af->is_decl_form();
            tpl.needs_outer_env = false;
            load_body_facts(af, tpl.body_is_strict, tpl.has_direct_eval);
            tpl.executable = ensure_shared_executable(af->get_cached_executable(), af->get_body(),
                                                      af->get_params(), af->source_start(), af->source_end(),
                                                      af->owning_unit());
            const bool af_fresh = !af->get_cached_executable();
            if (af_fresh) af->set_cached_executable(tpl.executable);
            if (!af->get_body()) {
                defer_leaf_body(af, tpl.executable.get(), af_fresh, tpl.body_is_strict,
                                /*is_generator=*/false, /*is_async=*/true);
            }
            tpl.declared_length = spec_length_of(*tpl.executable);
            break;
        }
        default:
            break;
    }
    return tpl;
}

// The Context-dependent half of instantiating a function literal: everything
// that differs between two closures born from the SAME source. Shared by the
// four evaluate() forms below and by Op::CreateClosure, so a compiled and an
// interpreted closure cannot come out different.
Value instantiate_closure(Context& ctx, const ClosureTemplate& tpl) {
    using Form = ClosureTemplate::Form;
    ExecutableRef<FunctionExecutable> exe = tpl.executable;

    Function* fn;
    if (tpl.is_async && tpl.is_generator) {
        fn = new AsyncGeneratorFunction(tpl.name, std::move(exe), &ctx);
    } else if (tpl.is_generator) {
        fn = new GeneratorFunction(tpl.name, std::move(exe), &ctx);
    } else if (tpl.is_async && tpl.form != Form::FunctionExpr) {
        fn = new AsyncFunction(tpl.name, std::move(exe), &ctx);
    } else {
        // Arrows are never constructible; nor is a method/getter/setter
        // shorthand (spec 14.3.9), whose .prototype the literal finalize step
        // would strip again immediately.
        bool create_prototype = !tpl.is_arrow && !tpl.is_method_shorthand &&
                                tpl.form != Form::Arrow;
        fn = new Function(tpl.name, std::move(exe), &ctx, create_prototype);
    }
    std::unique_ptr<Function> function(fn);

    if (tpl.needs_outer_env) function->mark_closure_environment_escaped();
    function->set_declared_length(tpl.declared_length);

    // A declaration and a sync arrow are the two forms whose own evaluate()
    // linked [[Prototype]] explicitly, because the shared-executable
    // constructor does not; the subclasses set their own intrinsic below.
    bool plain = !tpl.is_async && !tpl.is_generator;
    if (plain && (tpl.form == Form::Declaration || tpl.form == Form::Arrow)) {
        if (Object* func_proto = ObjectFactory::get_function_prototype()) {
            function->set_prototype(func_proto);
        }
    }

    // NamedEvaluation (spec 15.2.5/15.5.3/15.8.4): a named function expression
    // resolves its own name inside its body without aliasing an outer binding.
    // The Environment must be fresh per instantiation -- sharing it would let
    // two closures from one literal see each other's self-reference.
    if (tpl.needs_self_binding) {
        Environment* func_env = new Environment(Environment::Type::Declarative,
                                                ctx.get_lexical_environment());
        func_env->create_binding(tpl.name, Value(function.get()), false, false, false);
        function->set_closure_environment(func_env);
    }

    if (tpl.is_arrow) {
        function->set_is_arrow(true);
        if (ctx.has_binding("this")) {
            function->set_arrow_this(ctx.get_binding("this"));
        }
    }
    if (tpl.form == Form::Arrow) {
        // ContainsArguments is lexical: an arrow born inside a class field
        // initializer keeps the direct-eval `arguments` ban for its whole life,
        // however late it is called (marker read back in Function::call).
        if (ctx.is_in_class_field_init()) {
            function->set_internal_property("__in_cfi__", Value(true));
        }
        if (!tpl.is_async) {
            Value enclosing_new_target = ctx.get_new_target();
            if (!enclosing_new_target.is_undefined()) {
                function->set_internal_property("__arrow_new_target__", enclosing_new_target);
            }
        }
    }

    // Generator/async subclasses come out of the base constructor pointing at
    // plain Function.prototype -- retarget them so f.constructor is right.
    const char* intrinsic_name = (tpl.is_async && tpl.is_generator) ? "AsyncGeneratorFunction"
                                : tpl.is_generator ? "GeneratorFunction"
                                : tpl.is_async ? "@@AsyncFunction" : nullptr;
    if (intrinsic_name && tpl.form != Form::FunctionExpr && ctx.has_binding(intrinsic_name)) {
        Value ctor = ctx.get_binding(intrinsic_name);
        if (ctor.is_function()) {
            Value proto = ctor.as_function()->get_property("prototype");
            if (proto.is_object()) function->set_prototype(proto.as_object());
        }
    }

    if (tpl.form == Form::FunctionExpr && ctx.is_in_param_eval()) {
        function->set_is_param_default(true);
    }

    // ES5 10.1.1: strict if the enclosing code is strict OR the body says so.
    // No own "caller"/"arguments" go with it: ES2017 removed those from
    // ordinary functions, leaving the single %ThrowTypeError% accessor pair on
    // Function.prototype (FunctionBuiltin.cpp) to make the read throw by
    // inheritance.
    if (ctx.is_strict_mode() || tpl.body_is_strict) {
        function->set_is_strict(true);
    }

    if (tpl.has_direct_eval) {
        function->set_internal_property("__contains_eval__", Value(true));
    }
    return Value(function.release());
}

// FunctionDeclaration's own second half: instantiate, then bind the name where
// the environment shape says it belongs. Shared with Op::DeclareFunction.
Value declare_function(Context& ctx, const ClosureTemplate& tpl) {
    Value function_value = instantiate_closure(ctx, tpl);
    if (ctx.has_exception()) return Value();
    const std::string& function_name = tpl.name;

    // At the script top level the variable environment is the global Object env.
    // Function declarations there must become own properties of the global object regardless of strict mode.
    // HOWEVER, inside a block (lex env was pushed by a BlockStatement), the lex env's outer is the
    // script-hoist Declarative env (not the global Object env directly). Use that to distinguish
    // top-level from block-level so block-scoped strict-mode function decls stay in the block.
    Environment* var_env = ctx.get_variable_environment();
    Environment* lex_env = ctx.get_lexical_environment();
    bool var_env_is_global = var_env &&
        var_env->get_type() == Environment::Type::Object;
    // True only when lex_env is a DIRECT child of var_env (script-hoist env) or IS var_env.
    bool at_script_toplevel = var_env_is_global &&
        (lex_env == var_env || (lex_env && lex_env->get_outer() == var_env));

    if (at_script_toplevel) {
        ctx.create_global_function_binding(function_name, function_value);
    } else {
        // In strict mode inside a block/function where lex != var env, use lexical binding
        // so block-scoped function declarations don't bleed into the outer function scope.
        // Annex B.3.3's sloppy-mode var-scope leak applies only to plain FunctionDeclarations.
        // FunctionDeclarationInstantiation appends "arguments" to
        // parameterNames whenever the function needs an arguments object, and
        // Annex B skips the extension for a name that is a parameter. Without
        // this the block's `function arguments() {}` took the object's place
        // for the rest of the call.
        bool shadows_arguments = function_name == "arguments" && ctx.has_binding(function_name);
        bool use_lexical = shadows_arguments ||
            ((ctx.is_strict_mode() || tpl.is_generator || tpl.is_async) &&
             ctx.get_lexical_environment() != ctx.get_variable_environment());
        if (use_lexical) {
            if (!ctx.create_lexical_binding(function_name, function_value, true)) {
                ctx.create_lexical_binding_force(function_name, function_value);
            }
        } else {
            // Same rule as a `var`: the binding is not deletable outside eval
            // code (spec 10.2.11 step 36, CreateMutableBinding(fn, false)).
            if (!ctx.create_var_binding(function_name, function_value, true)) {
                // Entering the block a second time re-declares the same
                // function, and the binding this site already made is the one
                // to update (Annex B.3.3's copy out to the variable
                // environment). Binding a fresh name in the block instead left
                // every reference that had already resolved -- including a
                // cached one -- pointing at the first entry's closure. Only
                // this site's own binding is overwritten: a parameter, or a
                // name the program bound, is left alone.
                Environment* var_env_now = ctx.get_variable_environment();
                bool same_site = false;
                if (var_env_now && function_value.is_function()) {
                    Value existing = var_env_now->get_binding(function_name);
                    if (existing.is_function()) {
                        same_site = existing.as_function()->get_executable().get() ==
                                    function_value.as_function()->get_executable().get();
                    }
                }
                if (!same_site || !var_env_now->set_binding(function_name, function_value)) {
                    if (ctx.get_type() == Context::Type::Eval && !ctx.is_strict_mode()) {
                        ctx.create_global_function_binding(function_name, function_value, true);
                    } else {
                        ctx.create_lexical_binding_force(function_name, function_value);
                    }
                }
            }
        }
    }
    return Value();
}




std::string FunctionDeclaration::to_string() const {
    std::ostringstream oss;
    if (is_async_) {
        oss << "async ";
    }
    oss << "function";
    if (is_generator_) {
        oss << "*";
    }
    oss << " " << id_->get_name() << "(";
    for (size_t i = 0; i < params_.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << params_[i]->get_name();
    }
    oss << ") " << body_->to_string();
    return oss.str();
}

std::unique_ptr<ASTNode> FunctionDeclaration::clone() const {
    std::vector<std::unique_ptr<Parameter>> cloned_params;
    for (const auto& param : params_) {
        cloned_params.push_back(
            std::unique_ptr<Parameter>(static_cast<Parameter*>(param->clone().release()))
        );
    }

    auto cloned = std::make_unique<FunctionDeclaration>(
        std::unique_ptr<Identifier>(static_cast<Identifier*>(id_->clone().release())),
        std::move(cloned_params),
        std::unique_ptr<BlockStatement>(
            body_ ? static_cast<BlockStatement*>(body_->clone().release()) : nullptr),
        start_, end_, is_async_, is_generator_
    );
    // The source range is recorded post-construction, so clone() must carry it
    // (and the unit it points into) explicitly or toString() loses the source.
    cloned->set_source_ref(
        ast_detail::clone_keeps_source() ? owning_unit_ : nullptr, src_start_, src_end_);
    // A literal whose body was stepped over has nothing but its range, so a
    // copy that loses it can never be read back. One that still has its body
    // does not carry the range over: a copy is made to be rewritten, and the
    // source no longer says what it holds.
    if (!body_) {
        cloned->body_tok_first_ = body_tok_first_;
        cloned->body_tok_last_ = body_tok_last_;
        cloned->body_src_first_ = body_src_first_;
        cloned->body_is_leaf_ = body_is_leaf_;
        // And the source it refers into: with no body of its own, that is the
        // whole of what this literal is. Whatever turns the copy into an
        // executable holds the unit alive from there on.
        cloned->set_source_ref(owning_unit_, src_start_, src_end_);
    }
    return cloned;
}


// True if `stmt` is a direct `super(...)` call statement -- used to find where a
// derived class's field initializers must be spliced in an explicit constructor
// (immediately after super() returns, never before -- fields may read state the
// parent constructor just set, and `this` isn't initialized until super() runs).
// SetFunctionName (DefineField step 7): true for the node shapes whose evaluated
// value can be an otherwise-anonymous function/class -- named ones are skipped at
// runtime instead (fn->get_name() already non-empty), matching the identical check
// used for variable declarations (statements.cpp) and plain assignment (assignment.cpp).
static bool is_anonymous_function_like(const ASTNode* node) {
    if (!node) return false;
    auto t = node->get_type();
    return t == ASTNode::Type::FUNCTION_EXPRESSION ||
           t == ASTNode::Type::ARROW_FUNCTION_EXPRESSION ||
           t == ASTNode::Type::ASYNC_FUNCTION_EXPRESSION ||
           t == ASTNode::Type::CLASS_DECLARATION;
}

// Wraps an instance field's cloned value expression with __setfnname__(value, name)
// when the ORIGINAL (unresolved) value node is anonymous-function-shaped. Field
// values are deferred to construction time (unlike static fields, evaluated once
// here in ClassDeclaration::evaluate), so the naming has to happen at runtime too.
// The name is an expression rather than a string so a computed key -- known
// only once the class is built -- can name the field's function the same way.
static std::unique_ptr<ASTNode> wrap_field_value_with_name(
    std::unique_ptr<ASTNode> value_clone, const ASTNode* original_value,
    std::unique_ptr<ASTNode> name_expr, const Position& pos) {
    if (!is_anonymous_function_like(original_value)) return value_clone;
    auto setfnname_id = std::make_unique<EngineHelper>(EngineHelper::Kind::SetFunctionName, pos, pos);
    std::vector<std::unique_ptr<ASTNode>> args;
    args.push_back(std::move(value_clone));
    args.push_back(std::move(name_expr));
    return std::make_unique<CallExpression>(std::move(setfnname_id), std::move(args), pos, pos, false);
}

static std::unique_ptr<ASTNode> wrap_field_value_with_name(
    std::unique_ptr<ASTNode> value_clone, const ASTNode* original_value, const std::string& name,
    const Position& pos) {
    return wrap_field_value_with_name(std::move(value_clone), original_value,
                                      std::make_unique<StringLiteral>(name, pos, pos), pos);
}

static bool is_direct_super_call_statement(const ASTNode* stmt) {
    if (!stmt || stmt->get_type() != ASTNode::Type::EXPRESSION_STATEMENT) return false;
    const ASTNode* expr = static_cast<const ExpressionStatement*>(stmt)->get_expression();
    if (!expr || expr->get_type() != ASTNode::Type::CALL_EXPRESSION) return false;
    const auto* call = static_cast<const CallExpression*>(expr);
    return call->get_callee()->get_type() == ASTNode::Type::IDENTIFIER &&
           static_cast<const Identifier*>(call->get_callee())->get_name() == "super";
}

// The in_class_field_init flag is read by a direct eval inside an initialiser
// (ContainsArguments early error) and by a closure born in one, which carries
// the same ban for life. Rather than detect every shape that needs the flag,
// recognise the ones that provably cannot observe it.
static bool field_init_cannot_observe_cfi(const ASTNode* n) {
    if (!n) return true;  // `x;` with no initialiser
    switch (n->get_type()) {
        case ASTNode::Type::NUMBER_LITERAL:
        case ASTNode::Type::STRING_LITERAL:
        case ASTNode::Type::BOOLEAN_LITERAL:
        case ASTNode::Type::NULL_LITERAL:
        case ASTNode::Type::UNDEFINED_LITERAL:
        case ASTNode::Type::BIGINT_LITERAL:
        case ASTNode::Type::IDENTIFIER:
            return true;
        case ASTNode::Type::MEMBER_EXPRESSION: {
            const auto* m = static_cast<const MemberExpression*>(n);
            if (m->is_computed()) return false;
            return field_init_cannot_observe_cfi(m->get_object());
        }
        case ASTNode::Type::BINARY_EXPRESSION: {
            const auto* b = static_cast<const BinaryExpression*>(n);
            return field_init_cannot_observe_cfi(b->get_left()) &&
                   field_init_cannot_observe_cfi(b->get_right());
        }
        default:
            return false;
    }
}

// A class definition's own expressions -- computed keys, the heritage, a
// static field's value -- are compiled and run rather than walked. The
// fallback stays while the compiler still turns some shapes away; every use
// of it is a shape that keeps ASTNode::evaluate alive.
static Value eval_class_expr(const ASTNode* expr, Context& ctx) {
    bool compiled = false;
    Value v = VM::run_expression(expr, ctx, compiled);
    if (!compiled) {
        ctx.throw_type_error("Internal: class expression could not be compiled");
        return Value();
    }
    return v;
}



// What a value would construct: itself when it is a constructor, and what it
// stands in front of when it is a Proxy over one.
Function* resolve_construct_target(Object* obj) {
    while (obj && obj->get_type() == Object::ObjectType::Proxy) {
        obj = static_cast<Proxy*>(obj)->get_proxy_target();
    }
    if (!obj || !obj->is_function()) return nullptr;
    Function* fn = static_cast<Function*>(obj);
    return fn->is_constructor() ? fn : nullptr;
}

Value ClassDeclaration::define_class(Context& ctx) {
    std::string class_name = id_->get_name();

    // classScope (spec step 2-4): the class name is an inner IMMUTABLE binding,
    // independent from any outer one, visible to the heritage expression (TDZ
    // until the constructor exists), methods and static initializers. Every
    // closure created below captures this environment.
    struct ClassScopeGuard {
        Context& c;
        bool active = false;
        ~ClassScopeGuard() { if (active) c.pop_block_scope(); }
    } class_scope_guard{ctx};
    if (!class_name.empty()) {
        ctx.push_block_scope();
        class_scope_guard.active = true;
        if (Environment* env = ctx.get_lexical_environment()) {
            env->create_uninitialized_binding(class_name, /*is_mutable=*/false);
            env->mark_const_binding(class_name);
        }
    }

    CallStack& outer_cs_ = CallStack::instance();
    Object* outer_brands_ptr = nullptr;
    if (!outer_cs_.is_empty() && outer_cs_.top().function_ptr) {
        outer_brands_ptr = outer_cs_.top().function_ptr->private_brands();
    }
    std::vector<std::string> private_instance_names;
    std::vector<std::string> private_instance_method_names;
    std::vector<std::string> private_static_names;
    Object* instance_brands_raw = nullptr;

    auto prototype = ObjectFactory::create_object();

    std::unique_ptr<ASTNode> constructor_body = nullptr;
    std::vector<std::unique_ptr<Parameter>> constructor_params;
    // The constructor's own literal. Cloning its body here on every evaluation
    // is what a class defined inside a hot function used to pay; the clone is
    // now deferred to the paths that actually rewrite the body, and the
    // untouched case shares one executable through the node, exactly as the
    // methods below already do.
    FunctionExpression* ctor_func_expr = nullptr;
    // A computed field key is resolved on each evaluation and baked into the
    // synthesised constructor, so that body cannot be shared.
    // Resolved computed keys of INSTANCE fields, in declaration order. The
    // constructor reads them back by index (EngineHelper::ClassFieldKey).
    std::vector<std::string> computed_instance_keys;
    std::vector<std::unique_ptr<ASTNode>> field_initializers;
    std::vector<std::unique_ptr<ASTNode>> static_field_initializers;
    bool has_explicit_constructor = false;
    std::vector<std::pair<std::string, PropertyDescriptor>> deferred_instance_methods;
    // deferred_instance_methods' Function Values live inside PropertyDescriptor,
    // which ValueVectorRoot can't root directly (it only walks vector<Value>).
    // Mirror every such Value into this flat, rooted vector too -- computed
    // method keys can run arbitrary side-effecting code (toString()) between
    // when one method is deferred here and the next is processed.
    // Every computed key is evaluated once, where it stands, and the second
    // pass reads the answer back instead of running the expression again.
    std::unordered_map<const ASTNode*, std::string> resolved_static_method_keys;
    std::vector<Value> deferred_methods_values_root_vec;
    ValueVectorRoot deferred_methods_values_root(&deferred_methods_values_root_vec);

    if (body_) {
        for (const auto& stmt : body_->get_statements()) {
            if (stmt->get_type() == Type::CLASS_FIELD) {
                ClassField* cf = static_cast<ClassField*>(stmt.get());
                std::unique_ptr<ASTNode> resolved_stmt = stmt->clone();
                if (cf->is_computed()) {
                    // Computed field keys are evaluated exactly once, right here, in strict
                    // declaration order (interleaved with methods and static/instance fields
                    // alike) -- per spec, only the field's VALUE is deferred (instance: to
                    // each construction; static: to right after the class object is built).
                    // Bake the resolved key into a literal so it's never re-evaluated later.
                    Value key_val = eval_class_expr(cf->get_key(), ctx);
                    if (ctx.has_exception()) return Value();
                    std::string resolved_key = computed_key_to_property_key(ctx, key_val);
                    if (ctx.has_exception()) return Value();
                    const Position& kpos = cf->get_key()->get_start();
                    auto value_clone = cf->get_value() ? cf->get_value()->clone() : nullptr;
                    if (cf->is_static()) {
                        // Applied where the class is built, so the resolved key can
                        // simply stand in the statement.
                        auto literal_key = std::make_unique<StringLiteral>(resolved_key, kpos, kpos);
                        resolved_stmt = std::make_unique<ClassField>(
                            std::move(literal_key), std::move(value_clone), /*is_static=*/true,
                            /*computed=*/false, cf->get_start(), cf->get_end());
                    } else {
                        // Goes into the constructor, where a baked string would make
                        // the body differ per evaluation. The key is parked on the
                        // class and the body reads it back by index; `computed` stays
                        // set to mark the number below as that index.
                        auto index_key = std::make_unique<NumberLiteral>(
                            static_cast<double>(computed_instance_keys.size()), kpos, kpos);
                        computed_instance_keys.push_back(resolved_key);
                        resolved_stmt = std::make_unique<ClassField>(
                            std::move(index_key), std::move(value_clone), /*is_static=*/false,
                            /*computed=*/true, cf->get_start(), cf->get_end());
                    }
                }
                if (cf->is_static()) {
                    static_field_initializers.push_back(std::move(resolved_stmt));
                    if (!cf->is_computed()) {
                        if (Identifier* kid = dynamic_cast<Identifier*>(cf->get_key())) {
                            if (!kid->get_name().empty() && kid->get_name()[0] == '#')
                                private_static_names.push_back(kid->get_name());
                        }
                    }
                } else {
                    field_initializers.push_back(std::move(resolved_stmt));
                    if (!cf->is_computed()) {
                        if (Identifier* kid = dynamic_cast<Identifier*>(cf->get_key())) {
                            if (!kid->get_name().empty() && kid->get_name()[0] == '#')
                                private_instance_names.push_back(kid->get_name());
                        }
                    }
                }
                continue;
            }
            if (stmt->get_type() == Type::CLASS_STATIC_BLOCK) {
                static_field_initializers.push_back(stmt->clone());
                continue;
            }

            if (stmt->get_type() == Type::EXPRESSION_STATEMENT) {
                field_initializers.push_back(stmt->clone());
                continue;
            }

            if (stmt->get_type() == Type::METHOD_DEFINITION) {
                MethodDefinition* method = static_cast<MethodDefinition*>(stmt.get());
                std::string method_name;
                // A computed key belongs to the class's declaration order, which
                // interleaves methods and fields, static and not. The static
                // method itself is built in the pass below; only its key is
                // settled here, and that pass reads it back rather than running
                // the expression a second time.
                if (method->is_computed() && method->is_static()) {
                    Value skey = eval_class_expr(method->get_key(), ctx);
                    if (ctx.has_exception()) return Value();
                    std::string resolved = computed_key_to_property_key(ctx, skey);
                    if (ctx.has_exception()) return Value();
                    resolved_static_method_keys.emplace(stmt.get(), std::move(resolved));
                } else if (method->is_computed()) {
                    Value key_val = eval_class_expr(method->get_key(), ctx);
                    if (ctx.has_exception()) return Value();
                    method_name = computed_key_to_property_key(ctx, key_val);
                    if (ctx.has_exception()) return Value();
                } else if (Identifier* id = dynamic_cast<Identifier*>(method->get_key())) {
                    method_name = id->get_name();
                } else if (StringLiteral* str = dynamic_cast<StringLiteral*>(method->get_key())) {
                    method_name = str->get_value();
                } else if (NumberLiteral* num = dynamic_cast<NumberLiteral*>(method->get_key())) {
                    method_name = Value(num->get_value()).to_property_key();
                } else {
                    method_name = "[unknown]";
                }

                if (method->is_constructor()) {
                    has_explicit_constructor = true;
                    if (method->get_value()->get_type() == Type::FUNCTION_EXPRESSION) {
                        ctor_func_expr = static_cast<FunctionExpression*>(method->get_value());
                    }
                } else if (method->is_static()) {
                    if (!method_name.empty() && method_name[0] == '#')
                        private_static_names.push_back(method_name);
                } else {
                    if (!method_name.empty() && method_name[0] == '#') {
                        private_instance_names.push_back(method_name);
                        private_instance_method_names.push_back(method_name);
                    }
                    bool method_is_gen = false;
                    bool method_is_async = false;
                    if (method->get_value()->get_type() == Type::FUNCTION_EXPRESSION) {
                        FunctionExpression* func_expr = static_cast<FunctionExpression*>(method->get_value());
                        method_is_gen = func_expr->is_generator();
                        method_is_async = func_expr->is_async();
                    }
                    std::unique_ptr<Function> instance_method;
                    // Method syntax parses to a FunctionExpression internally -- share its
                    // executable via the same cache-on-node mechanism FunctionExpression's own
                    // evaluate() uses, so methods from one class declaration get real reuse
                    // whenever the class itself is nested inside a repeatedly-called function.
                    // Generator/async-generator/async methods share the SAME executable slot
                    // (mutually exclusive shapes for the same method node).
                    ExecutableRef<FunctionExecutable> exe;
                    FunctionExpression* method_func_expr =
                        method->get_value()->get_type() == Type::FUNCTION_EXPRESSION
                            ? static_cast<FunctionExpression*>(method->get_value()) : nullptr;
                    if (method_func_expr) exe = method_func_expr->get_cached_executable();
                    if (!exe) {
                        // Only the first evaluation of this method literal clones;
                        // every later one reuses the executable off the node.
                        exe = make_executable_ref();
                        install_literal_body(exe.get(), method->get_value());
                        if (method_func_expr) {
                            for (const auto& param : method_func_expr->get_params()) {
                                exe->parameter_objects.push_back(
                                    std::unique_ptr<Parameter>(static_cast<Parameter*>(param->clone().release())));
                                exe->parameters.push_back(
                                    exe->parameter_objects.back()->get_name()->get_name());
                            }
                            method_func_expr->set_cached_executable(exe);
                        }
                    }
                    size_t method_declared_length = 0;
                    for (const auto& p : exe->parameter_objects) {
                        if (p->is_rest() || p->has_default()) break;
                        method_declared_length++;
                    }
                    if (method_is_gen && method_is_async) {
                        instance_method = std::make_unique<AsyncGeneratorFunction>(method_name, std::move(exe), &ctx);
                    } else if (method_is_gen) {
                        instance_method = std::make_unique<GeneratorFunction>(method_name, std::move(exe), &ctx);
                    } else if (method_is_async) {
                        instance_method = std::make_unique<AsyncFunction>(method_name, std::move(exe), &ctx);
                    } else {
                        // Class methods are non-constructors; non-generator methods have no prototype
                        // (async ones already skip it in AsyncFunction's own ctor).
                        instance_method = std::make_unique<Function>(method_name, std::move(exe), &ctx, /*create_prototype=*/false);
                        // Matches create_js_function's own two follow-up steps (see
                        // FunctionDeclaration::evaluate's identical rationale).
                        instance_method->mark_closure_environment_escaped();
                        if (Object* func_proto = ObjectFactory::get_function_prototype()) {
                            instance_method->set_prototype(func_proto);
                        }
                    }
                    instance_method->set_declared_length(method_declared_length);
                    if (!method->get_source_text().empty()) instance_method->set_source_text(method->get_source_text());
                    instance_method->set_is_strict(true);
                    instance_method->set_internal_slot("__private_class_brand__", Value(prototype.get()));

                    // Private methods/accessors are stored on the prototype under the
                    // qualified key ("#m@<protoPtr>", the same key resolve_private_storage_key
                    // derives from the brand map) so a computed ["#m"] ordinary property
                    // can never collide with them.
                    std::string storage_name = method_name;
                    if (!method_name.empty() && method_name[0] == '#') {
                        storage_name = method_name + "@" + std::to_string(reinterpret_cast<uintptr_t>(prototype.get()));
                    }
                    if (method->get_kind() == MethodDefinition::GETTER || method->get_kind() == MethodDefinition::SETTER) {
                        bool is_getter = method->get_kind() == MethodDefinition::GETTER;
                        instance_method->set_name(accessor_function_name(method_name, is_getter ? "get " : "set "));
                        // Find existing deferred entry or create new one
                        PropertyDescriptor* existing_deferred = nullptr;
                        for (auto& dp : deferred_instance_methods) {
                            if (dp.first == storage_name) { existing_deferred = &dp.second; break; }
                        }
                        PropertyDescriptor desc;
                        if (existing_deferred && existing_deferred->is_accessor_descriptor()) desc = *existing_deferred;
                        if (is_getter) desc.set_getter(instance_method.release());
                        else desc.set_setter(instance_method.release());
                        desc.set_enumerable(false);
                        desc.set_configurable(true);
                        if (desc.get_getter()) deferred_methods_values_root_vec.push_back(Value(desc.get_getter()));
                        if (desc.get_setter()) deferred_methods_values_root_vec.push_back(Value(desc.get_setter()));
                        if (existing_deferred) *existing_deferred = desc;
                        else deferred_instance_methods.push_back({storage_name, desc});
                    } else {
                        if (method_name.find("@@sym:") == 0 || method_name.find("Symbol.") == 0) {
                            instance_method->set_name(accessor_function_name(method_name, ""));
                        }
                        Function* instance_method_raw = instance_method.release();
                        deferred_methods_values_root_vec.push_back(Value(instance_method_raw));
                        PropertyDescriptor method_desc(Value(instance_method_raw),
                            static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
                        deferred_instance_methods.push_back({storage_name, method_desc});
                    }
                }
            }
        }
    }

    // Base classes with private methods need the brand slot added in the constructor.
    // Derived classes get it after super() returns (handled in call.cpp / Function.cpp).
    // A base class's private-method brand is installed by Function::construct
    // before the body runs (spec 10.2.2 step 8), so nothing about it reaches the
    // synthesised body any more. What is left of it is deterministic unless an
    // instance field's computed key was resolved into it, which is what decides
    // whether one executable can serve every evaluation of this class site.
    const bool ctor_body_rewritten = !field_initializers.empty();
    const bool ctor_exe_cacheable = true;
    ExecutableRef<FunctionExecutable> cached_ctor =
        ctor_exe_cacheable ? get_cached_ctor_exe() : ExecutableRef<FunctionExecutable>();
    const bool build_ctor_body = !cached_ctor && (ctor_body_rewritten || !ctor_func_expr);

    if (build_ctor_body) {
        if (ctor_func_expr) {
            constructor_body = ctor_func_expr->get_body()->clone();
            const auto& ctor_ps = ctor_func_expr->get_params();
            constructor_params.reserve(ctor_ps.size());
            for (const auto& param : ctor_ps) {
                constructor_params.push_back(
                    std::unique_ptr<Parameter>(static_cast<Parameter*>(param->clone().release())));
            }
        } else {
            std::vector<std::unique_ptr<ASTNode>> empty_statements;
            constructor_body = std::make_unique<BlockStatement>(
                std::move(empty_statements), Position{0, 0}, Position{0, 0});
        }
    }

    if (ctor_body_rewritten && build_ctor_body) {
        BlockStatement* body_block = static_cast<BlockStatement*>(constructor_body.get());
        std::vector<std::unique_ptr<ASTNode>> new_statements;

        // Otherwise the enter/exit pair costs a global lookup and a native call
        // per construction for a flag nothing will read.
        bool needs_cfi_flag = false;
        for (const auto& fi : field_initializers) {
            const ASTNode* init_value = fi->get_type() == Type::CLASS_FIELD
                ? static_cast<ClassField*>(fi.get())->get_value() : fi.get();
            if (fi->get_type() != Type::CLASS_FIELD ||
                !field_init_cannot_observe_cfi(init_value)) {
                needs_cfi_flag = true;
                break;
            }
        }
        if (needs_cfi_flag) {
            Position z{0,0};
            auto enter_id = std::make_unique<EngineHelper>(EngineHelper::Kind::ClassFieldInitEnter, z, z);
            std::vector<std::unique_ptr<ASTNode>> no_args;
            auto enter_call = std::make_unique<CallExpression>(std::move(enter_id), std::move(no_args), z, z, false);
            new_statements.push_back(std::make_unique<ExpressionStatement>(std::move(enter_call), z, z));
        }

        if (!field_initializers.empty()) {
        for (auto& field_init : field_initializers) {
            if (field_init->get_type() == Type::CLASS_FIELD) {
                ClassField* cf = static_cast<ClassField*>(field_init.get());
                Position fstart = cf->get_start();

                // Check if this is a private field declaration (name starts with #)
                bool is_private = !cf->is_computed() &&
                    cf->get_key()->get_type() == ASTNode::Type::IDENTIFIER &&
                    !static_cast<Identifier*>(cf->get_key())->get_name().empty() &&
                    static_cast<Identifier*>(cf->get_key())->get_name()[0] == '#';

                if (is_private) {
                    // Private field: __pfadd__(this, "#name", value?) -- PrivateFieldAdd.
                    // The initializer is the third ARGUMENT so it evaluates before the
                    // slot is added (spec DefineField order: a side effect like
                    // Object.preventExtensions(this) inside it must make the add throw,
                    // and self-reads of the field during it must not see a slot yet).
                    const std::string& pname = static_cast<Identifier*>(cf->get_key())->get_name();
                    auto pfadd_id = std::make_unique<EngineHelper>(EngineHelper::Kind::PrivateFieldAdd, fstart, fstart);
                    auto this_id0 = std::make_unique<Identifier>("this", fstart, fstart);
                    auto pname_lit = std::make_unique<StringLiteral>(pname, fstart, fstart);
                    std::vector<std::unique_ptr<ASTNode>> pfadd_args;
                    pfadd_args.push_back(std::move(this_id0));
                    pfadd_args.push_back(std::move(pname_lit));
                    if (cf->get_value()) {
                        pfadd_args.push_back(wrap_field_value_with_name(
                            cf->get_value()->clone(), cf->get_value(), pname, fstart));
                    }
                    auto pfadd_call = std::make_unique<CallExpression>(
                        std::move(pfadd_id), std::move(pfadd_args), fstart, fstart, false);
                    new_statements.push_back(std::make_unique<ExpressionStatement>(std::move(pfadd_call), fstart, fstart));
                } else {
                    // `computed` marks the key node as the index of a key the class
                    // resolved once; every other form spells the key outright.
                    const bool key_from_class = cf->is_computed();
                    const double key_index = key_from_class
                        ? static_cast<NumberLiteral*>(cf->get_key())->get_value() : 0.0;
                    auto make_key_expr = [&]() -> std::unique_ptr<ASTNode> {
                        auto helper = std::make_unique<EngineHelper>(EngineHelper::Kind::ClassFieldKey, fstart, fstart);
                        std::vector<std::unique_ptr<ASTNode>> key_args;
                        key_args.push_back(std::make_unique<NumberLiteral>(key_index, fstart, fstart));
                        return std::make_unique<CallExpression>(std::move(helper), std::move(key_args), fstart, fstart, false);
                    };
                    std::string field_name;
                    if (!key_from_class) {
                        if (cf->get_key()->get_type() == ASTNode::Type::IDENTIFIER) {
                            field_name = static_cast<Identifier*>(cf->get_key())->get_name();
                        } else if (cf->get_key()->get_type() == ASTNode::Type::STRING_LITERAL) {
                            field_name = static_cast<StringLiteral*>(cf->get_key())->get_value();
                        } else if (cf->get_key()->get_type() == ASTNode::Type::NUMBER_LITERAL) {
                            field_name = Value(static_cast<NumberLiteral*>(cf->get_key())->get_value())
                                             .to_property_key();
                        }
                    }
                    std::unique_ptr<ASTNode> init_val;
                    if (cf->get_value()) {
                        init_val = key_from_class
                            ? wrap_field_value_with_name(cf->get_value()->clone(), cf->get_value(),
                                                         make_key_expr(), fstart)
                            : wrap_field_value_with_name(cf->get_value()->clone(), cf->get_value(),
                                                         field_name, fstart);
                    } else {
                        init_val = std::make_unique<Identifier>("undefined", fstart, fstart);
                    }
                    // DefineField step 9: CreateDataPropertyOrThrow defines an own data
                    // property directly -- it must NOT walk the prototype chain for an
                    // inherited setter the way a normal `this.x = value` assignment would.
                    auto deffield_id = std::make_unique<EngineHelper>(EngineHelper::Kind::DefineField, fstart, fstart);
                    auto this_id = std::make_unique<Identifier>("this", fstart, fstart);
                    std::unique_ptr<ASTNode> key_node = key_from_class
                        ? make_key_expr()
                        : std::unique_ptr<ASTNode>(std::make_unique<StringLiteral>(field_name, fstart, fstart));
                    std::vector<std::unique_ptr<ASTNode>> deffield_args;
                    deffield_args.push_back(std::move(this_id));
                    deffield_args.push_back(std::move(key_node));
                    deffield_args.push_back(std::move(init_val));
                    auto deffield_call = std::make_unique<CallExpression>(
                        std::move(deffield_id), std::move(deffield_args), fstart, fstart, false);
                    new_statements.push_back(std::make_unique<ExpressionStatement>(std::move(deffield_call), fstart, fstart));
                }
            } else {
                new_statements.push_back(std::move(field_init));
            }
        }

        if (needs_cfi_flag) {
            Position z{0,0};
            auto exit_id = std::make_unique<EngineHelper>(EngineHelper::Kind::ClassFieldInitExit, z, z);
            std::vector<std::unique_ptr<ASTNode>> no_args;
            auto exit_call = std::make_unique<CallExpression>(std::move(exit_id), std::move(no_args), z, z, false);
            new_statements.push_back(std::make_unique<ExpressionStatement>(std::move(exit_call), z, z));
        }
        } // end if (!field_initializers.empty())

        const auto& orig_statements = body_block->get_statements();
        if (has_superclass()) {
            // Derived class with an explicit constructor: field initializers must run
            // right after super() returns, not before it (this isn't bound yet, and a
            // field's initializer may read state the parent constructor just set).
            // Find the top-level `super(...)` call and splice the field-init code
            // (currently sitting in new_statements) in right after it.
            bool found_super = false;
            size_t super_stmt_index = 0;
            for (size_t i = 0; i < orig_statements.size(); i++) {
                if (is_direct_super_call_statement(orig_statements[i].get())) {
                    super_stmt_index = i;
                    found_super = true;
                    break;
                }
            }
            std::vector<std::unique_ptr<ASTNode>> field_init_stmts = std::move(new_statements);
            new_statements.clear();
            if (found_super) {
                size_t i = 0;
                for (; i <= super_stmt_index; i++) {
                    new_statements.push_back(orig_statements[i]->clone());
                }
                for (auto& s : field_init_stmts) new_statements.push_back(std::move(s));
                for (; i < orig_statements.size(); i++) {
                    new_statements.push_back(orig_statements[i]->clone());
                }
            } else {
                // No top-level super(...) call found (e.g. called indirectly through
                // a nested closure) -- fall back to the historical prepend-at-start
                // placement rather than guessing a splice point.
                for (auto& s : field_init_stmts) new_statements.push_back(std::move(s));
                for (auto& stmt : orig_statements) new_statements.push_back(stmt->clone());
            }
        } else {
            for (auto& stmt : orig_statements) {
                new_statements.push_back(stmt->clone());
            }
        }

        constructor_body = std::make_unique<BlockStatement>(
            std::move(new_statements),
            Position{0, 0},
            Position{0, 0}
        );
    }

    // Anonymous class expressions take their binding site's inferred name for
    // SetFunctionName only -- class_name itself stays empty so no inner
    // self-reference binding is created for it.
    const std::string ctor_name =
        (class_name.empty() && !inferred_name_.empty()) ? inferred_name_ : class_name;
    std::unique_ptr<Function> constructor_fn;
    ExecutableRef<FunctionExecutable> ctor_exe;
    if (ctor_func_expr && !ctor_body_rewritten) {
        // Explicit constructor whose body is used as written: shared through
        // the constructor literal's own node, like any other method.
        ctor_exe = ctor_func_expr->get_cached_executable();
        if (!ctor_exe) {
            ctor_exe = make_executable_ref();
            install_literal_body(ctor_exe.get(), ctor_func_expr);
            for (const auto& param : ctor_func_expr->get_params()) {
                ctor_exe->parameter_objects.push_back(
                    std::unique_ptr<Parameter>(static_cast<Parameter*>(param->clone().release())));
                ctor_exe->parameters.push_back(
                    ctor_exe->parameter_objects.back()->get_name()->get_name());
            }
            ctor_func_expr->set_cached_executable(ctor_exe);
        }
    } else {
        // Synthesised (fields, brand slot) or default empty body: shared through
        // the class node, but only when it comes out the same every time.
        ctor_exe = std::move(cached_ctor);
        if (!ctor_exe) {
            ctor_exe = make_executable_ref();
            ctor_exe->adopt_body(std::move(constructor_body));
            ctor_exe->parameter_objects = std::move(constructor_params);
            for (const auto& p : ctor_exe->parameter_objects) {
                ctor_exe->parameters.push_back(p->get_name()->get_name());
            }
            if (ctor_exe_cacheable) set_cached_ctor_exe(ctor_exe);
        }
    }

    size_t ctor_length = 0;
    for (const auto& p : ctor_exe->parameter_objects) {
        if (p->is_rest() || p->has_default()) break;
        ctor_length++;
    }
    constructor_fn = std::make_unique<Function>(ctor_name, std::move(ctor_exe), &ctx,
                                               /*create_prototype=*/true);
    // Matches create_js_function's own two follow-up steps.
    constructor_fn->mark_closure_environment_escaped();
    if (Object* func_proto = ObjectFactory::get_function_prototype()) {
        constructor_fn->set_prototype(func_proto);
    }
    constructor_fn->set_declared_length(ctor_length);

    Object* proto_ptr = prototype.get();
    if (constructor_fn.get() && proto_ptr) {
        // Spec 15.7.14: class prototype is non-writable, non-enumerable, non-configurable
        PropertyDescriptor proto_desc(Value(proto_ptr), static_cast<PropertyAttributes>(0));
        constructor_fn->set_property_descriptor("prototype", proto_desc);
        // Add constructor first so it appears first in getOwnPropertyNames per spec.
        {
            PropertyDescriptor ctor_desc(Value(constructor_fn.get()),
                static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
            proto_ptr->set_property_descriptor("constructor", ctor_desc);
        }
        // Add instance methods after constructor to ensure constructor comes first.
        for (auto& dm : deferred_instance_methods)
            proto_ptr->set_property_descriptor(dm.first, dm.second);
        constructor_fn->set_is_class_constructor(true);
        constructor_fn->set_is_strict(true);
        constructor_fn->set_construct_slot_hint(static_cast<uint32_t>(field_initializers.size()));
        constructor_fn->set_internal_slot("__private_class_brand__", Value(proto_ptr));

        {
            auto instance_brands = ObjectFactory::create_object();
            if (outer_brands_ptr) {
                for (const auto& k : outer_brands_ptr->get_own_property_keys())
                    instance_brands->set_property(k, outer_brands_ptr->get_property(k));
            }
            for (const auto& pn : private_instance_names)
                instance_brands->set_property(pn, Value(proto_ptr));
            for (const auto& pn : private_static_names)
                instance_brands->set_property(pn, Value(constructor_fn.get()));
            instance_brands_raw = instance_brands.release();
            constructor_fn->set_private_brands(instance_brands_raw);
            if (!private_instance_method_names.empty()) {
                // Per-class unique brand slot: use prototype address so each class evaluation
                // gets its own slot name. This prevents cross-class brand confusion when
                // multiple evaluations of the same class body produce different brands.
                std::string pm_slot = "#[[pm:" + std::to_string(reinterpret_cast<uintptr_t>(proto_ptr)) + "]]";
                constructor_fn->set_pm_brand_slot(pm_slot);
                // Mark which private names are methods (not fields) so the brand check
                // can distinguish "must have per-instance slot" from "must have method slot".
                auto method_names_obj = ObjectFactory::create_object();
                for (const auto& mn : private_instance_method_names)
                    method_names_obj->set_property(mn, Value(true));
                constructor_fn->set_internal_slot("__private_method_names__", Value(method_names_obj.release()));
            }
        }

        // The constructor body asks for these by index (EngineHelper::ClassFieldKey),
        // which is what keeps one body good for every evaluation of the class.
        if (!computed_instance_keys.empty()) {
            auto keys_arr = ObjectFactory::create_array(computed_instance_keys.size());
            for (size_t ki = 0; ki < computed_instance_keys.size(); ki++) {
                keys_arr->set_element(static_cast<uint32_t>(ki), Value(computed_instance_keys[ki]));
            }
            constructor_fn->set_internal_slot("__computed_field_keys__", Value(keys_arr.release()));
        }

        if (!has_explicit_constructor) {
            constructor_fn->set_default_ctor();
        }
        if (has_source_range()) {
            constructor_fn->set_source_text(get_source_text());
        }

        prototype.release();
    } else {
        ctx.throw_exception(Value(std::string("Class setup failed: null constructor or prototype")));
        return Value();
    }

    if (body_) {
        for (const auto& stmt : body_->get_statements()) {
            if (stmt->get_type() == Type::METHOD_DEFINITION) {
                MethodDefinition* method = static_cast<MethodDefinition*>(stmt.get());
                if (method->is_static()) {
                    std::string method_name;
                    if (method->is_computed()) {
                        auto rk = resolved_static_method_keys.find(stmt.get());
                        if (rk == resolved_static_method_keys.end()) continue;
                        method_name = rk->second;
                        // Computed static method named 'prototype' is a runtime TypeError
                        if (method_name == "prototype") {
                            ctx.throw_type_error("Class may not have a static property named 'prototype'");
                            return Value();
                        }
                    } else if (Identifier* id = dynamic_cast<Identifier*>(method->get_key())) {
                        method_name = id->get_name();
                    } else if (StringLiteral* str = dynamic_cast<StringLiteral*>(method->get_key())) {
                        method_name = str->get_value();
                    } else if (NumberLiteral* num = dynamic_cast<NumberLiteral*>(method->get_key())) {
                        // The literal's own value spells the canonical property key
                        // (0b10 becomes "2"); nothing here needs evaluating.
                        method_name = Value(num->get_value()).to_property_key();
                    } else {
                        method_name = "[unknown]";
                    }
                    bool static_is_gen = false;
                    bool static_is_async = false;
                    std::vector<std::unique_ptr<Parameter>> static_params;
                    if (method->get_value()->get_type() == Type::FUNCTION_EXPRESSION) {
                        FunctionExpression* func_expr = static_cast<FunctionExpression*>(method->get_value());
                        static_is_gen = func_expr->is_generator();
                        static_is_async = func_expr->is_async();
                        const auto& params = func_expr->get_params();
                        static_params.reserve(params.size());
                        for (const auto& param : params) {
                            static_params.push_back(std::unique_ptr<Parameter>(static_cast<Parameter*>(param->clone().release())));
                        }
                    }
                    std::unique_ptr<Function> static_method;
                    // See the instance-method branch above for the executable-sharing
                    // rationale -- generator/async-generator/async statics share the
                    // SAME executable slot (mutually exclusive shapes for the same node).
                    ExecutableRef<FunctionExecutable> exe;
                    FunctionExpression* method_func_expr =
                        method->get_value()->get_type() == Type::FUNCTION_EXPRESSION
                            ? static_cast<FunctionExpression*>(method->get_value()) : nullptr;
                    if (method_func_expr) exe = method_func_expr->get_cached_executable();
                    if (!exe) {
                        exe = make_executable_ref();
                        install_literal_body(exe.get(), method->get_value());
                        for (const auto& p : static_params) exe->parameters.push_back(p->get_name()->get_name());
                        exe->parameter_objects = std::move(static_params);
                        if (method_func_expr) method_func_expr->set_cached_executable(exe);
                    }
                    size_t method_declared_length = 0;
                    for (const auto& p : exe->parameter_objects) {
                        if (p->is_rest() || p->has_default()) break;
                        method_declared_length++;
                    }
                    if (static_is_gen && static_is_async) {
                        static_method = std::make_unique<AsyncGeneratorFunction>(method_name, std::move(exe), &ctx);
                    } else if (static_is_gen) {
                        static_method = std::make_unique<GeneratorFunction>(method_name, std::move(exe), &ctx);
                    } else if (static_is_async) {
                        static_method = std::make_unique<AsyncFunction>(method_name, std::move(exe), &ctx);
                    } else {
                        // Class static methods are non-constructors; non-generator methods have no
                        // prototype (async ones already skip it in AsyncFunction's own ctor).
                        static_method = std::make_unique<Function>(method_name, std::move(exe), &ctx, /*create_prototype=*/false);
                        static_method->mark_closure_environment_escaped();
                        if (Object* func_proto = ObjectFactory::get_function_prototype()) {
                            static_method->set_prototype(func_proto);
                        }
                    }
                    static_method->set_declared_length(method_declared_length);
                    if (!method->get_source_text().empty()) static_method->set_source_text(method->get_source_text());
                    static_method->set_is_strict(true);
                    static_method->set_internal_slot("__private_class_brand__", Value(constructor_fn.get()));
                    if (instance_brands_raw)
                        static_method->set_private_brands(instance_brands_raw);
                    // member.cpp's super lookup needs to know this resolves on the constructor itself, not its .prototype.
                    static_method->set_static_method();

                    // Same qualified-key scheme as instance private methods; the
                    // static brand is the constructor itself.
                    std::string static_storage_name = method_name;
                    if (!method_name.empty() && method_name[0] == '#') {
                        static_storage_name = method_name + "@" + std::to_string(reinterpret_cast<uintptr_t>(constructor_fn.get()));
                    }
                    if (method->get_kind() == MethodDefinition::GETTER || method->get_kind() == MethodDefinition::SETTER) {
                        bool is_getter = method->get_kind() == MethodDefinition::GETTER;
                        static_method->set_name(accessor_function_name(method_name, is_getter ? "get " : "set "));
                        PropertyDescriptor existing = constructor_fn->get_property_descriptor(static_storage_name);
                        PropertyDescriptor desc;
                        if (existing.is_accessor_descriptor() || existing.has_getter() || existing.has_setter()) {
                            desc = existing;
                        }
                        if (is_getter) {
                            desc.set_getter(static_method.release());
                        } else {
                            desc.set_setter(static_method.release());
                        }
                        desc.set_enumerable(false);
                        desc.set_configurable(true);
                        constructor_fn->set_property_descriptor(static_storage_name, desc);
                    } else {
                        if (method_name.find("@@sym:") == 0 || method_name.find("Symbol.") == 0) {
                            static_method->set_name(accessor_function_name(method_name, ""));
                        }
                        PropertyDescriptor method_desc(Value(static_method.release()),
                            static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
                        constructor_fn->set_property_descriptor(static_storage_name, method_desc);
                    }
                }
            }
        }
    }

    if (has_superclass()) {
        // ClassHeritage is class code: always strict (functions defined in it
        // must come out strict even when the class appears in sloppy code).
        bool saved_strict = ctx.is_strict_mode();
        ctx.set_strict_mode(true);
        Value super_constructor = eval_class_expr(superclass_.get(), ctx);
        ctx.set_strict_mode(saved_strict);
        if (ctx.has_exception()) return Value();

        if (super_constructor.is_null()) {
            if (proto_ptr) {
                proto_ptr->set_prototype(nullptr);
            }
            // Mark constructor so super() throws TypeError (spec: superclass null -> FunctionPrototype, not a constructor)
            constructor_fn->set_super_is_null();
        } else if (!super_constructor.is_object_like()) {
            // extends non-object (number, string, boolean, etc.) -> TypeError
            ctx.throw_type_error("Class extends value " + super_constructor.to_string() + " is not a constructor or null");
            return Value();
        } else if (super_constructor.is_object_like() && super_constructor.as_object()) {
            Object* super_obj = super_constructor.as_object();
            // Must be a constructor. A Proxy standing in front of one answers
            // is_function() for itself, not for what it wraps, so it is
            // followed to its target rather than turned away.
            Function* super_fn_check = resolve_construct_target(super_obj);
            if (!super_fn_check) {
                ctx.throw_type_error("Class extends value is not a constructor or null");
                return Value();
            }
            // super.prototype must be null or an object -- spec: Get(superclass, "prototype")
            // result must be Object or Null; undefined (e.g. a getter-less accessor) also throws.
            Value super_proto_check = super_obj->get_property("prototype");
            if (!super_proto_check.is_null() &&
                !super_proto_check.is_object() && !super_proto_check.is_function()) {
                ctx.throw_type_error("Class extends value has invalid prototype property");
                return Value();
            }
            Function* super_fn = super_fn_check;
            if (super_fn && constructor_fn.get()) {
                constructor_fn->set_prototype(super_fn);
                constructor_fn->set_super_constructor(super_fn);

                if (proto_ptr) {
                    auto method_keys = proto_ptr->get_own_property_keys_unfiltered();
                    for (const auto& mkey : method_keys) {
                        if (mkey == "constructor") continue;
                        // get_property/get_own_property invoke the getter for accessor properties, which would spuriously execute "get m() { return super.x(); }" right now and throw.
                        PropertyDescriptor mdesc = proto_ptr->get_property_descriptor(mkey);
                        if (mdesc.is_accessor_descriptor()) {
                            if (mdesc.has_getter() && mdesc.get_getter()) {
                                static_cast<Function*>(mdesc.get_getter())->set_super_constructor(super_fn);
                            }
                            if (mdesc.has_setter() && mdesc.get_setter()) {
                                static_cast<Function*>(mdesc.get_setter())->set_super_constructor(super_fn);
                            }
                        } else if (mdesc.has_value() && mdesc.get_value().is_function()) {
                            mdesc.get_value().as_function()->set_super_constructor(super_fn);
                        }
                    }
                }

                // Static methods/getters/setters need the super constructor too, or "static get foo() { return super.bar(); }" falls back to this's [[Prototype]] instead of the real binding.
                {
                    auto static_method_keys = constructor_fn->get_own_property_keys_unfiltered();
                    for (const auto& skey : static_method_keys) {
                        if (skey == "prototype" || skey == "name" || skey == "length") continue;
                        PropertyDescriptor sdesc = constructor_fn->get_property_descriptor(skey);
                        if (sdesc.is_accessor_descriptor()) {
                            if (sdesc.has_getter() && sdesc.get_getter()) {
                                static_cast<Function*>(sdesc.get_getter())->set_super_constructor(super_fn);
                            }
                            if (sdesc.has_setter() && sdesc.get_setter()) {
                                static_cast<Function*>(sdesc.get_setter())->set_super_constructor(super_fn);
                            }
                        } else if (sdesc.has_value() && sdesc.get_value().is_function()) {
                            sdesc.get_value().as_function()->set_super_constructor(super_fn);
                        }
                    }
                }

                // Reuse the value already fetched above (super_proto_check) -- Get(superclass,
                // "prototype") must run exactly once per ClassDefinitionEvaluation, not twice.
                Value super_proto_val = super_proto_check;
                if (proto_ptr) {
                    Object* super_proto_obj = nullptr;
                    if (super_proto_val.is_object()) super_proto_obj = super_proto_val.as_object();
                    else if (super_proto_val.is_function()) super_proto_obj = super_proto_val.as_function();
                    if (super_proto_obj) proto_ptr->set_prototype(super_proto_obj);
                }
            }
        }
    }

    std::string closure_key = "__closure_" + class_name;
    std::string const_marker = "__closure_const_" + class_name; // marks binding as immutable
    Value ctor_val(constructor_fn.get());
    // Skip methods that never read the class name -- its presence alone
    // disqualifies a call from Function::call's register-mode fast path, and
    // costs an own-key scan plus three property lookups on every call.
    // references_identifier answers true for direct eval, a nested class, and
    // anything it cannot follow, so an unread binding is also an unobservable
    // one. force=true only when there is no body to inspect.
    auto mark_class_name_closure = [&](Function* fn, bool force = false) {
        if (!force && fn->get_body()) {
            bool refs = BytecodeCompiler::references_identifier(fn->get_body(), class_name);
            if (!refs) {
                for (const auto& p : fn->get_parameter_objects()) {
                    if (p->has_default() && BytecodeCompiler::references_identifier(p->get_default_value(), class_name)) { refs = true; break; }
                    if (p->has_destructuring() && BytecodeCompiler::references_identifier(p->get_destructuring_pattern(), class_name)) { refs = true; break; }
                }
            }
            if (!refs) return;
        }
        fn->set_property(closure_key, ctor_val);
        fn->set_property(const_marker, Value(true));
    };
    // [[HomeObject]]: super inside a method resolves against the object the method
    // was defined on, never against its runtime receiver. Instance methods home on
    // the prototype, static methods on the constructor.
    auto stamp_home_object = [](Function* fn, Object* home) {
        if (fn && home) fn->set_home_object(home);
    };
    if (!class_name.empty()) {
        // The constructor sees the class name as const (15.7.14 step 7) -- but
        // its body here already has the field initialisers merged in, so the
        // same reference test that covers the methods covers it too.
        mark_class_name_closure(constructor_fn.get(), /*force=*/!constructor_fn->get_body());
    }
    // The constructor is a method definition too, so `super.x` inside it (and inside
    // an arrow that closes over it) homes on the prototype. Binding it here also
    // shadows any home object an enclosing method frame left on the scope chain.
    stamp_home_object(constructor_fn.get(), proto_ptr);
    if (proto_ptr) {
        auto proto_keys = proto_ptr->get_own_property_keys_unfiltered();
        for (const auto& key : proto_keys) {
            if (key == "constructor") continue;
            PropertyDescriptor desc = proto_ptr->get_property_descriptor(key);
            if (desc.is_accessor_descriptor()) {
                if (desc.has_getter() && desc.get_getter()) {
                    mark_class_name_closure(static_cast<Function*>(desc.get_getter()));
                    stamp_home_object(static_cast<Function*>(desc.get_getter()), proto_ptr);
                }
                if (desc.has_setter() && desc.get_setter()) {
                    mark_class_name_closure(static_cast<Function*>(desc.get_setter()));
                    stamp_home_object(static_cast<Function*>(desc.get_setter()), proto_ptr);
                }
            } else {
                Value method_val = proto_ptr->get_property(key);
                if (method_val.is_function()) {
                    mark_class_name_closure(method_val.as_function());
                    stamp_home_object(method_val.as_function(), proto_ptr);
                }
            }
        }
    }
    auto static_keys = constructor_fn->get_own_property_keys_unfiltered();
    for (const auto& key : static_keys) {
        if (key == "prototype" || key == "name" || key == "length") continue;
        // Engine markers are not static methods. __closure_<name> in particular holds
        // the constructor itself, so stamping it would overwrite the constructor's
        // own home object with the constructor.
        bool is_internal_marker = key.rfind("__", 0) == 0;
        // get_property invokes the getter for accessor properties (e.g. "static get foo() { ... }"), which we must not do here.
        PropertyDescriptor desc = constructor_fn->get_property_descriptor(key);
        if (desc.is_accessor_descriptor()) {
            if (desc.has_getter() && desc.get_getter()) {
                mark_class_name_closure(static_cast<Function*>(desc.get_getter()));
                if (!is_internal_marker) stamp_home_object(static_cast<Function*>(desc.get_getter()), constructor_fn.get());
            }
            if (desc.has_setter() && desc.get_setter()) {
                mark_class_name_closure(static_cast<Function*>(desc.get_setter()));
                if (!is_internal_marker) stamp_home_object(static_cast<Function*>(desc.get_setter()), constructor_fn.get());
            }
        } else if (desc.has_value() && desc.get_value().is_function()) {
            mark_class_name_closure(desc.get_value().as_function());
            if (!is_internal_marker) stamp_home_object(desc.get_value().as_function(), constructor_fn.get());
        }
    }

    if (proto_ptr && instance_brands_raw) {
        // For derived classes only: propagate pm_brand_slot + pm_method_names to all instance methods. 
        // This lets private_brand_check enforce the invariant that private methods/getters/setters are not accessible before super() returns.
        // Base classes install the pm_brand_slot at the START of the constructor via __pfadd__, so any method call after construction always passes the check.
        // Derived classes install it AFTER super() returns, so we need the check to block access from public methods called during super() execution.
        bool is_derived = has_superclass();
        const std::string& pm_slot_for_methods = constructor_fn->pm_brand_slot();
        Value pm_names_for_methods = is_derived ? constructor_fn->get_internal_slot("__private_method_names__") : Value();
        auto propagate_private_meta = [&](Function* fn) {
            fn->set_private_brands(instance_brands_raw);
            if (is_derived && !pm_slot_for_methods.empty())
                fn->set_pm_brand_slot(pm_slot_for_methods);
            if (pm_names_for_methods.is_object())
                fn->set_internal_slot("__private_method_names__", pm_names_for_methods);
        };
        for (const auto& key : proto_ptr->get_own_property_keys_unfiltered()) {
            if (key == "constructor") continue;
            PropertyDescriptor bdesc = proto_ptr->get_property_descriptor(key);
            if (bdesc.has_getter() && bdesc.get_getter())
                propagate_private_meta(static_cast<Function*>(bdesc.get_getter()));
            if (bdesc.has_setter() && bdesc.get_setter())
                propagate_private_meta(static_cast<Function*>(bdesc.get_setter()));
            if (!bdesc.is_accessor_descriptor()) {
                Value mv = proto_ptr->get_property(key);
                if (mv.is_function())
                    propagate_private_meta(mv.as_function());
            }
        }
    }

    // The inner classScope binding becomes initialized once the constructor
    // exists -- heritage closures created above start resolving to it now.
    // The outer binding (statement form) happens after the scope pops, below.
    if (class_scope_guard.active) {
        if (Environment* env = ctx.get_lexical_environment()) {
            env->initialize_binding(class_name, Value(constructor_fn.get()));
        }
    }

    if (!static_field_initializers.empty()) {
        for (auto& sfi : static_field_initializers) {
            if (sfi->get_type() == Type::CLASS_STATIC_BLOCK) {
                ClassStaticBlock* blk = static_cast<ClassStaticBlock*>(sfi.get());
                auto static_ctx = ContextFactory::create_function_context(ctx.get_engine(), &ctx, constructor_fn.get());
                // See ContextSurvivorGuard's doc comment: a closure defined inside
                // the static block (e.g. this test's IIFE generator) captures
                // static_ctx as its closure_context_ and can outlive this scope.
                ContextSurvivorGuard survivor_guard(static_ctx, ctx.get_engine());
                static_ctx->create_binding("this", Value(constructor_fn.get()), true);
                if (blk->get_body()) eval_class_expr(blk->get_body(), *static_ctx);
                // An abrupt completion inside a static block halts all further static
                // element evaluation (spec step 34d) -- propagate it to the outer ctx
                // instead of silently discarding it with static_ctx.
                if (static_ctx->has_exception()) {
                    ctx.throw_exception(static_ctx->get_exception(), true);
                    return Value();
                }
            } else if (sfi->get_type() == Type::CLASS_FIELD) {
                ClassField* cf = static_cast<ClassField*>(sfi.get());
                std::string key_name;
                if (cf->is_computed()) {
                    Value kv = eval_class_expr(cf->get_key(), ctx);
                    if (ctx.has_exception()) break;
                    key_name = computed_key_to_property_key(ctx, kv);
                    if (ctx.has_exception()) break;
                    // The "prototype"/"constructor" restriction (spec 15.7.14) is a syntax-level
                    // early error for a literal FieldDefinition name only -- a *computed* key that
                    // happens to evaluate to either string is an ordinary property definition.
                    // "prototype" still fails at the set_property_descriptor call below (it's
                    // already non-configurable from the earlier MakeConstructor-equivalent call),
                    // which is handled generically there via CreateDataPropertyOrThrow semantics.
                } else if (Identifier* kid = dynamic_cast<Identifier*>(cf->get_key())) {
                    key_name = kid->get_name();
                } else if (StringLiteral* ks = dynamic_cast<StringLiteral*>(cf->get_key())) {
                    key_name = ks->get_value();
                } else if (NumberLiteral* kn = dynamic_cast<NumberLiteral*>(cf->get_key())) {
                    key_name = Value(kn->get_value()).to_property_key();
                }
                Value val;
                if (cf->get_value()) {
                    // Spec: static field initializers run with this = class constructor.
                    // Evaluate in outer ctx (so closures stay valid) but swap this temporarily.
                    Object* saved_this_binding = ctx.get_this_binding();
                    Value saved_this_val = ctx.get_binding("this");
                    ctx.set_this_binding(constructor_fn.get());
                    ctx.create_binding_force("this", Value(constructor_fn.get()));
                    // Push constructor onto call stack so inner class declarations inherit outer brands.
                    {
                        CallStackFrameGuard frame_guard(CallStack::instance(), nullptr, constructor_fn.get());
                        val = eval_class_expr(cf->get_value(), ctx);
                    }
                    ctx.set_this_binding(saved_this_binding);
                    ctx.create_binding_force("this", saved_this_val);
                    if (ctx.has_exception()) break;
                    // SetFunctionName (DefineField step 7): an otherwise-anonymous
                    // function/class initializer is named after the static field.
                    if (val.is_function() && is_anonymous_function_like(cf->get_value())) {
                        Function* vfn = val.as_function();
                        if (vfn->get_name().empty() || vfn->get_name() == "<arrow>") {
                            vfn->set_name(key_name);
                        }
                    }
                }
                bool is_private_static = !cf->is_computed() && !key_name.empty() && key_name[0] == '#';
                if (is_private_static) {
                    // PrivateFieldAdd on the constructor: qualified slot (same shape
                    // resolve_private_storage_key produces via the brand map), with
                    // its extensibility/duplicate TypeErrors.
                    constructor_fn->add_private_field(
                        key_name + "@" + std::to_string(reinterpret_cast<uintptr_t>(constructor_fn.get())), val);
                    if (ctx.has_exception()) break;
                } else {
                    PropertyDescriptor fdesc(val, static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable | PropertyAttributes::Enumerable));
                    // CreateDataPropertyOrThrow: a computed key equal to "prototype" fails here
                    // (that property is already non-configurable, set by MakeConstructor above)
                    // and must throw, not silently no-op.
                    if (!constructor_fn->set_property_descriptor(key_name, fdesc)) {
                        ctx.throw_type_error("Cannot define class static field '" + key_name + "'");
                        break;
                    }
                }
            }
        }
    }

    Function* constructor_ptr = constructor_fn.get();

    constructor_fn.release();

    // Leave classScope FIRST, then bind the statement form's name in the outer
    // scope (a named class expression binds its name only inside the class).
    if (class_scope_guard.active) {
        ctx.pop_block_scope();
        class_scope_guard.active = false;
    }
    if (!is_expression_) {
        if (!ctx.create_lexical_binding(class_name, Value(constructor_ptr), true)) {
            ctx.create_lexical_binding_force(class_name, Value(constructor_ptr));
        }
    }

    // Class declarations have empty completion (spec 15.7.14 step 2: NormalCompletion(empty)).
    g_empty_completion = true;
    return Value(constructor_ptr);
}

std::string ClassDeclaration::to_string() const {
    std::ostringstream oss;
    oss << "class " << id_->get_name();

    if (has_superclass()) {
        oss << " extends " << superclass_->to_string();
    }

    oss << " " << body_->to_string();
    return oss.str();
}

std::unique_ptr<ASTNode> ClassDeclaration::clone() const {
    std::unique_ptr<ASTNode> cloned_superclass = nullptr;
    if (has_superclass()) {
        cloned_superclass = superclass_->clone();
    }

    std::unique_ptr<ClassDeclaration> cloned;
    if (has_superclass()) {
        cloned = std::make_unique<ClassDeclaration>(
            std::unique_ptr<Identifier>(static_cast<Identifier*>(id_->clone().release())),
            std::move(cloned_superclass),
            std::unique_ptr<BlockStatement>(
            body_ ? static_cast<BlockStatement*>(body_->clone().release()) : nullptr),
            start_, end_
        );
    } else {
        cloned = std::make_unique<ClassDeclaration>(
            std::unique_ptr<Identifier>(static_cast<Identifier*>(id_->clone().release())),
            std::unique_ptr<BlockStatement>(
            body_ ? static_cast<BlockStatement*>(body_->clone().release()) : nullptr),
            start_, end_
        );
    }
    cloned->set_source_ref(
        ast_detail::clone_keeps_source() ? owning_unit_ : nullptr, src_start_, src_end_);
    // A literal whose body was stepped over has nothing but its range, so a
    // copy that loses it can never be read back. One that still has its body
    // does not carry the range over: a copy is made to be rewritten, and the
    // source no longer says what it holds.
    if (!body_) {
        cloned->body_tok_first_ = body_tok_first_;
        cloned->body_tok_last_ = body_tok_last_;
        cloned->body_src_first_ = body_src_first_;
        cloned->body_is_leaf_ = body_is_leaf_;
        // And the source it refers into: with no body of its own, that is the
        // whole of what this literal is. Whatever turns the copy into an
        // executable holds the unit alive from there on.
        cloned->set_source_ref(owning_unit_, src_start_, src_end_);
    }
    cloned->set_is_expression(is_expression_);
    // The name a site infers belongs to the site, so a copy of it answers the
    // same. Dropping it left an anonymous class expression nameless wherever
    // its tree was copied.
    cloned->set_inferred_name(inferred_name_);
    return cloned;
}




std::unique_ptr<ASTNode> ClassField::clone() const {
    return std::make_unique<ClassField>(
        key_->clone(),
        value_ ? value_->clone() : nullptr,
        is_static_, computed_,
        start_, end_
    );
}



std::unique_ptr<ASTNode> ClassStaticBlock::clone() const {
    return std::make_unique<ClassStaticBlock>(
        std::unique_ptr<BlockStatement>(
            body_ ? static_cast<BlockStatement*>(body_->clone().release()) : nullptr),
        start_, end_
    );
}



std::string MethodDefinition::to_string() const {
    std::ostringstream oss;

    if (is_static_) {
        oss << "static ";
    }

    if (is_constructor()) {
        oss << "constructor";
    } else if (computed_) {
        oss << "[" << key_->to_string() << "]";
    } else if (Identifier* id = dynamic_cast<Identifier*>(key_.get())) {
        oss << id->get_name();
    } else {
        oss << key_->to_string();
    }

    if (value_) {
        oss << value_->to_string();
    } else {
        oss << "{ }";
    }

    return oss.str();
}

std::unique_ptr<ASTNode> MethodDefinition::clone() const {
    return std::make_unique<MethodDefinition>(
        key_ ? key_->clone() : nullptr,
        value_ ? std::unique_ptr<FunctionExpression>(static_cast<FunctionExpression*>(value_->clone().release())) : nullptr,
        kind_, is_static_, computed_, start_, end_
    );
}





std::string FunctionExpression::to_string() const {
    std::ostringstream oss;
    oss << "function";
    if (is_named()) {
        oss << " " << id_->get_name();
    }
    oss << "(";
    for (size_t i = 0; i < params_.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << params_[i]->get_name();
    }
    // A leaf body may have been released back to its token range (see
    // defer_leaf_body), and rendering it is exactly the kind of read that must
    // not force it back. The unit still holds the text this literal came from,
    // which is what toString is supposed to report anyway.
    if (!body_) {
        if (ScriptUnit* unit = owning_unit()) {
            std::string src = unit->source_range(source_start(), source_end());
            if (!src.empty()) return src;
        }
        oss << ") { [body] }";
        return oss.str();
    }
    oss << ") " << body_->to_string();
    return oss.str();
}

std::unique_ptr<ASTNode> FunctionExpression::clone() const {
    std::vector<std::unique_ptr<Parameter>> cloned_params;
    for (const auto& param : params_) {
        cloned_params.push_back(
            std::unique_ptr<Parameter>(static_cast<Parameter*>(param->clone().release()))
        );
    }

    std::unique_ptr<Identifier> cloned_id = nullptr;
    if (is_named()) {
        cloned_id = std::unique_ptr<Identifier>(static_cast<Identifier*>(id_->clone().release()));
    }

    auto cloned = std::make_unique<FunctionExpression>(
        std::move(cloned_id),
        std::move(cloned_params),
        std::unique_ptr<BlockStatement>(
            body_ ? static_cast<BlockStatement*>(body_->clone().release()) : nullptr),
        start_, end_, is_generator_, is_async_
    );
    // The source range is recorded post-construction, so clone() must carry it
    // (and the unit it points into) explicitly or toString() loses the source.
    cloned->set_source_ref(
        ast_detail::clone_keeps_source() ? owning_unit_ : nullptr, src_start_, src_end_);
    // A literal whose body was stepped over has nothing but its range, so a
    // copy that loses it can never be read back. One that still has its body
    // does not carry the range over: a copy is made to be rewritten, and the
    // source no longer says what it holds.
    if (!body_) {
        cloned->body_tok_first_ = body_tok_first_;
        cloned->body_tok_last_ = body_tok_last_;
        cloned->body_src_first_ = body_src_first_;
        cloned->body_is_leaf_ = body_is_leaf_;
        // And the source it refers into: with no body of its own, that is the
        // whole of what this literal is. Whatever turns the copy into an
        // executable holds the unit alive from there on.
        cloned->set_source_ref(owning_unit_, src_start_, src_end_);
    }
    cloned->set_decl_form(is_decl_form_);
    cloned->set_method_shorthand(is_method_shorthand_);
    return cloned;
}





std::string ArrowFunctionExpression::to_string() const {
    std::ostringstream oss;

    if (params_.size() == 1) {
        oss << params_[0]->get_name();
    } else {
        oss << "(";
        for (size_t i = 0; i < params_.size(); ++i) {
            if (i > 0) oss << ", ";
            oss << params_[i]->get_name();
        }
        oss << ")";
    }

    oss << " => ";
    oss << body_->to_string();

    return oss.str();
}

std::unique_ptr<ASTNode> ArrowFunctionExpression::clone() const {
    std::vector<std::unique_ptr<Parameter>> cloned_params;
    for (const auto& param : params_) {
        cloned_params.push_back(
            std::unique_ptr<Parameter>(static_cast<Parameter*>(param->clone().release()))
        );
    }

    auto cloned = std::make_unique<ArrowFunctionExpression>(
        std::move(cloned_params),
        body_->clone(),
        is_async_,
        start_, end_
    );
    // The source range is recorded post-construction, so clone() must carry it
    // (and the unit it points into) explicitly or toString() loses the source.
    cloned->set_source_ref(
        ast_detail::clone_keeps_source() ? owning_unit_ : nullptr, src_start_, src_end_);
    // A literal whose body was stepped over has nothing but its range, so a
    // copy that loses it can never be read back. One that still has its body
    // does not carry the range over: a copy is made to be rewritten, and the
    // source no longer says what it holds.
    if (!body_) {
        cloned->body_tok_first_ = body_tok_first_;
        cloned->body_tok_last_ = body_tok_last_;
        cloned->body_src_first_ = body_src_first_;
        cloned->body_is_leaf_ = body_is_leaf_;
        // And the source it refers into: with no body of its own, that is the
        // whole of what this literal is. Whatever turns the copy into an
        // executable holds the unit alive from there on.
        cloned->set_source_ref(owning_unit_, src_start_, src_end_);
    }
    return cloned;
}


// The suspend half of `await`, shared by the tree-walker's
// AwaitExpression::evaluate and the compiled Op::Await so the two cannot
// drift. The operand is already evaluated; `has_argument` carries the one
// thing the value alone cannot say, since a bare `await` suspends once and
// answers undefined rather than awaiting undefined.
Value perform_await(Context& ctx, Value awaited, bool has_argument) {
    // Async generator fiber path
    AsyncGenerator* async_gen = AsyncGenerator::get_current();
    if (async_gen && async_gen->fiber_->co != nullptr) {
        Value expr_val = awaited;

        Context* gctx = async_gen->get_outer_context() ? async_gen->get_outer_context()
                                                       : async_gen->get_generator_context();

        Value settled_val;
        bool settled_throw = false;
        bool is_pending = false;
        Promise* awaited_promise = nullptr;
        Value wrapped_keepalive; // pins a freshly created wrapper promise as a GC root, if one was needed

        if (AsyncUtils::is_promise(expr_val)) {
            awaited_promise = static_cast<Promise*>(expr_val.as_object());
        } else if (AsyncUtils::is_thenable(expr_val)) {
            // Spec Await step 2 (PromiseResolve): a non-Promise thenable must be wrapped in a real Promise via NewPromiseCapability, not awaited as-is.
            auto wrapped_obj = ObjectFactory::create_promise(gctx);
            Promise* wrapped_raw = static_cast<Promise*>(wrapped_obj.get());
            auto res_fn = ObjectFactory::create_native_function("",
                [wrapped_raw](Context&, std::span<const Value> args, Value receiver) -> Value {
                    wrapped_raw->fulfill(args.empty() ? Value() : args[0]); return Value();
                }, 1);
            auto rej_fn = ObjectFactory::create_native_function("",
                [wrapped_raw](Context&, std::span<const Value> args, Value receiver) -> Value {
                    wrapped_raw->reject(args.empty() ? Value() : args[0]); return Value();
                }, 1);
            wrapped_raw->set_internal_slot("__tr_", Value(res_fn.release()));
            wrapped_raw->set_internal_slot("__tj_", Value(rej_fn.release()));
            Object* thenable_obj = expr_val.as_object();
            Value then_val = thenable_obj->get_property("then");
            if (then_val.is_function()) {
                Value r = wrapped_raw->get_internal_slot("__tr_");
                Value j = wrapped_raw->get_internal_slot("__tj_");
                AsyncUtils::call_thenable_job(gctx, then_val.as_function(), expr_val, r, j, wrapped_raw);
            }
            awaited_promise = wrapped_raw;
            wrapped_keepalive = Value(wrapped_obj.release());
        }

        if (awaited_promise) {
            if (awaited_promise->get_state() == PromiseState::FULFILLED) {
                settled_val = awaited_promise->take_settled_value();
            } else if (awaited_promise->get_state() == PromiseState::REJECTED) {
                settled_val = awaited_promise->take_settled_value();
                settled_throw = true;
            } else {
                is_pending = true;
            }
        } else {
            settled_val = expr_val;
        }

        if (is_pending) {
            auto self = async_gen;
            auto on_f = ObjectFactory::create_native_function("",
                [self, gctx](Context&, std::span<const Value> args, Value receiver) -> Value {
                    Value val = args.empty() ? Value() : args[0];
                    self->resume_from_await(val, false);
                    return Value();
                });
            auto on_r = ObjectFactory::create_native_function("",
                [self, gctx](Context&, std::span<const Value> args, Value receiver) -> Value {
                    Value reason = args.empty() ? Value() : args[0];
                    self->resume_from_await(reason, true);
                    return Value();
                });
            std::string key = std::to_string(reinterpret_cast<uintptr_t>(async_gen));
            Function* ff_tmp_ = on_f.get(); Function* fr_tmp_ = on_r.get();
            awaited_promise->set_internal_slot("__af_" + key, Value(on_f.release()));
            awaited_promise->set_internal_slot("__ar_" + key, Value(on_r.release()));
            awaited_promise->then(ff_tmp_, fr_tmp_);
        } else {
            auto self = async_gen;
            Value val = settled_val;
            bool thr = settled_throw;
            if (gctx) gctx->queue_microtask([self, val, thr]() mutable { self->resume_from_await(val, thr); }, {Value(self), val});
        }

        async_gen->await_result_ = wrapped_keepalive.is_undefined() ? expr_val : wrapped_keepalive;  // pin awaited value (or wrapper promise) as GC root during suspension
        async_gen->suspend_reason_ = AsyncGenerator::SuspendReason::Await;
        Collector::write_barrier(async_gen);
        quanta_fiber_yield(async_gen->fiber_.get());

        if (async_gen->await_is_throw_) {
            ctx.throw_exception(async_gen->await_result_, true);
            async_gen->await_is_throw_ = false;
            async_gen->await_result_ = Value();
            return Value();
        }
        Value result = async_gen->await_result_;
        async_gen->await_result_ = Value();
        return result;
    }

    AsyncExecutor* exec = AsyncExecutor::get_current();

    if (exec && exec->fiber_->co != nullptr) {
        // Fiber-based await: evaluate, suspend, resume with result
        if (!has_argument) {
            // `await` with no argument -- suspend once then return undefined
            auto self = exec->shared_from_this();
            Context* gctx = exec->engine_ ? exec->engine_->get_current_context() : exec->exec_context_;
            if (gctx) gctx->queue_microtask([self]() mutable { self->resume(Value(), false); }, {});
            quanta_fiber_yield(exec->fiber_.get());
            exec->await_result_ = Value();
            exec->await_is_throw_ = false;
            return Value();
        }

        Value expr_val = awaited;

        Context* gctx = exec->engine_ ? exec->engine_->get_current_context() : exec->exec_context_;

        // Pin immediately -- before any allocations that could trigger GC.
        static thread_local size_t aw_pin_ctr = 0;
        std::string aw_pin_key = "__ap_" + std::to_string(aw_pin_ctr++);
        exec->outer_promise_->set_internal_slot(aw_pin_key, expr_val);

        Promise* awaited_promise = nullptr;
        Value settled_val;
        bool settled_throw = false;
        bool is_pending = false;

        if (AsyncUtils::is_promise(expr_val)) {
            awaited_promise = static_cast<Promise*>(expr_val.as_object());
            if (awaited_promise->get_state() == PromiseState::FULFILLED) {
                settled_val = awaited_promise->take_settled_value();
            } else if (awaited_promise->get_state() == PromiseState::REJECTED) {
                settled_val = awaited_promise->take_settled_value();
                settled_throw = true;
            } else {
                is_pending = true;
            }
        } else if (AsyncUtils::is_thenable(expr_val)) {
            auto wrapped_obj = ObjectFactory::create_promise(gctx);
            Promise* wrapped_raw = static_cast<Promise*>(wrapped_obj.get());
            auto res_fn = ObjectFactory::create_native_function("",
                [wrapped_raw](Context&, std::span<const Value> args, Value receiver) -> Value {
                    wrapped_raw->fulfill(args.empty() ? Value() : args[0]); return Value();
                });
            auto rej_fn = ObjectFactory::create_native_function("",
                [wrapped_raw](Context&, std::span<const Value> args, Value receiver) -> Value {
                    wrapped_raw->reject(args.empty() ? Value() : args[0]); return Value();
                });
            wrapped_raw->set_internal_slot("__tr_", Value(res_fn.release()));
            wrapped_raw->set_internal_slot("__tj_", Value(rej_fn.release()));
            Object* thenable_obj = expr_val.as_object();
            Value then_val = thenable_obj->get_property("then");
            if (then_val.is_function()) {
                Value r = wrapped_raw->get_internal_slot("__tr_");
                Value j = wrapped_raw->get_internal_slot("__tj_");
                AsyncUtils::call_thenable_job(gctx, then_val.as_function(), expr_val, r, j, wrapped_raw);
            }
            awaited_promise = wrapped_raw;
            // Keep wrapped promise alive on exec until fiber resumes
            exec->await_result_ = Value(wrapped_obj.release());

            if (awaited_promise->get_state() == PromiseState::FULFILLED) {
                settled_val = awaited_promise->take_settled_value();
                exec->await_result_ = Value();
            } else if (awaited_promise->get_state() == PromiseState::REJECTED) {
                settled_val = awaited_promise->take_settled_value();
                settled_throw = true;
                exec->await_result_ = Value();
            } else {
                is_pending = true;
            }
        } else {
            settled_val = expr_val;
        }

        if (is_pending) {
            auto self = exec->shared_from_this();
            auto on_f = ObjectFactory::create_native_function("",
                [self, gctx](Context&, std::span<const Value> args, Value receiver) -> Value {
                    Value val = args.empty() ? Value() : args[0];
                    self->resume(val, false);
                    return Value();
                });
            auto on_r = ObjectFactory::create_native_function("",
                [self, gctx](Context&, std::span<const Value> args, Value receiver) -> Value {
                    Value reason = args.empty() ? Value() : args[0];
                    self->resume(reason, true);
                    return Value();
                });
            std::string key = std::to_string(reinterpret_cast<uintptr_t>(exec));
            Function* ff_tmp_ = on_f.get(); Function* fr_tmp_ = on_r.get();
            awaited_promise->set_internal_slot("__af_" + key, Value(on_f.release()));
            awaited_promise->set_internal_slot("__ar_" + key, Value(on_r.release()));
            awaited_promise->then(ff_tmp_, fr_tmp_);
        } else {
            // Pin settled_val too (e.g. module namespace object in microtask capture).
            exec->outer_promise_->set_internal_slot(aw_pin_key + "_v", settled_val);
            auto self = exec->shared_from_this();
            Value val = settled_val;
            bool thr = settled_throw;
            if (gctx) gctx->queue_microtask([self, val, thr]() mutable { self->resume(val, thr); }, {val});
        }

        quanta_fiber_yield(exec->fiber_.get());
        exec->outer_promise_->delete_internal_slot(aw_pin_key);
        exec->outer_promise_->delete_internal_slot(aw_pin_key + "_v");

        if (exec->await_is_throw_) {
            ctx.throw_exception(exec->await_result_, true);
            exec->await_is_throw_ = false;
            exec->await_result_ = Value();
            return Value();
        }
        Value result = exec->await_result_;
        exec->await_result_ = Value();
        return result;
    }

    if (!has_argument) return Value();
    Value arg_value = awaited;

    if (!arg_value.is_object()) return arg_value;
    Object* obj = arg_value.as_object();
    if (!obj) return arg_value;

    Context* gctx = ctx.get_engine() ? ctx.get_engine()->get_global_context() : &ctx;
    Promise* promise = nullptr;
    Value wrapped_keepalive;  // pins a freshly created wrapper promise as a GC root
    if (obj->get_type() == Object::ObjectType::Promise) {
        promise = static_cast<Promise*>(obj);
    } else if (AsyncUtils::is_thenable(arg_value)) {
        // PromiseResolve step 2: a non-Promise thenable is adopted by a real
        // promise, and the job that calls its `then` lands on the very queue
        // the drain below turns.
        auto wrapped_obj = ObjectFactory::create_promise(gctx);
        Promise* wrapped_raw = static_cast<Promise*>(wrapped_obj.get());
        auto res_fn = ObjectFactory::create_native_function("",
            [wrapped_raw](Context&, std::span<const Value> args, Value receiver) -> Value {
                wrapped_raw->fulfill(args.empty() ? Value() : args[0]); return Value();
            }, 1);
        auto rej_fn = ObjectFactory::create_native_function("",
            [wrapped_raw](Context&, std::span<const Value> args, Value receiver) -> Value {
                wrapped_raw->reject(args.empty() ? Value() : args[0]); return Value();
            }, 1);
        wrapped_raw->set_internal_slot("__tr_", Value(res_fn.release()));
        wrapped_raw->set_internal_slot("__tj_", Value(rej_fn.release()));
        Value then_val = obj->get_property("then");
        if (then_val.is_function()) {
            Value r = wrapped_raw->get_internal_slot("__tr_");
            Value j = wrapped_raw->get_internal_slot("__tj_");
            AsyncUtils::call_thenable_job(gctx, then_val.as_function(), arg_value, r, j, wrapped_raw);
        }
        promise = wrapped_raw;
        wrapped_keepalive = Value(wrapped_obj.release());
    }
    if (!promise) return arg_value;

    if (promise->get_state() == PromiseState::PENDING) {
        // Quanta has no suspension mechanism for top-level await in modules
        // (no fiber executor here) -- force the promise to settle by draining
        // the microtask queue, bounded against one that never settles.
        int spins = 0;
        while (promise->get_state() == PromiseState::PENDING && spins < 100000) {
            if (!gctx || !gctx->has_pending_microtasks()) break;
            gctx->drain_microtasks();
            spins++;
        }
    }
    if (promise->get_state() == PromiseState::FULFILLED) {
        return promise->take_settled_value();
    }
    if (promise->get_state() == PromiseState::REJECTED) {
        ctx.throw_exception(promise->take_settled_value(), true);
        return Value();
    }
    return arg_value;
}



std::string AwaitExpression::to_string() const {
    return "await " + argument_->to_string();
}

std::unique_ptr<ASTNode> AwaitExpression::clone() const {
    return std::make_unique<AwaitExpression>(
        argument_->clone(),
        start_, end_
    );
}


// The suspend half of a plain `yield`, shared by the tree-walker's
// YieldExpression::evaluate and the compiled Op::Yield so the two cannot
// drift -- the same reason object_spread_into and append_spread_values are
// each one function. The value is already evaluated; what is left is the
// fiber switch and what the resumption asks for.
Value perform_yield(Context& ctx, Value yield_value) {
    Generator* current_gen = Generator::get_current_generator();
    if (!current_gen) {
        if (AsyncGenerator::get_current()) {
            AsyncGenerator* async_gen = AsyncGenerator::get_current();

            // AsyncGeneratorYield step 5: Await(value) unconditionally, even a plain value (1 tick).
            {
                Context* gctx = async_gen->get_outer_context() ? async_gen->get_outer_context()
                                                               : async_gen->get_generator_context();
                Promise* p;
                Value wrapped_keepalive; // pins a freshly created wrapper promise as a GC root, if one was needed
                if (AsyncUtils::is_promise(yield_value)) {
                    p = static_cast<Promise*>(yield_value.as_object());
                } else {
                    // Get(yield_value, "then") must be read exactly once -- a side-effecting
                    // getter must not be observed firing twice.
                    Value then_val;
                    if (yield_value.is_object()) then_val = yield_value.as_object()->get_property("then");
                    if (then_val.is_function()) {
                        auto wrapped_obj = ObjectFactory::create_promise(gctx);
                        Promise* wrapped_raw = static_cast<Promise*>(wrapped_obj.get());
                        auto res_fn = ObjectFactory::create_native_function("",
                            [wrapped_raw](Context&, std::span<const Value> args, Value receiver) -> Value {
                                wrapped_raw->fulfill(args.empty() ? Value() : args[0]); return Value();
                            }, 1);
                        auto rej_fn = ObjectFactory::create_native_function("",
                            [wrapped_raw](Context&, std::span<const Value> args, Value receiver) -> Value {
                                wrapped_raw->reject(args.empty() ? Value() : args[0]); return Value();
                            }, 1);
                        wrapped_raw->set_internal_slot("__tr_", Value(res_fn.release()));
                        wrapped_raw->set_internal_slot("__tj_", Value(rej_fn.release()));
                        Value r = wrapped_raw->get_internal_slot("__tr_");
                        Value j = wrapped_raw->get_internal_slot("__tj_");
                        AsyncUtils::call_thenable_job(gctx, then_val.as_function(), yield_value, r, j, wrapped_raw);
                        p = wrapped_raw;
                        wrapped_keepalive = Value(wrapped_obj.release());
                    } else {
                        auto wrapped_obj = ObjectFactory::create_promise(gctx);
                        Promise* wrapped_raw = static_cast<Promise*>(wrapped_obj.get());
                        wrapped_raw->fulfill(yield_value);
                        p = wrapped_raw;
                        wrapped_keepalive = Value(wrapped_obj.release());
                    }
                }
                if (p->get_state() == PromiseState::FULFILLED || p->get_state() == PromiseState::REJECTED) {
                    // Await always costs a tick, even for an already-settled promise --
                    // PerformPromiseThen never resolves its reaction synchronously, so a
                    // shortcut straight through here would skip the mandatory suspension.
                    bool was_rejected = (p->get_state() == PromiseState::REJECTED);
                    Value settled = p->take_settled_value();
                    if (gctx) gctx->queue_microtask([async_gen, settled, was_rejected]() mutable {
                        async_gen->resume_from_await(settled, was_rejected);
                    }, {Value(async_gen), settled});
                    async_gen->await_result_ = wrapped_keepalive.is_undefined() ? yield_value : wrapped_keepalive;
                    async_gen->suspend_reason_ = AsyncGenerator::SuspendReason::Await;
                    Collector::write_barrier(async_gen);
                    quanta_fiber_yield(async_gen->fiber_.get());
                    if (async_gen->await_is_throw_) {
                        ctx.throw_exception(async_gen->await_result_, true);
                        async_gen->await_is_throw_ = false;
                        async_gen->await_result_ = Value();
                        return Value();
                    }
                    yield_value = async_gen->await_result_;
                    async_gen->await_result_ = Value();
                } else {
                    auto on_f = ObjectFactory::create_native_function("",
                        [async_gen, gctx](Context&, std::span<const Value> args, Value receiver) -> Value {
                            Value val = args.empty() ? Value() : args[0];
                            async_gen->resume_from_await(val, false);
                            return Value();
                        });
                    auto on_r = ObjectFactory::create_native_function("",
                        [async_gen, gctx](Context&, std::span<const Value> args, Value receiver) -> Value {
                            Value reason = args.empty() ? Value() : args[0];
                            async_gen->resume_from_await(reason, true);
                            return Value();
                        });
                    std::string aw_key = "yw_" + std::to_string(reinterpret_cast<uintptr_t>(async_gen));
                    Function* ff_tmp = on_f.get(); Function* fr_tmp = on_r.get();
                    p->set_internal_slot("__af_" + aw_key, Value(on_f.release()));
                    p->set_internal_slot("__ar_" + aw_key, Value(on_r.release()));
                    p->then(ff_tmp, fr_tmp);
                    async_gen->await_result_ = wrapped_keepalive.is_undefined() ? yield_value : wrapped_keepalive;
                    async_gen->suspend_reason_ = AsyncGenerator::SuspendReason::Await;
                    Collector::write_barrier(async_gen);
                    quanta_fiber_yield(async_gen->fiber_.get());
                    if (async_gen->await_is_throw_) {
                        ctx.throw_exception(async_gen->await_result_, true);
                        async_gen->await_is_throw_ = false;
                        async_gen->await_result_ = Value();
                        return Value();
                    }
                    yield_value = async_gen->await_result_;
                    async_gen->await_result_ = Value();
                }
            }

            async_gen->yield_value_     = yield_value;
            async_gen->suspend_reason_  = AsyncGenerator::SuspendReason::Yield;
            Collector::write_barrier(async_gen);
            quanta_fiber_yield(async_gen->fiber_.get());
            // Resumed by next()/return()/throw()
            if (async_gen->returning_) {
                async_gen->returning_ = false;
                // 25.5.3.7 step 8.b: the resumption value is itself Awaited before completing.
                Value awaited_ret;
                bool ret_threw = await_value(ctx, async_gen->return_arg_, awaited_ret);
                if (ret_threw) { ctx.throw_exception(awaited_ret, true); return Value(); }
                throw GeneratorReturnException(awaited_ret);
            }
            if (async_gen->throwing_) {
                async_gen->throwing_ = false;
                ctx.throw_exception(async_gen->sent_value_, true);
                return Value();
            }
            return async_gen->sent_value_;
        }
        return yield_value;
    }

    // Fiber-based yield: actually suspend and switch back to caller
    current_gen->yielded_value_ = yield_value;
    current_gen->set_state(Generator::State::SuspendedYield);
    Collector::write_barrier(current_gen);
    quanta_fiber_yield(&current_gen->fiber_);

    // Resumed by next()/throw()/return()
    if (current_gen->returning_) {
        current_gen->returning_ = false;
        throw GeneratorReturnException(current_gen->return_argument_);
    }
    if (current_gen->throwing_) {
        current_gen->throwing_ = false;
        ctx.throw_exception(current_gen->throw_value_, true);
        return Value();
    }
    return current_gen->sent_value_;
}

// The `yield*` protocol: drive another iterator, forwarding next/throw/return
// into it and every result back out, until it reports done. Shared by
// YieldExpression::evaluate and Op::YieldStar so the two cannot drift. The
// operand is already evaluated.
Value perform_yield_delegate(Context& ctx, Value iterable) {
    Generator* current_gen = Generator::get_current_generator();


    // In async generator context: iterate and throw/yield each value
    if (!current_gen) {
        AsyncGenerator* async_gen = AsyncGenerator::get_current();
        if (!async_gen) return Value();

        // Resolve the iterable -- get iterator
        Object* iterable_obj = nullptr;
        if (iterable.is_object()) iterable_obj = iterable.as_object();
        else if (iterable.is_function()) iterable_obj = iterable.as_function();

        if (!iterable_obj) { ctx.throw_type_error("yield* requires iterable"); return Value(); }

        // Set current_context_ so getters use the right context for exceptions
        Context* prev_ctx = Object::current_context_;
        Object::current_context_ = &ctx;

        // Try Symbol.asyncIterator first, then Symbol.iterator
        // Getter access itself may throw (abrupt completion on GetIterator)
        Value iter_val;
        bool used_async_iterator = false;
        Symbol* async_iter_sym = Symbol::get_well_known(Symbol::ASYNC_ITERATOR);
        if (async_iter_sym) {
            Value iter_fn = iterable_obj->get_property(async_iter_sym->to_property_key());
            if (ctx.has_exception()) { Object::current_context_ = prev_ctx; return Value(); }
            bool async_iter_defined = false;
            if (!iter_fn.is_undefined() && !iter_fn.is_null()) {
                async_iter_defined = true;
                used_async_iterator = true;
                if (!iter_fn.is_function()) {
                    Object::current_context_ = prev_ctx;
                    ctx.throw_type_error("[Symbol.asyncIterator] is not callable");
                    return Value();
                }
                iter_val = iter_fn.as_function()->call(ctx, {}, iterable);
                if (ctx.has_exception()) { Object::current_context_ = prev_ctx; return Value(); }
                // If asyncIterator() returned non-object, throw TypeError immediately (don't fall back)
                if (!iter_val.is_undefined() && !iter_val.is_object() && !iter_val.is_function()) {
                    Object::current_context_ = prev_ctx;
                    ctx.throw_type_error("[Symbol.asyncIterator]() returned a non-object");
                    return Value();
                }
                if (iter_val.is_undefined()) {
                    Object::current_context_ = prev_ctx;
                    ctx.throw_type_error("[Symbol.asyncIterator]() returned undefined");
                    return Value();
                }
            }
        }
        if (iter_val.is_undefined() && !ctx.has_exception()) {
            Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
            if (iter_sym) {
                Value iter_fn = iterable_obj->get_property(iter_sym->to_property_key());
                if (ctx.has_exception()) { Object::current_context_ = prev_ctx; return Value(); }
                if (iter_fn.is_function()) {
                    iter_val = iter_fn.as_function()->call(ctx, {}, iterable);
                    if (ctx.has_exception()) { Object::current_context_ = prev_ctx; return Value(); }
                }
            }
        }
        Object::current_context_ = prev_ctx;
        if (iter_val.is_undefined()) {
            ctx.throw_type_error("yield* requires iterable with [Symbol.asyncIterator]");
            return Value();
        }
        // Iterator result must be an object
        if (!iter_val.is_object() && !iter_val.is_function()) {
            ctx.throw_type_error("[Symbol.asyncIterator]() returned a non-object iterator");
            return Value();
        }

        Object* iter_obj = iter_val.is_object() ? iter_val.as_object() : iter_val.as_function();
        if (!iter_obj) { ctx.throw_type_error("iterator is not an object"); return Value(); }

        Object::current_context_ = &ctx;
        Value next_fn_val = iter_obj->get_property("next");
        Object::current_context_ = prev_ctx;
        if (ctx.has_exception()) return Value();
        if (!next_fn_val.is_function()) { ctx.throw_type_error("iterator missing next()"); return Value(); }

        // Fiber-based yield* in async generator: drive the inner iterator,
        // yield each value (with await on each next() result), return final value.
        Context* gctx = async_gen->get_outer_context() ? async_gen->get_outer_context()
                                                       : async_gen->get_generator_context();

        // AsyncGeneratorYield step 5: Await(value) unconditionally, even a plain value (1 tick).
        auto await_before_yield = [&](Value& v) -> bool {
            Promise* p;
            Value wrapped_keepalive;
            if (AsyncUtils::is_promise(v)) {
                p = static_cast<Promise*>(v.as_object());
            } else {
                // Get(v, "then") must be read exactly once -- a getter with side effects
                // (as several test262 cases use) must not be observed firing twice.
                Value then_val;
                if (v.is_object()) then_val = v.as_object()->get_property("then");
                if (then_val.is_function()) {
                    auto wrapped_obj = ObjectFactory::create_promise(gctx);
                    Promise* wrapped_raw = static_cast<Promise*>(wrapped_obj.get());
                    auto res_fn = ObjectFactory::create_native_function("",
                        [wrapped_raw](Context&, std::span<const Value> args, Value receiver) -> Value {
                            wrapped_raw->fulfill(args.empty() ? Value() : args[0]); return Value();
                        }, 1);
                    auto rej_fn = ObjectFactory::create_native_function("",
                        [wrapped_raw](Context&, std::span<const Value> args, Value receiver) -> Value {
                            wrapped_raw->reject(args.empty() ? Value() : args[0]); return Value();
                        }, 1);
                    wrapped_raw->set_internal_slot("__tr_", Value(res_fn.release()));
                    wrapped_raw->set_internal_slot("__tj_", Value(rej_fn.release()));
                    Value r = wrapped_raw->get_internal_slot("__tr_");
                    Value j = wrapped_raw->get_internal_slot("__tj_");
                    AsyncUtils::call_thenable_job(gctx, then_val.as_function(), v, r, j, wrapped_raw);
                    p = wrapped_raw;
                    wrapped_keepalive = Value(wrapped_obj.release());
                } else {
                    auto wrapped_obj = ObjectFactory::create_promise(gctx);
                    Promise* wrapped_raw = static_cast<Promise*>(wrapped_obj.get());
                    wrapped_raw->fulfill(v);
                    p = wrapped_raw;
                    wrapped_keepalive = Value(wrapped_obj.release());
                }
            }
            if (p->get_state() == PromiseState::FULFILLED || p->get_state() == PromiseState::REJECTED) {
                // Await always costs a tick, even for an already-settled promise.
                bool was_rejected = (p->get_state() == PromiseState::REJECTED);
                Value settled = p->take_settled_value();
                if (gctx) gctx->queue_microtask([async_gen, settled, was_rejected]() mutable {
                    async_gen->resume_from_await(settled, was_rejected);
                }, {Value(async_gen), settled});
                async_gen->await_result_ = wrapped_keepalive.is_undefined() ? v : wrapped_keepalive;
                async_gen->suspend_reason_ = AsyncGenerator::SuspendReason::Await;
                Collector::write_barrier(async_gen);
                quanta_fiber_yield(async_gen->fiber_.get());
                if (async_gen->await_is_throw_) {
                    ctx.throw_exception(async_gen->await_result_, true);
                    async_gen->await_is_throw_ = false;
                    async_gen->await_result_ = Value();
                    return false;
                }
                v = async_gen->await_result_;
                async_gen->await_result_ = Value();
                return true;
            }
            auto on_f = ObjectFactory::create_native_function("",
                [async_gen, gctx](Context&, std::span<const Value> args, Value receiver) -> Value {
                    Value val = args.empty() ? Value() : args[0];
                    async_gen->resume_from_await(val, false);
                    return Value();
                });
            auto on_r = ObjectFactory::create_native_function("",
                [async_gen, gctx](Context&, std::span<const Value> args, Value receiver) -> Value {
                    Value reason = args.empty() ? Value() : args[0];
                    async_gen->resume_from_await(reason, true);
                    return Value();
                });
            std::string aw_key = "ydv_" + std::to_string(reinterpret_cast<uintptr_t>(async_gen));
            Function* ff_tmp = on_f.get(); Function* fr_tmp = on_r.get();
            p->set_internal_slot("__af_" + aw_key, Value(on_f.release()));
            p->set_internal_slot("__ar_" + aw_key, Value(on_r.release()));
            p->then(ff_tmp, fr_tmp);
            async_gen->await_result_ = wrapped_keepalive.is_undefined() ? v : wrapped_keepalive;
            async_gen->suspend_reason_ = AsyncGenerator::SuspendReason::Await;
            Collector::write_barrier(async_gen);
            quanta_fiber_yield(async_gen->fiber_.get());
            if (async_gen->await_is_throw_) {
                ctx.throw_exception(async_gen->await_result_, true);
                async_gen->await_is_throw_ = false;
                async_gen->await_result_ = Value();
                return false;
            }
            v = async_gen->await_result_;
            async_gen->await_result_ = Value();
            return true;
        };

        Value last_val;
        bool delegate_done = false;
        bool first_iter = true;
        while (!delegate_done) {
            Value nr;

            // Spec 27.6.3.9: forward throw()/next(sentValue) to the inner iterator.
            if (!first_iter && async_gen->throwing_) {
                async_gen->throwing_ = false;
                Context* prev_oc_t = Object::current_context_;
                Object::current_context_ = &ctx;
                Value throw_fn_v = iter_obj->get_property("throw");
                Object::current_context_ = prev_oc_t;
                if (ctx.has_exception()) return Value();
                if (throw_fn_v.is_function()) {
                    nr = throw_fn_v.as_function()->call(ctx, {async_gen->sent_value_}, iter_val);
                } else {
                    // No throw method -- IteratorClose first; a failure while closing
                    // (return getter or call throwing) wins over the pending TypeError.
                    Object::current_context_ = &ctx;
                    Value ret_fn_v = iter_obj->get_property("return");
                    Object::current_context_ = prev_oc_t;
                    if (ctx.has_exception()) return Value();
                    if (ret_fn_v.is_function()) {
                        ret_fn_v.as_function()->call(ctx, {}, iter_val);
                        if (ctx.has_exception()) return Value();
                    }
                    ctx.throw_type_error("The iterator does not have a 'throw' method");
                    return Value();
                }
            } else {
                // IteratorNext(iterator, received.[[Value]]): always pass value, undefined on first
                Value call_arg = first_iter ? Value() : async_gen->sent_value_;
                nr = next_fn_val.as_function()->call(ctx, {call_arg}, iter_val);
            }
            first_iter = false;

            if (ctx.has_exception()) return Value();

            // innerResult itself is Awaited unconditionally (27.6.3.9), separately from
            // and before the "Await(value) before yield" step further below.
            if (!await_before_yield(nr)) return Value();

            if (!nr.is_object()) { ctx.throw_type_error("iterator result is not an object"); return Value(); }
            {
                Context* prev_oc = Object::current_context_;
                Object::current_context_ = &ctx;
                Value done_val_tmp = nr.as_object()->get_property("done");
                Object::current_context_ = prev_oc;
                if (ctx.has_exception()) return Value();
                last_val = [&]() -> Value {
                    Context* p2 = Object::current_context_;
                    Object::current_context_ = &ctx;
                    Value v = nr.as_object()->get_property("value");
                    Object::current_context_ = p2;
                    return v;
                }();
                if (ctx.has_exception()) return Value();
                if (done_val_tmp.to_boolean()) { delegate_done = true; break; }
            }

            if (!used_async_iterator) {
                // AsyncFromSyncIteratorContinuation with closeOnRejection: PromiseResolve
                // reads value.constructor, and a rejected value closes the sync iterator
                // before the rejection propagates. Close failures are swallowed.
                bool value_threw = false;
                if (AsyncUtils::is_promise(last_val)) {
                    Context* prev_oc_v = Object::current_context_;
                    Object::current_context_ = &ctx;
                    last_val.as_object()->get_property("constructor");
                    Object::current_context_ = prev_oc_v;
                    value_threw = ctx.has_exception();
                }
                if (!value_threw && !await_before_yield(last_val)) value_threw = true;
                if (value_threw) {
                    Value original_err = ctx.get_exception();
                    ctx.clear_exception();
                    Context* prev_oc_c = Object::current_context_;
                    Object::current_context_ = &ctx;
                    Value close_fn = iter_obj->get_property("return");
                    if (!ctx.has_exception() && close_fn.is_function()) {
                        close_fn.as_function()->call(ctx, {}, iter_val);
                    }
                    Object::current_context_ = prev_oc_c;
                    ctx.clear_exception();
                    ctx.throw_exception(original_err, true);
                    return Value();
                }
            }
            async_gen->yield_value_    = last_val;
            async_gen->suspend_reason_ = AsyncGenerator::SuspendReason::Yield;
            Collector::write_barrier(async_gen);
            quanta_fiber_yield(async_gen->fiber_.get());
            // AsyncGeneratorUnwrapYieldResumption step 2: a `return` resumption value is itself
            // Awaited before this completion reaches the Repeat loop's return-handling below.
            if (async_gen->returning_) {
                Value awaited_resume;
                bool resume_threw = await_value(ctx, async_gen->return_arg_, awaited_resume);
                if (resume_threw) { ctx.throw_exception(awaited_resume, true); return Value(); }
                async_gen->return_arg_ = awaited_resume;
            }
            // `while` not `if`: a !done forwarded result suspends again, and a repeat
            // iter.return() before the next resumption must be forwarded too.
            while (async_gen->returning_) {
                async_gen->returning_ = false;
                Value ret_arg = async_gen->return_arg_;
                Context* prev_oc_r = Object::current_context_;
                Object::current_context_ = &ctx;
                Value ret_fn_v = iter_obj->get_property("return");
                Object::current_context_ = prev_oc_r;
                if (ctx.has_exception()) return Value();
                if (!ret_fn_v.is_function()) {
                    // No return method: Await the arg, then terminate the whole generator.
                    Value awaited_ret_arg;
                    bool ret_arg_threw = await_value(ctx, ret_arg, awaited_ret_arg);
                    if (ret_arg_threw) { ctx.throw_exception(awaited_ret_arg, true); return Value(); }
                    throw GeneratorReturnException(awaited_ret_arg);
                }
                Value ret_result = ret_fn_v.as_function()->call(ctx, {ret_arg}, iter_val);
                if (ctx.has_exception()) return Value();
                // Await the return() result
                if (AsyncUtils::is_promise(ret_result)) {
                    Promise* rp = static_cast<Promise*>(ret_result.as_object());
                    if (rp->get_state() == PromiseState::FULFILLED || rp->get_state() == PromiseState::REJECTED) {
                        // Await always costs a tick, even for an already-settled promise.
                        bool was_rejected = (rp->get_state() == PromiseState::REJECTED);
                        Value settled = rp->take_settled_value();
                        if (gctx) gctx->queue_microtask([async_gen, settled, was_rejected]() mutable {
                            async_gen->resume_from_await(settled, was_rejected);
                        }, {Value(async_gen), settled});
                        async_gen->await_result_ = ret_result;
                        async_gen->suspend_reason_ = AsyncGenerator::SuspendReason::Await;
                        Collector::write_barrier(async_gen);
                        quanta_fiber_yield(async_gen->fiber_.get());
                        if (async_gen->await_is_throw_) {
                            ctx.throw_exception(async_gen->await_result_, true);
                            async_gen->await_is_throw_ = false;
                            async_gen->await_result_ = Value();
                            return Value();
                        }
                        ret_result = async_gen->await_result_;
                        async_gen->await_result_ = Value();
                    } else {
                        auto on_f2 = ObjectFactory::create_native_function("",
                            [async_gen, gctx](Context&, std::span<const Value> args, Value receiver) -> Value {
                                Value val = args.empty() ? Value() : args[0];
                                async_gen->resume_from_await(val, false);
                                return Value();
                            });
                        auto on_r2 = ObjectFactory::create_native_function("",
                            [async_gen, gctx](Context&, std::span<const Value> args, Value receiver) -> Value {
                                Value reason = args.empty() ? Value() : args[0];
                                async_gen->resume_from_await(reason, true);
                                return Value();
                            });
                        std::string rkey = "yr_" + std::to_string(reinterpret_cast<uintptr_t>(async_gen));
                        Function* frf = on_f2.get(); Function* frr = on_r2.get();
                        rp->set_internal_slot("__af_" + rkey, Value(on_f2.release()));
                        rp->set_internal_slot("__ar_" + rkey, Value(on_r2.release()));
                        rp->then(frf, frr);
                        async_gen->await_result_ = ret_result;
                        async_gen->suspend_reason_ = AsyncGenerator::SuspendReason::Await;
                        Collector::write_barrier(async_gen);
                        quanta_fiber_yield(async_gen->fiber_.get());
                        if (async_gen->await_is_throw_) {
                            ctx.throw_exception(async_gen->await_result_, true);
                            async_gen->await_is_throw_ = false;
                            async_gen->await_result_ = Value();
                            return Value();
                        }
                        ret_result = async_gen->await_result_;
                        async_gen->await_result_ = Value();
                    }
                } else if (ret_result.is_object()) {
                    // Custom thenable: get 'then' (may throw), call then(resolve, reject)
                    Context* prev_oc3 = Object::current_context_;
                    Object::current_context_ = &ctx;
                    Value rr_then = ret_result.as_object()->get_property("then");
                    Object::current_context_ = prev_oc3;
                    if (ctx.has_exception()) return Value();
                    if (rr_then.is_function()) {
                        auto on_f3 = ObjectFactory::create_native_function("",
                            [async_gen, gctx](Context&, std::span<const Value> args, Value receiver) -> Value {
                                Value val = args.empty() ? Value() : args[0];
                                async_gen->resume_from_await(val, false);
                                return Value();
                            });
                        auto on_r3 = ObjectFactory::create_native_function("",
                            [async_gen, gctx](Context&, std::span<const Value> args, Value receiver) -> Value {
                                Value reason = args.empty() ? Value() : args[0];
                                async_gen->resume_from_await(reason, true);
                                return Value();
                            });
                        Function* rf3 = on_f3.get(); Function* rjf3 = on_r3.get();
                        ret_result.as_object()->set_internal_slot("__th_rf3_", Value(on_f3.release()));
                        ret_result.as_object()->set_internal_slot("__th_rjf3_", Value(on_r3.release()));
                        // NewPromiseResolveThenableJob: `then` is called in its own queued
                        // microtask (on the global context, for chronological ordering
                        // against unrelated Promise chains), not synchronously.
                        if (gctx) {
                            Function* then_fn3 = rr_then.as_function();
                            Value ret_result_capture = ret_result;
                            Context* queue_ctx_tn3 = ctx.get_engine() && ctx.get_engine()->get_global_context()
                                ? ctx.get_engine()->get_global_context() : gctx;
                            queue_ctx_tn3->queue_microtask([gctx, then_fn3, ret_result_capture, rf3, rjf3]() {
                                then_fn3->call(*gctx, {Value(rf3), Value(rjf3)}, ret_result_capture);
                                if (gctx->has_exception()) {
                                    Value exc = gctx->get_exception();
                                    gctx->clear_exception();
                                    rjf3->call(*gctx, {exc});
                                }
                            }, {Value(then_fn3), ret_result_capture, Value(rf3), Value(rjf3)});
                        }
                        async_gen->await_result_ = ret_result;
                        async_gen->suspend_reason_ = AsyncGenerator::SuspendReason::Await;
                        Collector::write_barrier(async_gen);
                        quanta_fiber_yield(async_gen->fiber_.get());
                        if (async_gen->await_is_throw_) {
                            ctx.throw_exception(async_gen->await_result_, true);
                            async_gen->await_is_throw_ = false;
                            async_gen->await_result_ = Value();
                            return Value();
                        }
                        ret_result = async_gen->await_result_;
                        async_gen->await_result_ = Value();
                    }
                }
                if (!ret_result.is_object()) { ctx.throw_type_error("iterator return result is not an object"); return Value(); }
                {
                    Context* prev_oc_rd = Object::current_context_;
                    Object::current_context_ = &ctx;
                    Value ret_done = ret_result.as_object()->get_property("done");
                    Object::current_context_ = prev_oc_rd;
                    if (ctx.has_exception()) return Value();
                    Object::current_context_ = &ctx;
                    last_val = ret_result.as_object()->get_property("value");
                    Object::current_context_ = prev_oc_rd;
                    if (ctx.has_exception()) return Value();
                    if (ret_done.to_boolean()) {
                        // Spec 27.6.3.9 step 8.b.iv.viii: a Return completion, not a
                        // normal yield* result -- terminates the outer generator here.
                        if (!await_before_yield(last_val)) return Value();
                        throw GeneratorReturnException(last_val);
                    }
                }
                // Not done: yield and suspend -- the `while` condition re-forwards
                // on resume if the consumer called iter.return() again.
                if (!used_async_iterator && !await_before_yield(last_val)) return Value();
                async_gen->yield_value_ = last_val;
                async_gen->suspend_reason_ = AsyncGenerator::SuspendReason::Yield;
                Collector::write_barrier(async_gen);
                quanta_fiber_yield(async_gen->fiber_.get());
            }
            // throwing_ is handled at the top of the next iteration
        }
        return last_val;
    }

    // Fiber-based generators: yield* directly swaps context for each element
    // (target_yield_index_ stays 0 since fiber doesn't use replay)
    if (current_gen->fiber_.co != nullptr) {
        // Set current_context_ so accessor getters (poisoned properties in tests) can execute.
        Context* prev_ctx = Object::current_context_;
        Object::current_context_ = &ctx;

        // Get iterator from iterable
        Value iter_val;
        if (iterable.is_string()) {
            // Box string to call Symbol.iterator
            auto boxed = ObjectFactory::create_object();
            boxed->set_property("length", Value(0.0));
            Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
            if (iter_sym) {
                Value str_ctor = ctx.get_binding("String");
                if (str_ctor.is_function()) {
                    Value proto = str_ctor.as_function()->get_property("prototype");
                    if (proto.is_object()) {
                        Value iter_fn = proto.as_object()->get_property(iter_sym->to_property_key());
                        if (iter_fn.is_function()) {
                            iter_val = iter_fn.as_function()->call(ctx, {}, iterable);
                            if (ctx.has_exception()) { Object::current_context_ = prev_ctx; return Value(); }
                        }
                    }
                }
            }
        } else if (iterable.is_boolean() || iterable.is_number()) {
            // Box to call a custom Symbol.iterator on Boolean.prototype/Number.prototype
            // (GetIterator works on any value, not just objects -- ToObject boxes first).
            std::string ctor_name = iterable.is_boolean() ? "Boolean" : "Number";
            Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
            if (iter_sym) {
                Value ctor = ctx.get_binding(ctor_name);
                if (ctor.is_function()) {
                    Value proto = ctor.as_function()->get_property("prototype");
                    if (proto.is_object()) {
                        Value iter_fn = proto.as_object()->get_property(iter_sym->to_property_key());
                        if (iter_fn.is_function()) {
                            iter_val = iter_fn.as_function()->call(ctx, {}, iterable);
                            if (ctx.has_exception()) { Object::current_context_ = prev_ctx; return Value(); }
                        }
                    }
                }
            }
        } else if (iterable.is_object() || iterable.is_function()) {
            Object* itbl = iterable.is_object() ? iterable.as_object() : iterable.as_function();
            Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
            if (iter_sym) {
                Value iter_fn = itbl->get_property(iter_sym->to_property_key());
                if (ctx.has_exception()) { Object::current_context_ = prev_ctx; return Value(); }
                if (iter_fn.is_function()) {
                    iter_val = iter_fn.as_function()->call(ctx, {}, iterable);
                    if (ctx.has_exception()) { Object::current_context_ = prev_ctx; return Value(); }
                }
            }
            if (iter_val.is_undefined() || iter_val.is_null()) iter_val = iterable;
        } else {
            iter_val = iterable;
        }
        Object* iter_obj = iter_val.is_object() ? iter_val.as_object()
            : (iter_val.is_function() ? iter_val.as_function() : nullptr);
        if (!iter_obj) { Object::current_context_ = prev_ctx; ctx.throw_type_error("yield* requires iterable"); return Value(); }

        Value next_fn = iter_obj->get_property("next");
        if (ctx.has_exception()) { Object::current_context_ = prev_ctx; return Value(); }
        if (!next_fn.is_function()) { Object::current_context_ = prev_ctx; ctx.throw_type_error("yield* iterator missing next()"); return Value(); }

        Value final_val;
        Value next_arg; // undefined for first call; updated to sent_value_ after each resume
        while (true) {
            Value result = next_fn.as_function()->call(ctx, {next_arg}, iter_val);
            if (ctx.has_exception()) { Object::current_context_ = prev_ctx; return Value(); }
            if (!result.is_object()) {
                Object::current_context_ = prev_ctx;
                ctx.throw_type_error("Iterator result is not an object");
                return Value();
            }
            Value done = result.as_object()->get_property("done");
            if (ctx.has_exception()) { Object::current_context_ = prev_ctx; return Value(); }
            if (done.to_boolean()) {
                // done=true: access value to return from yield* expression
                Value val = result.as_object()->get_property("value");
                if (ctx.has_exception()) { Object::current_context_ = prev_ctx; return Value(); }
                final_val = val;
                break;
            }
            // done=false: yield the WHOLE inner result object without accessing value (spec 14.4.14)
            current_gen->yielded_result_ = result;
            current_gen->yield_raw_result_ = true;
            current_gen->set_state(Generator::State::SuspendedYield);
            // Direct-assigned traced field: re-gray for an open incremental cycle.
            Collector::write_barrier(current_gen);
            quanta_fiber_yield(&current_gen->fiber_);
            // Caller side may have repointed this thread-local during suspension -- restore it.
            Object::current_context_ = &ctx;
            // Resumed -- forward the value sent to outer generator into inner next()
            next_arg = current_gen->sent_value_;
            // `while` not `if`: each subsequent .return()/.throw() on the outer generator
            // must delegate again until the inner iterator reports done=true (Repeat).
            bool delegate_done = false;
            while (current_gen->returning_ || current_gen->throwing_) {
                bool is_return = current_gen->returning_;
                current_gen->returning_ = false;
                current_gen->throwing_ = false;
                Value deleg_fn = iter_obj->get_property(is_return ? "return" : "throw");
                if (ctx.has_exception()) { Object::current_context_ = prev_ctx; return Value(); }
                if (!deleg_fn.is_function()) {
                    if (is_return) {
                        // No return method: propagate the return with original argument.
                        Object::current_context_ = prev_ctx;
                        throw GeneratorReturnException(current_gen->return_argument_);
                    }
                    // No throw method: IteratorClose, then throw TypeError.
                    Value close_fn = iter_obj->get_property("return");
                    if (ctx.has_exception()) { Object::current_context_ = prev_ctx; return Value(); }
                    if (close_fn.is_function()) {
                        close_fn.as_function()->call(ctx, {}, iter_val);
                        if (ctx.has_exception()) { Object::current_context_ = prev_ctx; return Value(); }
                    }
                    Object::current_context_ = prev_ctx;
                    ctx.throw_type_error("The iterator does not provide a throw method");
                    return Value();
                }
                Value deleg_arg = is_return ? current_gen->return_argument_ : current_gen->throw_value_;
                Value deleg_result = deleg_fn.as_function()->call(ctx, {deleg_arg}, iter_val);
                if (ctx.has_exception()) { Object::current_context_ = prev_ctx; return Value(); }
                if (!deleg_result.is_object()) {
                    Object::current_context_ = prev_ctx;
                    ctx.throw_type_error(is_return ? "Iterator return() result is not an Object"
                                                    : "Iterator throw() result is not an Object");
                    return Value();
                }
                Value deleg_done = deleg_result.as_object()->get_property("done");
                if (ctx.has_exception()) { Object::current_context_ = prev_ctx; return Value(); }
                if (deleg_done.to_boolean()) {
                    // done=true: access value and complete.
                    Value deleg_val = deleg_result.as_object()->get_property("value");
                    if (ctx.has_exception()) { Object::current_context_ = prev_ctx; return Value(); }
                    if (is_return) {
                        current_gen->return_argument_ = deleg_val;
                        Collector::write_barrier(current_gen);
                        Object::current_context_ = prev_ctx;
                        throw GeneratorReturnException(deleg_val);
                    }
                    final_val = deleg_val;
                    delegate_done = true;
                    break;
                }
                // done=false: yield the inner result, then wait for the next resumption.
                current_gen->yielded_result_ = deleg_result;
                current_gen->yield_raw_result_ = true;
                current_gen->set_state(Generator::State::SuspendedYield);
                Collector::write_barrier(current_gen);
                quanta_fiber_yield(&current_gen->fiber_);
                Object::current_context_ = &ctx;
                next_arg = current_gen->sent_value_;
            }
            if (delegate_done) break;
        }
        Object::current_context_ = prev_ctx;
        return final_val;
    }

    size_t delegate_start = Generator::increment_yield_counter();

    size_t needed_idx = current_gen->target_yield_index_ >= delegate_start
        ? current_gen->target_yield_index_ - delegate_start
        : 0;

    std::vector<Value> elements;
    bool delegate_exhausted = false;

    auto collect_from_iterator = [&](Value iter_val) {
        Object* iter_obj = nullptr;
        if (iter_val.is_object()) iter_obj = iter_val.as_object();
        else if (iter_val.is_function()) iter_obj = iter_val.as_function();
        if (!iter_obj) { delegate_exhausted = true; return; }

        Value next_fn = iter_obj->get_property("next");
        if (!next_fn.is_function()) { delegate_exhausted = true; return; }

        Generator* saved_gen = Generator::get_current_generator();
        Generator::set_current_generator(nullptr);

        for (size_t i = 0; i <= needed_idx; i++) {
            std::vector<Value> no_args;
            Value next_result = next_fn.as_function()->call(ctx, no_args, iter_val);
            if (ctx.has_exception()) { delegate_exhausted = true; break; }

            Value done_val;
            Value val;
            if (next_result.is_object()) {
                done_val = next_result.as_object()->get_property("done");
                val = next_result.as_object()->get_property("value");
            }

            if (done_val.to_boolean()) {
                delegate_exhausted = true;
                break;
            }
            elements.push_back(val);
        }

        Generator::set_current_generator(saved_gen);
    };

    auto throw_at_iterator = [&](Value iter_val) -> bool {
        Object* iter_obj = nullptr;
        if (iter_val.is_object()) iter_obj = iter_val.as_object();
        else if (iter_val.is_function()) iter_obj = iter_val.as_function();
        if (!iter_obj) return false;

        Value next_fn = iter_obj->get_property("next");
        if (!next_fn.is_function()) return false;

        Generator* saved_gen = Generator::get_current_generator();
        Generator::set_current_generator(nullptr);

        bool iter_done = false;
        for (size_t i = 0; i < needed_idx && !iter_done; i++) {
            std::vector<Value> no_args;
            Value nr = next_fn.as_function()->call(ctx, no_args, iter_val);
            if (ctx.has_exception()) { iter_done = true; break; }
            if (nr.is_object() && nr.as_object()->get_property("done").to_boolean()) {
                iter_done = true;
            }
        }

        if (!iter_done) {
            Value throw_fn = iter_obj->get_property("throw");
            if (throw_fn.is_function()) {
                std::vector<Value> throw_args = {current_gen->throw_value_};
                Value throw_result = throw_fn.as_function()->call(ctx, throw_args, iter_val);
                Generator::set_current_generator(saved_gen);
                if (ctx.has_exception()) return false;
                if (throw_result.is_object()) {
                    Value done_v = throw_result.as_object()->get_property("done");
                    if (!done_v.to_boolean()) {
                        Value val = throw_result.as_object()->get_property("value");
                        current_gen->throwing_ = false;
                        throw YieldException(val);
                    }
                }
            } else {
                Value return_fn_v = iter_obj->get_property("return");
                if (return_fn_v.is_function()) {
                    std::vector<Value> ret_args;
                    return_fn_v.as_function()->call(ctx, ret_args, iter_val);
                    ctx.clear_exception();
                }
                Generator::set_current_generator(saved_gen);
                ctx.throw_exception(current_gen->throw_value_, true);
                return false;
            }
        }
        Generator::set_current_generator(saved_gen);
        return true;
    };

    if (current_gen->throwing_) {
        bool handled = false;
        if (iterable.is_object() || iterable.is_function()) {
            Object* obj = iterable.is_object() ? iterable.as_object() : iterable.as_function();
            Value sym_iter_fn = obj->get_property("Symbol.iterator");
            if (sym_iter_fn.is_function()) {
                Generator* saved_gen = Generator::get_current_generator();
                Generator::set_current_generator(nullptr);
                std::vector<Value> no_args;
                Value iter_val = sym_iter_fn.as_function()->call(ctx, no_args, iterable);
                Generator::set_current_generator(saved_gen);
                if (!ctx.has_exception()) {
                    handled = throw_at_iterator(iter_val);
                }
            } else {
                Value next_fn = obj->get_property("next");
                if (next_fn.is_function()) {
                    handled = throw_at_iterator(iterable);
                }
            }
        }
        current_gen->throwing_ = false;
        (void)handled;
        return Value();
    }

    if (current_gen->returning_) {
        if (iterable.is_object() || iterable.is_function()) {
            Object* obj = iterable.is_object() ? iterable.as_object() : iterable.as_function();
            Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
            Value iter_val;

            Generator* saved_gen = Generator::get_current_generator();
            Generator::set_current_generator(nullptr);

            if (iter_sym) {
                Value sym_iter_fn = obj->get_property(iter_sym->to_property_key());
                if (sym_iter_fn.is_function()) {
                    std::vector<Value> no_args;
                    iter_val = sym_iter_fn.as_function()->call(ctx, no_args, iterable);
                }
            }
            if (iter_val.is_undefined()) iter_val = iterable;

            Object* iter_obj = iter_val.is_object() ? iter_val.as_object()
                             : iter_val.is_function() ? static_cast<Object*>(iter_val.as_function()) : nullptr;

            if (iter_obj && !ctx.has_exception()) {
                Value next_fn_v = iter_obj->get_property("next");
                for (size_t i = 0; i < needed_idx && next_fn_v.is_function(); i++) {
                    std::vector<Value> no_args;
                    Value nr = next_fn_v.as_function()->call(ctx, no_args, iter_val);
                    if (ctx.has_exception()) break;
                    if (nr.is_object() && nr.as_object()->get_property("done").to_boolean()) break;
                }
                if (!ctx.has_exception()) {
                    Value return_fn_v = iter_obj->get_property("return");
                    if (return_fn_v.is_function()) {
                        std::vector<Value> ret_args = {current_gen->return_argument_};
                        return_fn_v.as_function()->call(ctx, ret_args, iter_val);
                        ctx.clear_exception();
                    }
                }
            }
            Generator::set_current_generator(saved_gen);
        }
        return Value();
    }

    if (iterable.is_string()) {
        std::string str = iterable.to_string();
        size_t byte_idx = 0;
        size_t char_idx = 0;
        while (byte_idx < str.size() && char_idx <= needed_idx) {
            unsigned char c = (unsigned char)str[byte_idx];
            size_t char_len = 1;
            if (c >= 0xF0) char_len = 4;
            else if (c >= 0xE0) char_len = 3;
            else if (c >= 0xC0) char_len = 2;
            elements.push_back(Value(str.substr(byte_idx, char_len)));
            byte_idx += char_len;
            char_idx++;
        }
        if (elements.size() < needed_idx + 1) delegate_exhausted = true;
    } else if (iterable.is_object() || iterable.is_function()) {
        Object* obj = iterable.is_object() ? iterable.as_object() : iterable.as_function();

        Value sym_iter_fn = obj->get_property("Symbol.iterator");

        if (sym_iter_fn.is_function()) {
            Generator* saved_gen = Generator::get_current_generator();
            Generator::set_current_generator(nullptr);
            std::vector<Value> no_args;
            Value iter_val = sym_iter_fn.as_function()->call(ctx, no_args, iterable);
            Generator::set_current_generator(saved_gen);
            if (!ctx.has_exception()) {
                collect_from_iterator(iter_val);
            }
        } else {
            Value next_fn = obj->get_property("next");
            if (next_fn.is_function()) {
                collect_from_iterator(iterable);
            } else {
                delegate_exhausted = true;
            }
        }
    } else {
        delegate_exhausted = true;
    }

    size_t delegate_count = elements.size();
    for (size_t i = 1; i < delegate_count; i++) {
        Generator::increment_yield_counter();
    }

    if (current_gen->target_yield_index_ >= delegate_start + delegate_count) {
        return Value();
    }

    size_t element_idx = current_gen->target_yield_index_ - delegate_start;
    throw YieldException(elements[element_idx]);
}



std::string YieldExpression::to_string() const {
    std::ostringstream oss;
    oss << "yield";
    if (is_delegate_) {
        oss << "*";
    }
    if (argument_) {
        oss << " " << argument_->to_string();
    }
    return oss.str();
}

std::unique_ptr<ASTNode> YieldExpression::clone() const {
    return std::make_unique<YieldExpression>(
        argument_ ? argument_->clone() : nullptr,
        is_delegate_,
        start_, end_
    );
}





std::string AsyncFunctionExpression::to_string() const {
    std::ostringstream oss;
    oss << "async function";

    if (id_) {
        oss << " " << id_->get_name();
    }

    oss << "(";
    for (size_t i = 0; i < params_.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << params_[i]->get_name();
    }
    oss << ") ";

    oss << body_->to_string();

    return oss.str();
}

std::unique_ptr<ASTNode> AsyncFunctionExpression::clone() const {
    std::vector<std::unique_ptr<Parameter>> cloned_params;
    for (const auto& param : params_) {
        cloned_params.push_back(
            std::unique_ptr<Parameter>(static_cast<Parameter*>(param->clone().release()))
        );
    }

    auto cloned = std::make_unique<AsyncFunctionExpression>(
        id_ ? std::unique_ptr<Identifier>(static_cast<Identifier*>(id_->clone().release())) : nullptr,
        std::move(cloned_params),
        std::unique_ptr<BlockStatement>(
            body_ ? static_cast<BlockStatement*>(body_->clone().release()) : nullptr),
        start_, end_, is_arrow_
    );
    cloned->set_decl_form(is_decl_form_);
    return cloned;
}




std::string ImportSpecifier::to_string() const {
    if (imported_name_ != local_name_) {
        return imported_name_ + " as " + local_name_;
    }
    return imported_name_;
}

std::unique_ptr<ASTNode> ImportSpecifier::clone() const {
    return std::make_unique<ImportSpecifier>(imported_name_, local_name_, start_, end_);
}

// DeferredNamespaceObject now lives in ModuleLoader.h (CustomObjectBase's own
// switch dispatch needs to name it directly -- see its doc comment there).

Value ImportStatement::evaluate(Context& ctx) {
    Engine* engine = ctx.get_engine();
    if (!engine) {
        ctx.throw_exception(Value(std::string("No engine available for module loading")));
        return Value();
    }

    ModuleLoader* module_loader = engine->get_module_loader();
    if (!module_loader) {
        ctx.throw_exception(Value(std::string("ModuleLoader not available")));
        return Value();
    }

    try {
        // Use the current module's filename so relative imports resolve correctly
        std::string from_path = ctx.get_current_filename();

        // InnerModuleEvaluation: a module whose dependency threw does not
        // evaluate. The dependency's own error object is what propagates, so an
        // importer that catches it sees the value the failing module threw.
        auto dependency_failed = [&ctx](Module* m) -> bool {
            if (!m || !m->has_thrown_exception()) return false;
            ctx.throw_exception(m->get_thrown_exception());
            return true;
        };

        if (!is_namespace_import_ && (!is_default_import_ || is_mixed_import())) {
            if (specifiers_.empty()) {
                // Side-effect-only import: execute the module for its side effects
                if (dependency_failed(module_loader->load_module(module_source_, from_path, module_type_))) return Value();
            } else {
                for (const auto& specifier : specifiers_) {
                    std::string imported_name = specifier->get_imported_name();
                    std::string local_name = specifier->get_local_name();

                    // The binding is an alias for the exporting module's
                    // own, resolved where it is read: a value that module has
                    // not produced yet is still seen once it exists.
                    Module* src = module_loader->load_module(module_source_, from_path, module_type_);
                    if (dependency_failed(src)) return Value();
                    // Two `export *` sources disagreeing about a name make it
                    // ambiguous, which is not an error until something asks for
                    // it by name. This is that moment.
                    if (src && src->resolve_export(imported_name).ambiguous) {
                        ctx.throw_syntax_error("Ambiguous import of '" + imported_name +
                                               "' from '" + module_source_ + "'");
                        return Value();
                    }
                    ctx.create_import_binding(
                        local_name, Value(new ImportBindingObject(src, imported_name)));
                }
            }
        }

        if (is_namespace_import_) {
            Value namespace_obj;
            if (is_deferred_) {
                // `import defer` still loads and links the module; only its
                // evaluation waits. Resolving it here is what lets two deferred
                // imports of one module name the same object.
                Module* deferred = module_loader->prepare_module(module_source_, from_path, module_type_);
                if (deferred && !deferred->get_deferred_namespace().is_undefined()) {
                    namespace_obj = deferred->get_deferred_namespace();
                } else {
                    namespace_obj = Value(new DeferredNamespaceObject(module_loader, module_source_, from_path, module_type_));
                    if (deferred) deferred->set_deferred_namespace(namespace_obj);
                }
            } else {
                Module* src = module_loader->load_module(module_source_, from_path, module_type_);
                if (dependency_failed(src)) return Value();
                // Built from the module the type resolved to, not from the
                // specifier again: one file imported as JSON and as source is
                // two modules with two namespaces.
                namespace_obj = ModuleLoader::build_module_namespace(src);
            }
            ctx.create_binding(namespace_alias_, namespace_obj,
                               /*mutable_binding=*/false, /*deletable=*/false);
        }

        if (is_default_import_) {
            Module* src = module_loader->load_module(module_source_, from_path, module_type_);
            if (dependency_failed(src)) return Value();
            ctx.create_import_binding(default_alias_,
                                      Value(new ImportBindingObject(src, "default")));
        }

        return Value();

    } catch (const std::exception& e) {
        ctx.throw_exception(Value("Module import failed: " + std::string(e.what())));
        return Value();
    }
}

std::string ImportStatement::to_string() const {
    std::string result = "import ";

    if (is_namespace_import_) {
        result += "* as " + namespace_alias_;
    } else if (is_default_import_) {
        result += default_alias_;
    } else {
        result += "{ ";
        for (size_t i = 0; i < specifiers_.size(); ++i) {
            if (i > 0) result += ", ";
            result += specifiers_[i]->to_string();
        }
        result += " }";
    }

    result += " from \"" + module_source_ + "\"";
    return result;
}

std::unique_ptr<ASTNode> ImportStatement::clone() const {
    if (is_namespace_import_ && is_default_import_) {
        return std::make_unique<ImportStatement>(default_alias_, namespace_alias_, module_source_, start_, end_);
    } else if (is_namespace_import_) {
        return std::make_unique<ImportStatement>(namespace_alias_, module_source_, start_, end_, is_deferred_);
    } else if (is_default_import_) {
        return std::make_unique<ImportStatement>(default_alias_, module_source_, true, start_, end_);
    } else {
        std::vector<std::unique_ptr<ImportSpecifier>> cloned_specifiers;
        for (const auto& spec : specifiers_) {
            cloned_specifiers.push_back(
                std::make_unique<ImportSpecifier>(
                    spec->get_imported_name(),
                    spec->get_local_name(),
                    spec->get_start(),
                    spec->get_end()
                )
            );
        }
        return std::make_unique<ImportStatement>(std::move(cloned_specifiers), module_source_, start_, end_);
    }
}



std::string ExportSpecifier::to_string() const {
    if (local_name_ != exported_name_) {
        return local_name_ + " as " + exported_name_;
    }
    return local_name_;
}

std::unique_ptr<ASTNode> ExportSpecifier::clone() const {
    return std::make_unique<ExportSpecifier>(local_name_, exported_name_, start_, end_);
}

Value ExportStatement::evaluate(Context& ctx) { return link(ctx, /*declaration_already_run=*/false); }

bool ExportStatement::default_is_hoistable() const {
    if (!is_default_export_ || !default_export_) return false;
    // The parser already made the distinction the grammar draws: a
    // HoistableDeclaration is marked as such, while `export default (function
    // (){})` came through AssignmentExpression and keeps its dead zone.
    switch (default_export_->get_type()) {
        case Type::FUNCTION_EXPRESSION:
            return static_cast<const FunctionExpression*>(default_export_.get())->is_decl_form();
        case Type::ASYNC_FUNCTION_EXPRESSION:
            return static_cast<const AsyncFunctionExpression*>(default_export_.get())->is_decl_form();
        default:
            return false;
    }
}

void ExportStatement::hoist_default(Context& ctx) {
    if (default_is_hoistable()) link(ctx, /*declaration_already_run=*/false);
}

// Applies one export record. Everything the declaration settled while
// compiling is here; the only thing that ran is a default export's value,
// which arrives in `default_value`.
Value link_export_record(Context& ctx, const BytecodeChunk::ExportRecord& rec,
                         const Value& default_value) {
    Value exports_value = ctx.get_binding("exports");
    Object* exports_obj = nullptr;
    if (!exports_value.is_object()) {
        exports_obj = new Object();
        ctx.create_binding("exports", Value(exports_obj), true);
        if (Environment* lexical_env = ctx.get_lexical_environment()) {
            lexical_env->create_binding("exports", Value(exports_obj), true);
        }
    } else {
        exports_obj = exports_value.as_object();
    }

    // export name -> the module-scope binding behind it, so Module::get_export
    // can answer with what that binding holds now rather than a copy of what it
    // held at instantiation.
    auto record_local_name = [&](const std::string& export_name, const std::string& local_name) {
        const std::string LOCALNAMES_KEY = "\x01localnames";
        Object* tracker = nullptr;
        Value tracker_val = ctx.get_binding(LOCALNAMES_KEY);
        if (tracker_val.is_object()) {
            tracker = tracker_val.as_object();
        } else {
            auto t = ObjectFactory::create_object();
            if (t) {
                tracker = t.get();
                ctx.create_binding(LOCALNAMES_KEY, Value(t.release()));
            }
        }
        if (tracker) tracker->set_property(export_name, Value(local_name));
    };

    if (rec.is_default) {
        const std::string binding_name =
            rec.default_local.empty() ? std::string("*default*") : rec.default_local;
        // A hoistable default was built and bound before the module's
        // statements ran; reaching the statement must not replace the function
        // an importer may already hold.
        Value value = (rec.default_is_hoistable && ctx.has_binding(binding_name))
                          ? ctx.get_binding(binding_name)
                          : default_value;
        if (rec.default_local.empty() && value.is_function()) {
            // SetFunctionName: an anonymous default is named "default".
            Function* fn = value.as_function();
            if (fn->get_property("name").to_string().empty()) {
                PropertyDescriptor nm(Value(std::string("default")), PropertyAttributes::Configurable);
                fn->set_property_descriptor("name", nm);
            }
        }
        if (!ctx.has_binding(binding_name)) {
            ctx.create_binding(binding_name, value, true);
        } else {
            ctx.set_binding(binding_name, value);
        }
        record_local_name("default", binding_name);
        exports_obj->set_property("default", value);
        if (Engine* engine = ctx.get_engine()) engine->register_default_export("", value);
    }

    for (const auto& entry : rec.entries) {
        const std::string& export_name = entry.export_name;
        const std::string& local_name = entry.local_name;
        if (rec.is_re_export && !rec.source_module.empty()) {
            Engine* engine = ctx.get_engine();
            ModuleLoader* loader = engine ? engine->get_module_loader() : nullptr;
            if (!loader) continue;
            Module* src = loader->load_module(rec.source_module, ctx.get_current_filename());
            if (local_name == "*") {
                if (src && src->has_thrown_exception()) {
                    ctx.throw_exception(src->get_thrown_exception());
                    return Value();
                }
                // `export * from` re-exports each name rather than a property;
                // only the named form (`export * as ns from`) makes one.
                if (src && export_name != "*") {
                    exports_obj->set_property(export_name,
                                              ModuleLoader::build_module_namespace(src));
                }
                continue;
            }
            if (!src) {
                ctx.throw_syntax_error("Cannot re-export '" + local_name + "' from '" +
                                       rec.source_module + "'");
                return Value();
            }
            if (src->has_thrown_exception()) {
                ctx.throw_exception(src->get_thrown_exception());
                return Value();
            }
            if (src->resolve_export(local_name).ambiguous) {
                ctx.throw_syntax_error("Ambiguous re-export of '" + local_name + "' from '" +
                                       rec.source_module + "'");
                return Value();
            }
            if (!src->is_loading() && !src->has_export(local_name)) {
                ctx.throw_syntax_error("Cannot re-export '" + local_name + "' from '" +
                                       rec.source_module + "'");
                return Value();
            }
            exports_obj->set_property(export_name, Value(new ImportBindingObject(src, local_name)));
            continue;
        }
        if (!ctx.has_binding(local_name)) {
            if (entry.from_declaration) continue;
            ctx.throw_exception(Value("ReferenceError: " + local_name + " is not defined"));
            return Value();
        }
        exports_obj->set_property(export_name, ctx.get_binding(local_name));
        record_local_name(export_name, local_name);
    }
    return Value();
}

Value ExportStatement::link(Context& ctx, bool declaration_already_run) {
    Value exports_value = ctx.get_binding("exports");
    Object* exports_obj = nullptr;

    if (!exports_value.is_object()) {
        exports_obj = new Object();
        ctx.create_binding("exports", Value(exports_obj), true);

        Environment* lexical_env = ctx.get_lexical_environment();
        if (lexical_env) {
            lexical_env->create_binding("exports", Value(exports_obj), true);
        }
    } else {
        exports_obj = exports_value.as_object();
    }

    // Records export_name -> local module-scope binding name in a hidden tracker
    // object, read back by ModuleLoader::execute_module_file so Module::get_export
    // can return LIVE values (ES module exports are live bindings, not snapshots).
    auto record_local_name = [&](const std::string& export_name, const std::string& local_name) {
        const std::string LOCALNAMES_KEY = "\x01localnames";
        Object* tracker = nullptr;
        Value tracker_val = ctx.get_binding(LOCALNAMES_KEY);
        if (tracker_val.is_object()) {
            tracker = tracker_val.as_object();
        } else {
            auto t = ObjectFactory::create_object();
            if (t) {
                tracker = t.get();
                ctx.create_binding(LOCALNAMES_KEY, Value(t.release()));
            }
        }
        if (tracker) tracker->set_property(export_name, Value(local_name));
    };

    if (is_default_export_ && default_export_) {
        // export default function fn() {} / export default class Foo {} / export default
        // async function fn() {} are HoistableDeclarations: the bound name gets a normal
        // MUTABLE module-scope binding (unlike a named function expression, whose own name
        // is an immutable self-reference). `default` is then a live alias for that binding,
        // so reassignments inside the function (e.g. `fn = 2`) are observable through the
        // module namespace's `default` property.
        std::string default_local_name;
        switch (default_export_->get_type()) {
            case Type::FUNCTION_EXPRESSION: {
                auto* fe = static_cast<FunctionExpression*>(default_export_.get());
                if (fe->is_named()) default_local_name = fe->get_id()->get_name();
                break;
            }
            case Type::ASYNC_FUNCTION_EXPRESSION: {
                auto* afe = static_cast<AsyncFunctionExpression*>(default_export_.get());
                if (afe->get_id()) default_local_name = afe->get_id()->get_name();
                break;
            }
            case Type::CLASS_DECLARATION: {
                auto* cd = static_cast<ClassDeclaration*>(default_export_.get());
                if (cd->get_id()) default_local_name = cd->get_id()->get_name();
                // NamedEvaluation: static initializers observe this.name during
                // evaluation, so the "default" name must be inferred beforehand.
                if (default_local_name.empty()) cd->set_inferred_name("default");
                break;
            }
            default:
                break;
        }

        // The default is an ordinary expression (a function or class literal
        // included), so it compiles like any other rather than being walked.
        // A hoistable one was already built and bound before the module's
        // statements ran; reaching the statement itself must not replace the
        // function an importer may already be holding.
        const std::string default_binding =
            default_local_name.empty() ? std::string("*default*") : default_local_name;
        Value default_value;
        if (default_is_hoistable() && ctx.has_binding(default_binding)) {
            default_value = ctx.get_binding(default_binding);
        } else {
            default_value = eval_class_expr(default_export_.get(), ctx);
            if (ctx.has_exception()) return Value();
        }

        if (!default_local_name.empty()) {
            if (!ctx.has_binding(default_local_name)) {
                ctx.create_binding(default_local_name, default_value, true);
            } else {
                ctx.set_binding(default_local_name, default_value);
            }
            record_local_name("default", default_local_name);
        } else {
            // Spec: anonymous default export (class/function) gets name "default" (SetFunctionName).
            if (default_value.is_function()) {
                Function* fn = default_value.as_function();
                Value nm = fn->get_property("name");
                if (nm.to_string().empty()) {
                    PropertyDescriptor nm_desc(Value(std::string("default")), PropertyAttributes::Configurable);
                    fn->set_property_descriptor("name", nm_desc);
                }
            }
            // An anonymous default still needs a module-scope binding to be an
            // alias FOR: an importer resolves through the local name, and
            // without one it would only ever see what the export record held
            // when the import was made.
            const std::string anon_default = "*default*";
            if (!ctx.has_binding(anon_default)) {
                ctx.create_binding(anon_default, default_value, true);
            } else {
                ctx.set_binding(anon_default, default_value);
            }
            record_local_name("default", anon_default);
        }

        exports_obj->set_property("default", default_value);

        Engine* engine = ctx.get_engine();
        if (engine) {
            engine->register_default_export("", default_value);
        }
    }

    if (is_declaration_export_ && declaration_) {
        // The declaration itself was compiled with the rest of the module;
        // only the record it needs is kept here.
        (void)declaration_already_run;

        if (declaration_->get_type() == Type::FUNCTION_DECLARATION) {
            FunctionDeclaration* func_decl = static_cast<FunctionDeclaration*>(declaration_.get());
            std::string func_name = func_decl->get_id()->get_name();

            if (ctx.has_binding(func_name)) {
                Value func_value = ctx.get_binding(func_name);
                exports_obj->set_property(func_name, func_value);
                record_local_name(func_name, func_name);
            }
        } else if (declaration_->get_type() == Type::VARIABLE_DECLARATION) {
            VariableDeclaration* var_decl = static_cast<VariableDeclaration*>(declaration_.get());

            auto record = [&](const std::string& var_name) {
                if (var_name.empty() || !ctx.has_binding(var_name)) return;
                exports_obj->set_property(var_name, ctx.get_binding(var_name));
                record_local_name(var_name, var_name);
            };
            for (const auto& declarator : var_decl->get_declarations()) {
                // `export const [a, b] = ...` binds through a pattern, so the
                // names are its leaves rather than the declarator's own id.
                if (declarator->get_init() &&
                    declarator->get_init()->get_type() == Type::DESTRUCTURING_ASSIGNMENT) {
                    std::vector<std::string> bound;
                    static_cast<const DestructuringAssignment*>(declarator->get_init())
                        ->collect_bound_names(bound);
                    for (const auto& bn : bound) record(bn);
                    continue;
                }
                record(declarator->get_id()->get_name());
            }
        } else if (declaration_->get_type() == Type::CLASS_DECLARATION) {
            auto* class_decl = static_cast<ClassDeclaration*>(declaration_.get());
            if (class_decl->get_id() && !class_decl->get_id()->get_name().empty()) {
                const std::string& cname = class_decl->get_id()->get_name();
                if (ctx.has_binding(cname)) {
                    exports_obj->set_property(cname, ctx.get_binding(cname));
                    record_local_name(cname, cname);
                }
            }
        }
    }

    for (const auto& specifier : specifiers_) {
        std::string local_name = specifier->get_local_name();
        std::string export_name = specifier->get_exported_name();
        Value export_value;

        if (is_re_export_ && !source_module_.empty()) {
            Engine* engine = ctx.get_engine();
            ModuleLoader* module_loader = engine ? engine->get_module_loader() : nullptr;

            if (local_name == "*") {
                // export * from './module.js'  OR  export * as ns from './module.js'
                if (module_loader) {
                    Module* src_mod = module_loader->load_module(source_module_, ctx.get_current_filename());
                    if (src_mod && src_mod->has_thrown_exception()) {
                        ctx.throw_exception(src_mod->get_thrown_exception());
                        return Value();
                    }
                    if (src_mod) {
                        if (export_name == "*") {
                            // `export * from` contributes nothing of this
                            // module's own. The source was recorded as a star
                            // source before the module ran, and resolving a
                            // name walks those -- copying the source's names
                            // here would make them look like own exports, and
                            // a name two sources disagree about would stop
                            // being ambiguous.
                        } else {
                            // export * as ns from './module.js' -- use cached namespace object
                            // (same source module must always yield the same namespace object identity
                            //  so that star-export ambiguity checks compare correctly)
                            Value ns_val = ModuleLoader::build_module_namespace(src_mod);
                            exports_obj->set_property(export_name, ns_val);
                        }
                    }
                }
                continue;
            }

            // Named re-export: export { local_name as export_name } from './module.js'
            if (module_loader) {
                Module* src_mod = module_loader->load_module(source_module_, ctx.get_current_filename());
                if (!src_mod) {
                    ctx.throw_syntax_error("Cannot re-export '" + local_name + "' from '" + source_module_ + "'");
                    return Value();
                }
                if (src_mod->has_thrown_exception()) {
                    ctx.throw_exception(src_mod->get_thrown_exception());
                    return Value();
                }
                if (src_mod->resolve_export(local_name).ambiguous) {
                    ctx.throw_syntax_error("Ambiguous re-export of '" + local_name + "' from '" +
                                           source_module_ + "'");
                    return Value();
                }
                if (!src_mod->is_loading() && !src_mod->has_export(local_name)) {
                    // Fully loaded source doesn't export this name
                    ctx.throw_syntax_error("Cannot re-export '" + local_name + "' from '" + source_module_ + "'");
                    return Value();
                }
                // What this records is the link, not the value behind it. A
                // re-export chain that runs back through a cycle names a
                // binding the source has not produced yet, and reading the
                // export later follows the chain to whoever really owns it.
                export_value = Value(new ImportBindingObject(src_mod, local_name));
            }
        } else {
            if (ctx.has_binding(local_name)) {
                export_value = ctx.get_binding(local_name);
                record_local_name(export_name, local_name);
            } else {
                ctx.throw_exception(Value("ReferenceError: " + local_name + " is not defined"));
                return Value();
            }
        }

        exports_obj->set_property(export_name, export_value);
    }

    return Value();
}

std::string ExportStatement::to_string() const {
    std::string result = "export ";

    if (is_default_export_) {
        result += "default " + default_export_->to_string();
    } else if (is_declaration_export_) {
        result += declaration_->to_string();
    } else {
        result += "{ ";
        for (size_t i = 0; i < specifiers_.size(); ++i) {
            if (i > 0) result += ", ";
            result += specifiers_[i]->to_string();
        }
        result += " }";

        if (is_re_export_) {
            result += " from \"" + source_module_ + "\"";
        }
    }

    return result;
}

std::unique_ptr<ASTNode> ExportStatement::clone() const {
    if (is_default_export_) {
        return std::make_unique<ExportStatement>(default_export_->clone(), true, start_, end_);
    } else if (is_declaration_export_) {
        return std::make_unique<ExportStatement>(declaration_->clone(), start_, end_);
    } else {
        std::vector<std::unique_ptr<ExportSpecifier>> cloned_specifiers;
        for (const auto& spec : specifiers_) {
            cloned_specifiers.push_back(
                std::make_unique<ExportSpecifier>(
                    spec->get_local_name(),
                    spec->get_exported_name(),
                    spec->get_start(),
                    spec->get_end()
                )
            );
        }

        if (is_re_export_) {
            return std::make_unique<ExportStatement>(std::move(cloned_specifiers), source_module_, start_, end_);
        } else {
            return std::make_unique<ExportStatement>(std::move(cloned_specifiers), start_, end_);
        }
    }
}

} // namespace Quanta
