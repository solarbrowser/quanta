/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <cstdio>
#include <cstdlib>
#include "quanta/core/vm/BytecodeCompiler.h"
#include <functional>
#include "quanta/core/runtime/BigInt.h"
#include <algorithm>
#include "quanta/parser/AST.h"
#include "quanta/core/runtime/Shape.h"
#include <climits>
#include <cmath>
#include <sstream>

namespace Quanta {

// From call.cpp: the site's frozen template object, the one the tree-walker
// hands a tag. Asked for once while compiling and parked in the constants, so
// the chunk keeps the object rather than the node it was built from.
Value get_template_object(class TemplateLiteral* tmpl);


// Defined in the tree-walker's language.cpp: the per-decl-site description a
// function literal instantiates from, so the compiled and interpreted paths
// build closures from the identical data.
ClosureTemplate closure_template_for(const ASTNode* literal);


namespace {

constexpr int kMaxRegisters = 255;

// Freezes the compiler's name pool into the chunk's interned form. This is the
// one place a name's text is hashed; from here on every reader holds the
// canonical pointer, which is what lets a binding lookup compare pointers
// instead of bytes (see BytecodeChunk::names).
FixedArray<const std::string*> intern_name_pool(std::vector<std::string> names) {
    std::vector<const std::string*> keys;
    keys.reserve(names.size());
    for (const std::string& n : names) keys.push_back(Shape::intern(n));
    return FixedArray<const std::string*>::from(std::move(keys));
}

struct DeclInfo {
    std::string name;
    bool is_lexical;  // let/const: needs a TDZ-seeded register
    bool is_const;
    bool is_catch_param = false;  // gets its own per-catch env; never a function-level local
    bool is_annexb_fn = false;    // Annex B B.3.4 if/label clause: dropped when a
                                  // lexical or a parameter already owns the name
};

// Appends every leaf binding name a pattern declares. Nested patterns are no
// longer opaque here: they used to be delimiter-encoded identifier strings, so
// any nesting made this bail and dropped the whole function to the tree-walker.
bool collect_flat_pattern_names(const ASTNode* pattern, bool is_lexical, bool is_const,
                                 std::vector<DeclInfo>& out) {
    if (!pattern) return true;
    if (pattern->get_type() == ASTNode::Type::IDENTIFIER) {
        const std::string& n = static_cast<const Identifier*>(pattern)->get_name();
        if (n.empty()) return true;  // elision, or a destructuring declarator's placeholder id
        out.push_back({n, is_lexical, is_const});
        return true;
    }
    if (pattern->get_type() != ASTNode::Type::DESTRUCTURING_ASSIGNMENT) return false;
    std::vector<std::string> bound;
    static_cast<const DestructuringAssignment*>(pattern)->collect_bound_names(bound);
    for (const auto& n : bound) out.push_back({n, is_lexical, is_const});
    return true;
}

// Collects every declared name up front (var hoisting), including repeats --
// see contains_nested_lexical_decl for why duplicates are fine here.
bool prescan_declarations(const ASTNode* node, std::vector<DeclInfo>& out);

// Annex B B.3.4 positions: a function declaration standing alone as an if
// clause or a label's body declares a var-scoped name, unlike one inside a
// block, whose binding belongs to the block.
bool prescan_annexb_fn_decl(const ASTNode* node, std::vector<DeclInfo>& out) {
    if (node && node->get_type() == ASTNode::Type::FUNCTION_DECLARATION) {
        const Identifier* id = static_cast<const FunctionDeclaration*>(node)->get_id();
        if (!id || id->get_name().empty()) return false;
        out.push_back({id->get_name(), /*is_lexical=*/false, /*is_const=*/false,
                       /*is_catch_param=*/false, /*is_annexb_fn=*/true});
        return true;
    }
    return prescan_declarations(node, out);
}

bool prescan_declarations(const ASTNode* node, std::vector<DeclInfo>& out) {
    if (!node) return true;
    switch (node->get_type()) {
        case ASTNode::Type::CLASS_DECLARATION: {
            // Nested class statements are lexically-scoped like `let` and
            // need a declared slot too, or is_local() never recognizes the
            // name (see the CLASS_DECLARATION case in compile_statement).
            const auto* cd = static_cast<const ClassDeclaration*>(node);
            if (!cd->is_expression() && cd->get_id() && !cd->get_id()->get_name().empty()) {
                out.push_back({cd->get_id()->get_name(), /*is_lexical=*/true, /*is_const=*/false});
            }
            return true;
        }
        case ASTNode::Type::USING_DECLARATION: {
            // Lexically scoped and never reassignable, so it needs a declared
            // slot exactly as a `const` does.
            for (const auto& b : static_cast<const UsingDeclaration*>(node)->get_bindings()) {
                if (b.name.empty()) return false;
                out.push_back({b.name, /*is_lexical=*/true, /*is_const=*/true});
            }
            return true;
        }
        case ASTNode::Type::VARIABLE_DECLARATION: {
            const auto* decl = static_cast<const VariableDeclaration*>(node);
            bool is_lexical = decl->get_kind() != VariableDeclarator::Kind::VAR;
            bool is_const = decl->get_kind() == VariableDeclarator::Kind::CONST;
            for (const auto& d : decl->get_declarations()) {
                if (!d->get_id()) return false;
                const std::string& name = d->get_id()->get_name();
                if (name.empty()) {
                    // A destructuring declarator has no name of its own; its
                    // names come from the pattern. Declaring them here is what
                    // lets emit_pattern_bind write them as ordinary locals,
                    // with the same TDZ and const marking a plain `const x`
                    // gets.
                    const ASTNode* init = d->get_init();
                    if (init && init->get_type() == ASTNode::Type::ASSIGNMENT_EXPRESSION) {
                        init = static_cast<const AssignmentExpression*>(init)->get_left();
                    }
                    if (init && init->get_type() == ASTNode::Type::DESTRUCTURING_ASSIGNMENT) {
                        std::vector<std::string> bound;
                        static_cast<const DestructuringAssignment*>(init)->collect_bound_names(bound);
                        for (const auto& bn : bound) out.push_back({bn, is_lexical, is_const});
                    }
                    continue;
                }
                out.push_back({name, is_lexical, is_const});
            }
            return true;
        }
        case ASTNode::Type::BLOCK_STATEMENT: {
            const auto* block = static_cast<const BlockStatement*>(node);
            for (const auto& stmt : block->get_statements()) {
                if (!prescan_declarations(stmt.get(), out)) return false;
            }
            return true;
        }
        case ASTNode::Type::IF_STATEMENT: {
            const auto* n = static_cast<const IfStatement*>(node);
            return prescan_annexb_fn_decl(n->get_consequent(), out) &&
                   prescan_annexb_fn_decl(n->get_alternate(), out);
        }
        case ASTNode::Type::WHILE_STATEMENT: {
            const auto* n = static_cast<const WhileStatement*>(node);
            return prescan_declarations(n->get_body(), out);
        }
        case ASTNode::Type::DO_WHILE_STATEMENT: {
            const auto* n = static_cast<const DoWhileStatement*>(node);
            return prescan_declarations(n->get_body(), out);
        }
        case ASTNode::Type::FOR_STATEMENT: {
            const auto* n = static_cast<const ForStatement*>(node);
            return prescan_declarations(n->get_init(), out) &&
                   prescan_declarations(n->get_body(), out);
        }
        case ASTNode::Type::FOR_OF_STATEMENT: {
            const auto* n = static_cast<const ForOfStatement*>(node);
            if (n->get_left()->get_type() == ASTNode::Type::VARIABLE_DECLARATION) {
                const auto* vd = static_cast<const VariableDeclaration*>(n->get_left());
                if (vd->declaration_count() != 1) return false;
                const auto& d = vd->get_declarations()[0];
                if (!d->get_id()) return false;
                const std::string& name = d->get_id()->get_name();
                // Empty = destructuring declarator: no named slot (pushing ""
                // would collide in declare_local).
                if (!name.empty()) {
                    out.push_back({name, vd->get_kind() != VariableDeclarator::Kind::VAR,
                                    vd->get_kind() == VariableDeclarator::Kind::CONST});
                }
            } else if (n->get_left()->get_type() == ASTNode::Type::USING_DECLARATION) {
                for (const auto& b : static_cast<const UsingDeclaration*>(n->get_left())->get_bindings()) {
                    if (b.name.empty()) return false;
                    out.push_back({b.name, /*is_lexical=*/true, /*is_const=*/true});
                }
            } else if (n->get_left()->get_type() == ASTNode::Type::DESTRUCTURING_ASSIGNMENT &&
                       n->get_left_decl_kind() >= 0) {
                // Destructuring header: leaf names need declared slots too,
                // or reads compile to LdaLookup, whose per-chunk cache would
                // freeze on iteration 1's loop env.
                if (!collect_flat_pattern_names(n->get_left(), n->get_left_decl_kind() != 0,
                                                 n->get_left_decl_kind() == 2, out)) {
                    return false;
                }
            }
            return prescan_declarations(n->get_body(), out);
        }
        case ASTNode::Type::FOR_IN_STATEMENT: {
            const auto* n = static_cast<const ForInStatement*>(node);
            if (n->get_left()->get_type() == ASTNode::Type::VARIABLE_DECLARATION) {
                const auto* vd = static_cast<const VariableDeclaration*>(n->get_left());
                if (vd->declaration_count() != 1) return false;
                const auto& d = vd->get_declarations()[0];
                if (!d->get_id()) return false;
                const std::string& name = d->get_id()->get_name();
                if (!name.empty()) {
                    out.push_back({name, vd->get_kind() != VariableDeclarator::Kind::VAR,
                                    vd->get_kind() == VariableDeclarator::Kind::CONST});
                }
            } else if (n->get_left()->get_type() == ASTNode::Type::DESTRUCTURING_ASSIGNMENT &&
                       n->get_left_decl_kind() >= 0) {
                if (!collect_flat_pattern_names(n->get_left(), n->get_left_decl_kind() != 0,
                                                 n->get_left_decl_kind() == 2, out)) {
                    return false;
                }
            }
            return prescan_declarations(n->get_body(), out);
        }
        case ASTNode::Type::TRY_STATEMENT: {
            const auto* n = static_cast<const TryStatement*>(node);
            if (!prescan_declarations(n->get_try_block(), out)) return false;
            if (const ASTNode* cc = n->get_catch_clause()) {
                const auto* clause = static_cast<const CatchClause*>(cc);
                if (const ASTNode* pat = clause->get_destructuring_pattern()) {
                    // A pattern parameter binds the names the pattern names,
                    // in the same per-catch environment a plain one gets.
                    std::vector<std::string> bound;
                    static_cast<const DestructuringAssignment*>(pat)->collect_bound_names(bound);
                    for (const auto& bn : bound) {
                        if (bn.empty()) return false;
                        out.push_back({bn, false, false, true});
                    }
                } else {
                    const std::string& pname = clause->get_parameter_name();
                    if (!pname.empty()) {
                        out.push_back({pname, false, false, true});  // its own per-catch env
                    }
                }
                if (!prescan_declarations(clause->get_body(), out)) return false;
            }
            return prescan_declarations(n->get_finally_block(), out);
        }
        case ASTNode::Type::SWITCH_STATEMENT: {
            const auto* n = static_cast<const SwitchStatement*>(node);
            for (const auto& c : n->get_cases()) {
                for (const auto& s : static_cast<const CaseClause*>(c.get())->get_consequent()) {
                    if (!prescan_declarations(s.get(), out)) return false;
                }
            }
            return true;
        }
        case ASTNode::Type::WITH_STATEMENT:
            // The object expression declares nothing; the body does, and it is
            // ordinary statements -- the with only changes how names resolve.
            return prescan_declarations(static_cast<const WithStatement*>(node)->get_body(), out);
        case ASTNode::Type::LABELED_STATEMENT:
            return prescan_annexb_fn_decl(static_cast<const LabeledStatement*>(node)->get_statement(), out);
        default:
            return true;
    }
}

// True if `name` is ever an assignment or ++/-- target anywhere in `node`.
// Whether a pattern suspends while it runs. A `return()` on the generator
// resumes such a suspension by unwinding a C++ exception, which travels past
// the handler-table entry the emitter uses to close the iterator, so a pattern
// like `[ {} = yield ] = vals` stays on the tree-walker.
bool pattern_contains_suspension(const ASTNode* node) {
    if (!node) return false;
    switch (node->get_type()) {
        case ASTNode::Type::YIELD_EXPRESSION:
        case ASTNode::Type::AWAIT_EXPRESSION:
            return true;
        case ASTNode::Type::OBJECT_LITERAL: {
            const auto* n = static_cast<const ObjectLiteral*>(node);
            for (const auto& prop : n->get_properties()) {
                if (prop->computed && pattern_contains_suspension(prop->key.get())) return true;
                if (prop->value && pattern_contains_suspension(prop->value.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::ARRAY_LITERAL: {
            const auto* n = static_cast<const ArrayLiteral*>(node);
            for (const auto& el : n->get_elements()) {
                if (el && pattern_contains_suspension(el.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::ASSIGNMENT_EXPRESSION: {
            const auto* n = static_cast<const AssignmentExpression*>(node);
            return pattern_contains_suspension(n->get_left()) ||
                   pattern_contains_suspension(n->get_right());
        }
        case ASTNode::Type::SPREAD_ELEMENT:
            return pattern_contains_suspension(
                static_cast<const SpreadElement*>(node)->get_argument());
        case ASTNode::Type::MEMBER_EXPRESSION: {
            // A member target carries expressions of its own, and its computed
            // key is where a suspension actually lands.
            const auto* n = static_cast<const MemberExpression*>(node);
            if (pattern_contains_suspension(n->get_object())) return true;
            return n->is_computed() && pattern_contains_suspension(n->get_property());
        }
        default:
            return false;
    }
}

// Every name a destructuring pattern writes to, at any depth. The cover
// grammar parses a pattern as an object/array literal, so the targets sit where
// property values and array elements do, under an optional default or spread.
void collect_pattern_target_names(const ASTNode* pattern, std::vector<std::string>& out) {
    if (!pattern) return;
    auto take = [&out](const ASTNode* t) {
        if (t && t->get_type() == ASTNode::Type::IDENTIFIER) {
            out.push_back(static_cast<const Identifier*>(t)->get_name());
        } else {
            collect_pattern_target_names(t, out);
        }
    };
    auto unwrap = [](const ASTNode* t) {
        if (t && t->get_type() == ASTNode::Type::SPREAD_ELEMENT) {
            t = static_cast<const SpreadElement*>(t)->get_argument();
        }
        if (t && t->get_type() == ASTNode::Type::ASSIGNMENT_EXPRESSION) {
            t = static_cast<const AssignmentExpression*>(t)->get_left();
        }
        return t;
    };
    if (pattern->get_type() == ASTNode::Type::OBJECT_LITERAL) {
        for (const auto& prop : static_cast<const ObjectLiteral*>(pattern)->get_properties()) {
            if (prop->value) take(unwrap(prop->value.get()));
        }
    } else if (pattern->get_type() == ASTNode::Type::ARRAY_LITERAL) {
        for (const auto& el : static_cast<const ArrayLiteral*>(pattern)->get_elements()) {
            if (el) take(unwrap(el.get()));
        }
    }
}

// Runtime const-immutability isn't implemented, so this is the compile-time check.
// The identifier targets a pattern writes to. Only the shapes
// pattern_is_emittable accepts need walking: any other leaf refuses to compile
// before residency can matter.
bool pattern_writes_identifier(const ASTNode* pattern, const std::string& name) {
    if (!pattern) return false;
    switch (pattern->get_type()) {
        case ASTNode::Type::IDENTIFIER:
            return static_cast<const Identifier*>(pattern)->get_name() == name;
        case ASTNode::Type::SPREAD_ELEMENT:
            return pattern_writes_identifier(
                static_cast<const SpreadElement*>(pattern)->get_argument(), name);
        case ASTNode::Type::ASSIGNMENT_EXPRESSION:
            return pattern_writes_identifier(
                static_cast<const AssignmentExpression*>(pattern)->get_left(), name);
        case ASTNode::Type::DESTRUCTURING_ASSIGNMENT:
            return pattern_writes_identifier(
                static_cast<const DestructuringAssignment*>(pattern)->get_pattern_literal(), name);
        case ASTNode::Type::ARRAY_LITERAL:
            for (const auto& el : static_cast<const ArrayLiteral*>(pattern)->get_elements()) {
                if (pattern_writes_identifier(el.get(), name)) return true;
            }
            return false;
        case ASTNode::Type::OBJECT_LITERAL:
            for (const auto& prop : static_cast<const ObjectLiteral*>(pattern)->get_properties()) {
                if (prop->value && pattern_writes_identifier(prop->value.get(), name)) return true;
            }
            return false;
        default:
            return false;
    }
}

// A for-in/of head without a declaration keyword assigns to its target once
// per iteration rather than binding it, which is what makes a const there a
// TypeError. Without this the compiled loop wrote the const's register and
// the refusal never happened.
bool head_target_assigns(const ASTNode* left, int decl_kind, const std::string& name) {
    if (decl_kind >= 0) return false;  // a declaration binds, it does not assign
    return pattern_writes_identifier(left, name);
}

bool assigns_to_identifier(const ASTNode* node, const std::string& name) {
    if (!node) return false;
    switch (node->get_type()) {
        case ASTNode::Type::BLOCK_STATEMENT: {
            const auto* n = static_cast<const BlockStatement*>(node);
            for (const auto& stmt : n->get_statements()) {
                if (assigns_to_identifier(stmt.get(), name)) return true;
            }
            return false;
        }
        case ASTNode::Type::IF_STATEMENT: {
            const auto* n = static_cast<const IfStatement*>(node);
            return assigns_to_identifier(n->get_test(), name) ||
                   assigns_to_identifier(n->get_consequent(), name) ||
                   assigns_to_identifier(n->get_alternate(), name);
        }
        case ASTNode::Type::WHILE_STATEMENT: {
            const auto* n = static_cast<const WhileStatement*>(node);
            return assigns_to_identifier(n->get_test(), name) ||
                   assigns_to_identifier(n->get_body(), name);
        }
        case ASTNode::Type::DO_WHILE_STATEMENT: {
            const auto* n = static_cast<const DoWhileStatement*>(node);
            return assigns_to_identifier(n->get_body(), name) ||
                   assigns_to_identifier(n->get_test(), name);
        }
        case ASTNode::Type::FOR_STATEMENT: {
            const auto* n = static_cast<const ForStatement*>(node);
            return assigns_to_identifier(n->get_init(), name) ||
                   assigns_to_identifier(n->get_test(), name) ||
                   assigns_to_identifier(n->get_update(), name) ||
                   assigns_to_identifier(n->get_body(), name);
        }
        case ASTNode::Type::FOR_OF_STATEMENT: {
            const auto* n = static_cast<const ForOfStatement*>(node);
            return head_target_assigns(n->get_left(), n->get_left_decl_kind(), name) ||
                   assigns_to_identifier(n->get_right(), name) ||
                   assigns_to_identifier(n->get_body(), name);
        }
        case ASTNode::Type::FOR_IN_STATEMENT: {
            const auto* n = static_cast<const ForInStatement*>(node);
            return head_target_assigns(n->get_left(), n->get_left_decl_kind(), name) ||
                   assigns_to_identifier(n->get_right(), name) ||
                   assigns_to_identifier(n->get_body(), name);
        }
        case ASTNode::Type::TRY_STATEMENT: {
            const auto* n = static_cast<const TryStatement*>(node);
            if (assigns_to_identifier(n->get_try_block(), name)) return true;
            if (const ASTNode* cc = n->get_catch_clause()) {
                if (assigns_to_identifier(static_cast<const CatchClause*>(cc)->get_body(), name)) return true;
            }
            return assigns_to_identifier(n->get_finally_block(), name);
        }
        case ASTNode::Type::SWITCH_STATEMENT: {
            const auto* n = static_cast<const SwitchStatement*>(node);
            if (assigns_to_identifier(n->get_discriminant(), name)) return true;
            for (const auto& c : n->get_cases()) {
                const auto* cc = static_cast<const CaseClause*>(c.get());
                if (cc->get_test() && assigns_to_identifier(cc->get_test(), name)) return true;
                for (const auto& s : cc->get_consequent()) {
                    if (assigns_to_identifier(s.get(), name)) return true;
                }
            }
            return false;
        }
        case ASTNode::Type::LABELED_STATEMENT:
            return assigns_to_identifier(
                static_cast<const LabeledStatement*>(node)->get_statement(), name);
        case ASTNode::Type::EXPRESSION_STATEMENT:
            return assigns_to_identifier(
                static_cast<const ExpressionStatement*>(node)->get_expression(), name);
        case ASTNode::Type::RETURN_STATEMENT: {
            const auto* n = static_cast<const ReturnStatement*>(node);
            return n->get_argument() && assigns_to_identifier(n->get_argument(), name);
        }
        case ASTNode::Type::VARIABLE_DECLARATION: {
            const auto* n = static_cast<const VariableDeclaration*>(node);
            for (const auto& d : n->get_declarations()) {
                if (d->get_init() && assigns_to_identifier(d->get_init(), name)) return true;
            }
            return false;
        }
        case ASTNode::Type::ASSIGNMENT_EXPRESSION: {
            const auto* n = static_cast<const AssignmentExpression*>(node);
            const ASTNode* left = n->get_left();
            if (left->get_type() == ASTNode::Type::IDENTIFIER &&
                static_cast<const Identifier*>(left)->get_name() == name) {
                return true;
            }
            // A pattern on the left writes to every name it holds, and those
            // sit where an ordinary literal's values do -- walking the left as
            // an expression would read them as mere mentions.
            if (left->get_type() == ASTNode::Type::ARRAY_LITERAL ||
                left->get_type() == ASTNode::Type::OBJECT_LITERAL) {
                std::vector<std::string> targets;
                collect_pattern_target_names(left, targets);
                for (const auto& t : targets) if (t == name) return true;
            }
            return assigns_to_identifier(left, name) ||
                   assigns_to_identifier(n->get_right(), name);
        }
        case ASTNode::Type::UNARY_EXPRESSION: {
            const auto* n = static_cast<const UnaryExpression*>(node);
            using UnOp = UnaryExpression::Operator;
            auto op = n->get_operator();
            if ((op == UnOp::PRE_INCREMENT || op == UnOp::PRE_DECREMENT ||
                 op == UnOp::POST_INCREMENT || op == UnOp::POST_DECREMENT) &&
                n->get_operand()->get_type() == ASTNode::Type::IDENTIFIER &&
                static_cast<const Identifier*>(n->get_operand())->get_name() == name) {
                return true;
            }
            return assigns_to_identifier(n->get_operand(), name);
        }
        case ASTNode::Type::BINARY_EXPRESSION: {
            const auto* n = static_cast<const BinaryExpression*>(node);
            return assigns_to_identifier(n->get_left(), name) ||
                   assigns_to_identifier(n->get_right(), name);
        }
        case ASTNode::Type::CONDITIONAL_EXPRESSION: {
            const auto* n = static_cast<const ConditionalExpression*>(node);
            return assigns_to_identifier(n->get_test(), name) ||
                   assigns_to_identifier(n->get_consequent(), name) ||
                   assigns_to_identifier(n->get_alternate(), name);
        }
        case ASTNode::Type::CALL_EXPRESSION: {
            const auto* n = static_cast<const CallExpression*>(node);
            if (assigns_to_identifier(n->get_callee(), name)) return true;
            for (const auto& arg : n->get_arguments()) {
                if (assigns_to_identifier(arg.get(), name)) return true;
            }
            return false;
        }
        case ASTNode::Type::MEMBER_EXPRESSION: {
            const auto* n = static_cast<const MemberExpression*>(node);
            return assigns_to_identifier(n->get_object(), name) ||
                   (n->is_computed() && assigns_to_identifier(n->get_property(), name));
        }
        case ASTNode::Type::OBJECT_LITERAL: {
            const auto* n = static_cast<const ObjectLiteral*>(node);
            for (const auto& prop : n->get_properties()) {
                if (prop->value && assigns_to_identifier(prop->value.get(), name)) return true;
            }
            return false;
        }
        case ASTNode::Type::ARRAY_LITERAL: {
            const auto* n = static_cast<const ArrayLiteral*>(node);
            for (const auto& el : n->get_elements()) {
                if (el && assigns_to_identifier(el.get(), name)) return true;
            }
            return false;
        }
        case ASTNode::Type::FUNCTION_EXPRESSION: {
            const auto* n = static_cast<const FunctionExpression*>(node);
            return assigns_to_identifier(n->get_body(), name);
        }
        case ASTNode::Type::ARROW_FUNCTION_EXPRESSION: {
            const auto* n = static_cast<const ArrowFunctionExpression*>(node);
            return assigns_to_identifier(n->get_body(), name);
        }
        case ASTNode::Type::FUNCTION_DECLARATION: {
            const auto* n = static_cast<const FunctionDeclaration*>(node);
            return assigns_to_identifier(n->get_body(), name);
        }
        case ASTNode::Type::ASYNC_FUNCTION_EXPRESSION: {
            const auto* n = static_cast<const AsyncFunctionExpression*>(node);
            return assigns_to_identifier(n->get_body(), name);
        }
        case ASTNode::Type::CLASS_DECLARATION: {
            const auto* n = static_cast<const ClassDeclaration*>(node);
            return assigns_to_identifier(n->get_superclass(), name) ||
                   assigns_to_identifier(n->get_body(), name);
        }
        case ASTNode::Type::METHOD_DEFINITION: {
            const auto* n = static_cast<const MethodDefinition*>(node);
            return (n->is_computed() && assigns_to_identifier(n->get_key(), name)) ||
                   assigns_to_identifier(n->get_value(), name);
        }
        case ASTNode::Type::CLASS_FIELD: {
            const auto* n = static_cast<const ClassField*>(node);
            return (n->is_computed() && assigns_to_identifier(n->get_key(), name)) ||
                   assigns_to_identifier(n->get_value(), name);
        }
        case ASTNode::Type::CLASS_STATIC_BLOCK:
            return assigns_to_identifier(static_cast<const ClassStaticBlock*>(node)->get_body(), name);
        case ASTNode::Type::TEMPLATE_LITERAL: {
            const auto* n = static_cast<const TemplateLiteral*>(node);
            for (const auto& el : n->get_elements()) {
                if (el.expression && assigns_to_identifier(el.expression.get(), name)) return true;
            }
            return false;
        }
        case ASTNode::Type::NULLISH_COALESCING_EXPRESSION: {
            const auto* n = static_cast<const NullishCoalescingExpression*>(node);
            return assigns_to_identifier(n->get_left(), name) ||
                   assigns_to_identifier(n->get_right(), name);
        }
        case ASTNode::Type::NEW_EXPRESSION: {
            const auto* n = static_cast<const NewExpression*>(node);
            if (assigns_to_identifier(n->get_constructor(), name)) return true;
            for (const auto& arg : n->get_arguments()) {
                if (assigns_to_identifier(arg.get(), name)) return true;
            }
            return false;
        }
        case ASTNode::Type::OPTIONAL_CHAINING_EXPRESSION: {
            const auto* n = static_cast<const OptionalChainingExpression*>(node);
            return assigns_to_identifier(n->get_object(), name) ||
                   assigns_to_identifier(n->get_property(), name);
        }
        case ASTNode::Type::SPREAD_ELEMENT:
            return assigns_to_identifier(static_cast<const SpreadElement*>(node)->get_argument(), name);
        case ASTNode::Type::THROW_STATEMENT:
            return assigns_to_identifier(static_cast<const ThrowStatement*>(node)->get_expression(), name);

        // Leaves: nothing inside them to assign through. Everything else falls
        // to the answer below.
        case ASTNode::Type::NUMBER_LITERAL:
        case ASTNode::Type::STRING_LITERAL:
        case ASTNode::Type::BOOLEAN_LITERAL:
        case ASTNode::Type::NULL_LITERAL:
        case ASTNode::Type::BIGINT_LITERAL:
        case ASTNode::Type::UNDEFINED_LITERAL:
        case ASTNode::Type::REGEX_LITERAL:
        case ASTNode::Type::IDENTIFIER:
        case ASTNode::Type::META_PROPERTY:
        case ASTNode::Type::ENGINE_HELPER:
        case ASTNode::Type::EMPTY_STATEMENT:
        case ASTNode::Type::BREAK_STATEMENT:
        case ASTNode::Type::CONTINUE_STATEMENT:
            return false;

        default:
            // A node shape this walk does not know could hold an assignment
            // anywhere inside it, and answering "no" would let a const be given
            // a register and then overwritten in silence. The callers all treat
            // "yes" as a reason to be careful, so an unknown shape says yes.
            return true;
    }
}

// True if `node` contains a nested function/arrow. Forces env_mode (locals in
// a real Environment instead of registers, which die with the call frame).
// The oracle the parser-computed bit is checked against, and the answer
// for any tree the parser did not fill in.
bool contains_closure_by_walk(const ASTNode* node) {
    if (!node) return false;
    switch (node->get_type()) {
        case ASTNode::Type::FUNCTION_EXPRESSION:
        case ASTNode::Type::ARROW_FUNCTION_EXPRESSION:
        case ASTNode::Type::FUNCTION_DECLARATION:
        case ASTNode::Type::ASYNC_FUNCTION_EXPRESSION:
        case ASTNode::Type::CLASS_DECLARATION:  // methods capture the environment
            return true;
        case ASTNode::Type::BLOCK_STATEMENT: {
            const auto* n = static_cast<const BlockStatement*>(node);
            for (const auto& stmt : n->get_statements()) {
                if (contains_closure_by_walk(stmt.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::IF_STATEMENT: {
            const auto* n = static_cast<const IfStatement*>(node);
            return contains_closure_by_walk(n->get_test()) || contains_closure_by_walk(n->get_consequent()) ||
                   contains_closure_by_walk(n->get_alternate());
        }
        case ASTNode::Type::WHILE_STATEMENT: {
            const auto* n = static_cast<const WhileStatement*>(node);
            return contains_closure_by_walk(n->get_test()) || contains_closure_by_walk(n->get_body());
        }
        case ASTNode::Type::DO_WHILE_STATEMENT: {
            const auto* n = static_cast<const DoWhileStatement*>(node);
            return contains_closure_by_walk(n->get_body()) || contains_closure_by_walk(n->get_test());
        }
        case ASTNode::Type::FOR_STATEMENT: {
            const auto* n = static_cast<const ForStatement*>(node);
            return contains_closure_by_walk(n->get_init()) || contains_closure_by_walk(n->get_test()) ||
                   contains_closure_by_walk(n->get_update()) || contains_closure_by_walk(n->get_body());
        }
        case ASTNode::Type::FOR_OF_STATEMENT: {
            const auto* n = static_cast<const ForOfStatement*>(node);
            return contains_closure_by_walk(n->get_right()) || contains_closure_by_walk(n->get_body());
        }
        case ASTNode::Type::FOR_IN_STATEMENT: {
            const auto* n = static_cast<const ForInStatement*>(node);
            return contains_closure_by_walk(n->get_right()) || contains_closure_by_walk(n->get_body());
        }
        case ASTNode::Type::TRY_STATEMENT: {
            const auto* n = static_cast<const TryStatement*>(node);
            if (contains_closure_by_walk(n->get_try_block())) return true;
            if (const ASTNode* cc = n->get_catch_clause()) {
                if (contains_closure_by_walk(static_cast<const CatchClause*>(cc)->get_body())) return true;
            }
            return contains_closure_by_walk(n->get_finally_block());
        }
        case ASTNode::Type::SWITCH_STATEMENT: {
            const auto* n = static_cast<const SwitchStatement*>(node);
            if (contains_closure_by_walk(n->get_discriminant())) return true;
            for (const auto& c : n->get_cases()) {
                const auto* cc = static_cast<const CaseClause*>(c.get());
                if (cc->get_test() && contains_closure_by_walk(cc->get_test())) return true;
                for (const auto& s : cc->get_consequent()) {
                    if (contains_closure_by_walk(s.get())) return true;
                }
            }
            return false;
        }
        case ASTNode::Type::LABELED_STATEMENT:
            return contains_closure_by_walk(static_cast<const LabeledStatement*>(node)->get_statement());
        case ASTNode::Type::EXPRESSION_STATEMENT:
            return contains_closure_by_walk(static_cast<const ExpressionStatement*>(node)->get_expression());
        case ASTNode::Type::RETURN_STATEMENT: {
            const auto* n = static_cast<const ReturnStatement*>(node);
            return n->get_argument() && contains_closure_by_walk(n->get_argument());
        }
        case ASTNode::Type::VARIABLE_DECLARATION: {
            const auto* n = static_cast<const VariableDeclaration*>(node);
            for (const auto& d : n->get_declarations()) {
                if (d->get_init() && contains_closure_by_walk(d->get_init())) return true;
            }
            return false;
        }
        case ASTNode::Type::ASSIGNMENT_EXPRESSION: {
            const auto* n = static_cast<const AssignmentExpression*>(node);
            return contains_closure_by_walk(n->get_left()) || contains_closure_by_walk(n->get_right());
        }
        case ASTNode::Type::UNARY_EXPRESSION:
            return contains_closure_by_walk(static_cast<const UnaryExpression*>(node)->get_operand());
        case ASTNode::Type::BINARY_EXPRESSION: {
            const auto* n = static_cast<const BinaryExpression*>(node);
            return contains_closure_by_walk(n->get_left()) || contains_closure_by_walk(n->get_right());
        }
        case ASTNode::Type::CONDITIONAL_EXPRESSION: {
            const auto* n = static_cast<const ConditionalExpression*>(node);
            return contains_closure_by_walk(n->get_test()) || contains_closure_by_walk(n->get_consequent()) ||
                   contains_closure_by_walk(n->get_alternate());
        }
        case ASTNode::Type::CALL_EXPRESSION: {
            const auto* n = static_cast<const CallExpression*>(node);
            if (contains_closure_by_walk(n->get_callee())) return true;
            for (const auto& arg : n->get_arguments()) {
                if (contains_closure_by_walk(arg.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::MEMBER_EXPRESSION: {
            const auto* n = static_cast<const MemberExpression*>(node);
            return contains_closure_by_walk(n->get_object()) ||
                   (n->is_computed() && contains_closure_by_walk(n->get_property()));
        }
        case ASTNode::Type::OBJECT_LITERAL: {
            const auto* n = static_cast<const ObjectLiteral*>(node);
            for (const auto& prop : n->get_properties()) {
                // Rare: a closure used AS a computed key (e.g. an IIFE),
                // independent of whether the property's own value is one.
                if (prop->computed && prop->key && contains_closure_by_walk(prop->key.get())) return true;
                if (prop->value && contains_closure_by_walk(prop->value.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::ARRAY_LITERAL: {
            const auto* n = static_cast<const ArrayLiteral*>(node);
            for (const auto& el : n->get_elements()) {
                if (el && contains_closure_by_walk(el.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::TEMPLATE_LITERAL: {
            const auto* n = static_cast<const TemplateLiteral*>(node);
            for (const auto& el : n->get_elements()) {
                if (el.expression && contains_closure_by_walk(el.expression.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::NULLISH_COALESCING_EXPRESSION: {
            const auto* n = static_cast<const NullishCoalescingExpression*>(node);
            return contains_closure_by_walk(n->get_left()) || contains_closure_by_walk(n->get_right());
        }
        case ASTNode::Type::NEW_EXPRESSION: {
            const auto* n = static_cast<const NewExpression*>(node);
            if (contains_closure_by_walk(n->get_constructor())) return true;
            for (const auto& arg : n->get_arguments()) {
                if (contains_closure_by_walk(arg.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::OPTIONAL_CHAINING_EXPRESSION: {
            const auto* n = static_cast<const OptionalChainingExpression*>(node);
            return contains_closure_by_walk(n->get_object()) || contains_closure_by_walk(n->get_property());
        }
        case ASTNode::Type::SPREAD_ELEMENT:
            return contains_closure_by_walk(static_cast<const SpreadElement*>(node)->get_argument());
        case ASTNode::Type::THROW_STATEMENT:
            return contains_closure_by_walk(static_cast<const ThrowStatement*>(node)->get_expression());

        // Leaves: nothing inside them to hold a closure.
        case ASTNode::Type::NUMBER_LITERAL:
        case ASTNode::Type::STRING_LITERAL:
        case ASTNode::Type::BOOLEAN_LITERAL:
        case ASTNode::Type::NULL_LITERAL:
        case ASTNode::Type::BIGINT_LITERAL:
        case ASTNode::Type::UNDEFINED_LITERAL:
        case ASTNode::Type::REGEX_LITERAL:
        case ASTNode::Type::IDENTIFIER:
        case ASTNode::Type::META_PROPERTY:
        case ASTNode::Type::ENGINE_HELPER:
        case ASTNode::Type::EMPTY_STATEMENT:
        case ASTNode::Type::BREAK_STATEMENT:
        case ASTNode::Type::CONTINUE_STATEMENT:
            return false;

        case ASTNode::Type::DESTRUCTURING_ASSIGNMENT: {
            // The pattern literal is the whole pattern: every default and every
            // computed key sits in it, at any depth, as an ordinary expression.
            const auto* n = static_cast<const DestructuringAssignment*>(node);
            return contains_closure_by_walk(n->get_source()) ||
                   contains_closure_by_walk(n->get_pattern_literal());
        }
        case ASTNode::Type::PARAMETER: {
            const auto* n = static_cast<const Parameter*>(node);
            return contains_closure_by_walk(n->get_default_value()) ||
                   contains_closure_by_walk(n->get_destructuring_pattern());
        }
        case ASTNode::Type::VARIABLE_DECLARATOR:
            return contains_closure_by_walk(static_cast<const VariableDeclarator*>(node)->get_init());
        case ASTNode::Type::AWAIT_EXPRESSION:
            return contains_closure_by_walk(static_cast<const AwaitExpression*>(node)->get_argument());
        case ASTNode::Type::YIELD_EXPRESSION:
            return contains_closure_by_walk(static_cast<const YieldExpression*>(node)->get_argument());
        case ASTNode::Type::WITH_STATEMENT: {
            const auto* n = static_cast<const WithStatement*>(node);
            return contains_closure_by_walk(n->get_object()) || contains_closure_by_walk(n->get_body());
        }
        case ASTNode::Type::CATCH_CLAUSE: {
            const auto* n = static_cast<const CatchClause*>(node);
            return contains_closure_by_walk(n->get_destructuring_pattern()) ||
                   contains_closure_by_walk(n->get_body());
        }
        case ASTNode::Type::CASE_CLAUSE: {
            const auto* n = static_cast<const CaseClause*>(node);
            if (contains_closure_by_walk(n->get_test())) return true;
            for (const auto& st : n->get_consequent()) {
                if (contains_closure_by_walk(st.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::USING_DECLARATION: {
            const auto* n = static_cast<const UsingDeclaration*>(node);
            for (const auto& b : n->get_bindings()) {
                if (contains_closure_by_walk(b.initializer.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::PROGRAM: {
            for (const auto& st : static_cast<const Program*>(node)->get_statements()) {
                if (contains_closure_by_walk(st.get())) return true;
            }
            return false;
        }

        default:
            // An unknown shape could hold a closure, and answering "no" leaves
            // env_mode off, which is what every closure-emitting instruction
            // refuses to run without -- the whole function goes to the
            // tree-walker instead. Saying yes only costs its registers.
            return true;
    }
}

bool contains_closure(const ASTNode* node) {
    if (!node) return false;
    const uint32_t flags = node->subtree_flags();
    if (!(flags & kSubtreeComputed)) return contains_closure_by_walk(node);
    const bool bit = (flags & kSubtreeClosure) != 0;
#ifdef QUANTA_VALIDATE_BYTECODE
    if (std::getenv("QUANTA_SUBTREE_CHECK")) {
        const bool walked = contains_closure_by_walk(node);
        if (bit != walked) {
            std::fprintf(stderr, "[subtree] closure bit=%d walk=%d node_type=%d\n",
                         bit ? 1 : 0, walked ? 1 : 0, static_cast<int>(node->get_type()));
        }
    }
#endif
    return bit;
}

bool has_spread(const std::vector<std::unique_ptr<ASTNode>>& nodes) {
    for (const auto& n : nodes) {
        if (n && n->get_type() == ASTNode::Type::SPREAD_ELEMENT) return true;
    }
    return false;
}

// Spread compiles everywhere except `super(...)`, which still needs the
// derived-constructor ceremony and delegates (see compile_expression). The
// three walkers below predate that: they treated ANY spread as a delegate,
// which forced its operands to be environment-resident and demoted the whole
// containing function out of register mode.
bool spread_call_delegates(const CallExpression* n) {
    if (!has_spread(n->get_arguments())) return false;
    const ASTNode* callee = n->get_callee();
    return callee && callee->get_type() == ASTNode::Type::IDENTIFIER &&
           static_cast<const Identifier*>(callee)->get_name() == "super";
}

// Anything the native path can't emit: spread, non-computed __proto__ (the
// [[Prototype]]-setting form -- computed/shorthand __proto__ is just a plain
// data property, handled like any other computed key), computed-key
// Getter/Setter (excluded on purpose: merging two independently-computed
// keys' converted values into one accessor descriptor, should they happen to
// be runtime-equal, isn't worth the complexity here), oversized literals.
// Static/computed Value and Method properties and static-key Getter/Setter
// all have native codegen (see the OBJECT_LITERAL case below).
bool object_literal_is_complex(const ObjectLiteral* lit) {
    for (const auto& prop : lit->get_properties()) {
        // A spread has a null key and is emitted by Op::ObjectSpreadInto.
        if (!prop->key) continue;
        if (prop->computed) continue;
        auto kt = prop->key->get_type();
        if (kt != ASTNode::Type::IDENTIFIER && kt != ASTNode::Type::STRING_LITERAL &&
            kt != ASTNode::Type::NUMBER_LITERAL) {
            return true;
        }
    }
    // The only structural limit is Op::CreateObject's u16 property count;
    // each property's key/value temps are freed within its own iteration, so
    // a long literal costs no more registers than a short one.
    return lit->get_properties().size() > 0xFFFF;
}

// True if an always-delegated expression -- a bare destructuring assignment
// (`[a,b]=[b,a];`, not a declaration's init), a complex object literal, or a
// spread in call/new/array-literal position -- appears anywhere: those need
// env_mode to delegate at all. Same arrow/function descent rule as
// uses_arguments.
bool contains_delegated_expr(const ASTNode* node) {
    if (!node) return false;
    if (node->get_type() == ASTNode::Type::DESTRUCTURING_ASSIGNMENT) return true;
    switch (node->get_type()) {
        case ASTNode::Type::FUNCTION_EXPRESSION:
        case ASTNode::Type::FUNCTION_DECLARATION:
            return false;
        case ASTNode::Type::ARROW_FUNCTION_EXPRESSION:
            return contains_delegated_expr(static_cast<const ArrowFunctionExpression*>(node)->get_body());
        case ASTNode::Type::ASYNC_FUNCTION_EXPRESSION: {
            const auto* n = static_cast<const AsyncFunctionExpression*>(node);
            return n->is_arrow() && contains_delegated_expr(n->get_body());
        }
        case ASTNode::Type::CLASS_DECLARATION:
            return false;  // method bodies are their own compile() unit
        case ASTNode::Type::BLOCK_STATEMENT: {
            const auto* n = static_cast<const BlockStatement*>(node);
            for (const auto& stmt : n->get_statements()) {
                if (contains_delegated_expr(stmt.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::IF_STATEMENT: {
            const auto* n = static_cast<const IfStatement*>(node);
            return contains_delegated_expr(n->get_test()) || contains_delegated_expr(n->get_consequent()) ||
                   contains_delegated_expr(n->get_alternate());
        }
        case ASTNode::Type::WHILE_STATEMENT: {
            const auto* n = static_cast<const WhileStatement*>(node);
            return contains_delegated_expr(n->get_test()) || contains_delegated_expr(n->get_body());
        }
        case ASTNode::Type::DO_WHILE_STATEMENT: {
            const auto* n = static_cast<const DoWhileStatement*>(node);
            return contains_delegated_expr(n->get_body()) || contains_delegated_expr(n->get_test());
        }
        case ASTNode::Type::FOR_STATEMENT: {
            const auto* n = static_cast<const ForStatement*>(node);
            return contains_delegated_expr(n->get_init()) || contains_delegated_expr(n->get_test()) ||
                   contains_delegated_expr(n->get_update()) || contains_delegated_expr(n->get_body());
        }
        case ASTNode::Type::FOR_OF_STATEMENT: {
            const auto* n = static_cast<const ForOfStatement*>(node);
            return contains_delegated_expr(n->get_right()) || contains_delegated_expr(n->get_body());
        }
        case ASTNode::Type::FOR_IN_STATEMENT: {
            const auto* n = static_cast<const ForInStatement*>(node);
            return contains_delegated_expr(n->get_right()) || contains_delegated_expr(n->get_body());
        }
        case ASTNode::Type::TRY_STATEMENT: {
            const auto* n = static_cast<const TryStatement*>(node);
            if (contains_delegated_expr(n->get_try_block())) return true;
            if (const ASTNode* cc = n->get_catch_clause()) {
                if (contains_delegated_expr(static_cast<const CatchClause*>(cc)->get_body())) return true;
            }
            return contains_delegated_expr(n->get_finally_block());
        }
        case ASTNode::Type::SWITCH_STATEMENT: {
            const auto* n = static_cast<const SwitchStatement*>(node);
            if (contains_delegated_expr(n->get_discriminant())) return true;
            for (const auto& c : n->get_cases()) {
                const auto* cc = static_cast<const CaseClause*>(c.get());
                if (cc->get_test() && contains_delegated_expr(cc->get_test())) return true;
                for (const auto& s : cc->get_consequent()) {
                    if (contains_delegated_expr(s.get())) return true;
                }
            }
            return false;
        }
        case ASTNode::Type::LABELED_STATEMENT:
            return contains_delegated_expr(static_cast<const LabeledStatement*>(node)->get_statement());
        case ASTNode::Type::EXPRESSION_STATEMENT:
            return contains_delegated_expr(static_cast<const ExpressionStatement*>(node)->get_expression());
        case ASTNode::Type::RETURN_STATEMENT: {
            const auto* n = static_cast<const ReturnStatement*>(node);
            return n->get_argument() && contains_delegated_expr(n->get_argument());
        }
        case ASTNode::Type::THROW_STATEMENT:
            return contains_delegated_expr(static_cast<const ThrowStatement*>(node)->get_expression());
        case ASTNode::Type::VARIABLE_DECLARATION: {
            const auto* n = static_cast<const VariableDeclaration*>(node);
            for (const auto& d : n->get_declarations()) {
                // A declaration's own init is contains_destructuring's job;
                // only destructuring exprs nested inside it count here.
                if (d->get_init() && d->get_init()->get_type() != ASTNode::Type::DESTRUCTURING_ASSIGNMENT &&
                    contains_delegated_expr(d->get_init())) return true;
                if (d->get_init() && d->get_init()->get_type() == ASTNode::Type::DESTRUCTURING_ASSIGNMENT) {
                    const auto* da = static_cast<const DestructuringAssignment*>(d->get_init());
                    if (contains_delegated_expr(da->get_source())) return true;
                    bool found = false;
                    da->for_each_expression([&](const ASTNode* e) {
                        if (!found && contains_delegated_expr(e)) found = true;
                    });
                    if (found) return true;
                }
            }
            return false;
        }
        case ASTNode::Type::ASSIGNMENT_EXPRESSION: {
            // `[a,b]=[b,a];` parses as an array/object-literal LHS, not a
            // DESTRUCTURING_ASSIGNMENT node.
            const auto* n = static_cast<const AssignmentExpression*>(node);
            if (n->get_left()->get_type() == ASTNode::Type::ARRAY_LITERAL ||
                n->get_left()->get_type() == ASTNode::Type::OBJECT_LITERAL) {
                return true;
            }
            return contains_delegated_expr(n->get_left()) || contains_delegated_expr(n->get_right());
        }
        case ASTNode::Type::UNARY_EXPRESSION:
            return contains_delegated_expr(static_cast<const UnaryExpression*>(node)->get_operand());
        case ASTNode::Type::BINARY_EXPRESSION: {
            const auto* n = static_cast<const BinaryExpression*>(node);
            return contains_delegated_expr(n->get_left()) || contains_delegated_expr(n->get_right());
        }
        case ASTNode::Type::NULLISH_COALESCING_EXPRESSION: {
            const auto* n = static_cast<const NullishCoalescingExpression*>(node);
            return contains_delegated_expr(n->get_left()) || contains_delegated_expr(n->get_right());
        }
        case ASTNode::Type::CONDITIONAL_EXPRESSION: {
            const auto* n = static_cast<const ConditionalExpression*>(node);
            return contains_delegated_expr(n->get_test()) || contains_delegated_expr(n->get_consequent()) ||
                   contains_delegated_expr(n->get_alternate());
        }
        case ASTNode::Type::CALL_EXPRESSION: {
            const auto* n = static_cast<const CallExpression*>(node);
            if (spread_call_delegates(n)) return true;
            if (contains_delegated_expr(n->get_callee())) return true;
            for (const auto& arg : n->get_arguments()) {
                if (contains_delegated_expr(arg.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::NEW_EXPRESSION: {
            const auto* n = static_cast<const NewExpression*>(node);
            if (contains_delegated_expr(n->get_constructor())) return true;
            for (const auto& arg : n->get_arguments()) {
                if (contains_delegated_expr(arg.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::MEMBER_EXPRESSION: {
            const auto* n = static_cast<const MemberExpression*>(node);
            return contains_delegated_expr(n->get_object()) ||
                   (n->is_computed() && contains_delegated_expr(n->get_property()));
        }
        case ASTNode::Type::OPTIONAL_CHAINING_EXPRESSION: {
            const auto* n = static_cast<const OptionalChainingExpression*>(node);
            return contains_delegated_expr(n->get_object()) ||
                   (n->is_computed() && contains_delegated_expr(n->get_property()));
        }
        case ASTNode::Type::SPREAD_ELEMENT:
            return contains_delegated_expr(static_cast<const SpreadElement*>(node)->get_argument());
        case ASTNode::Type::TEMPLATE_LITERAL: {
            const auto* n = static_cast<const TemplateLiteral*>(node);
            for (const auto& el : n->get_elements()) {
                if (el.type == TemplateLiteral::Element::Type::EXPRESSION &&
                    contains_delegated_expr(el.expression.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::OBJECT_LITERAL: {
            const auto* n = static_cast<const ObjectLiteral*>(node);
            if (object_literal_is_complex(n)) return true;
            for (const auto& prop : n->get_properties()) {
                // A computed key's own expression can itself be an always-
                // delegated form (e.g. a destructuring assignment) -- now that
                // object_literal_is_complex no longer blanket-excludes every
                // computed key, this can't rely on that short circuit anymore.
                if (prop->computed && prop->key && contains_delegated_expr(prop->key.get())) return true;
                if (prop->value && contains_delegated_expr(prop->value.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::ARRAY_LITERAL: {
            const auto* n = static_cast<const ArrayLiteral*>(node);
            for (const auto& el : n->get_elements()) {
                if (el && contains_delegated_expr(el.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::YIELD_EXPRESSION: {
            const auto* n = static_cast<const YieldExpression*>(node);
            return n->get_argument() && contains_delegated_expr(n->get_argument());
        }
        case ASTNode::Type::AWAIT_EXPRESSION: {
            const auto* n = static_cast<const AwaitExpression*>(node);
            return n->get_argument() && contains_delegated_expr(n->get_argument());
        }
        default:
            return false;
    }
}

// True if `node` references the enclosing function's `arguments`: descends
// into arrow bodies (arrows share it) but not into nested regular functions
// (they get their own).
// The oracle the parser-computed bit is checked against, and the answer
// for any tree the parser did not fill in.
bool uses_arguments_by_walk(const ASTNode* node) {
    if (!node) return false;
    switch (node->get_type()) {
        case ASTNode::Type::IDENTIFIER:
            return static_cast<const Identifier*>(node)->get_name() == "arguments";
        case ASTNode::Type::FUNCTION_EXPRESSION:
        case ASTNode::Type::FUNCTION_DECLARATION:
            return false;
        case ASTNode::Type::ARROW_FUNCTION_EXPRESSION: {
            // An arrow has no `arguments` of its own, so one written here reads
            // this function's -- and a body stepped over is one this cannot be
            // asked about, which is not the same as one that does not.
            const auto* n = static_cast<const ArrowFunctionExpression*>(node);
            return !n->get_body() || uses_arguments_by_walk(n->get_body());
        }
        case ASTNode::Type::ASYNC_FUNCTION_EXPRESSION: {
            // Async arrows share the enclosing arguments; async functions own theirs.
            const auto* n = static_cast<const AsyncFunctionExpression*>(node);
            return n->is_arrow() && (!n->get_body() || uses_arguments_by_walk(n->get_body()));
        }
        case ASTNode::Type::CLASS_DECLARATION: {
            // Superclass expressions and computed keys evaluate in the
            // enclosing scope; method bodies get their own arguments.
            const auto* n = static_cast<const ClassDeclaration*>(node);
            return uses_arguments_by_walk(n->get_superclass()) || uses_arguments_by_walk(n->get_body());
        }
        case ASTNode::Type::METHOD_DEFINITION: {
            const auto* n = static_cast<const MethodDefinition*>(node);
            return n->is_computed() && uses_arguments_by_walk(n->get_key());
        }
        case ASTNode::Type::CLASS_FIELD: {
            const auto* n = static_cast<const ClassField*>(node);
            return n->is_computed() && uses_arguments_by_walk(n->get_key());
        }
        case ASTNode::Type::BLOCK_STATEMENT: {
            const auto* n = static_cast<const BlockStatement*>(node);
            for (const auto& stmt : n->get_statements()) {
                if (uses_arguments_by_walk(stmt.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::IF_STATEMENT: {
            const auto* n = static_cast<const IfStatement*>(node);
            return uses_arguments_by_walk(n->get_test()) || uses_arguments_by_walk(n->get_consequent()) ||
                   uses_arguments_by_walk(n->get_alternate());
        }
        case ASTNode::Type::WHILE_STATEMENT: {
            const auto* n = static_cast<const WhileStatement*>(node);
            return uses_arguments_by_walk(n->get_test()) || uses_arguments_by_walk(n->get_body());
        }
        case ASTNode::Type::DO_WHILE_STATEMENT: {
            const auto* n = static_cast<const DoWhileStatement*>(node);
            return uses_arguments_by_walk(n->get_body()) || uses_arguments_by_walk(n->get_test());
        }
        case ASTNode::Type::FOR_STATEMENT: {
            const auto* n = static_cast<const ForStatement*>(node);
            return uses_arguments_by_walk(n->get_init()) || uses_arguments_by_walk(n->get_test()) ||
                   uses_arguments_by_walk(n->get_update()) || uses_arguments_by_walk(n->get_body());
        }
        case ASTNode::Type::FOR_OF_STATEMENT: {
            const auto* n = static_cast<const ForOfStatement*>(node);
            return uses_arguments_by_walk(n->get_right()) || uses_arguments_by_walk(n->get_body());
        }
        case ASTNode::Type::FOR_IN_STATEMENT: {
            const auto* n = static_cast<const ForInStatement*>(node);
            return uses_arguments_by_walk(n->get_right()) || uses_arguments_by_walk(n->get_body());
        }
        case ASTNode::Type::TRY_STATEMENT: {
            const auto* n = static_cast<const TryStatement*>(node);
            if (uses_arguments_by_walk(n->get_try_block())) return true;
            if (const ASTNode* cc = n->get_catch_clause()) {
                if (uses_arguments_by_walk(static_cast<const CatchClause*>(cc)->get_body())) return true;
            }
            return uses_arguments_by_walk(n->get_finally_block());
        }
        case ASTNode::Type::SWITCH_STATEMENT: {
            const auto* n = static_cast<const SwitchStatement*>(node);
            if (uses_arguments_by_walk(n->get_discriminant())) return true;
            for (const auto& c : n->get_cases()) {
                const auto* cc = static_cast<const CaseClause*>(c.get());
                if (cc->get_test() && uses_arguments_by_walk(cc->get_test())) return true;
                for (const auto& s : cc->get_consequent()) {
                    if (uses_arguments_by_walk(s.get())) return true;
                }
            }
            return false;
        }
        case ASTNode::Type::LABELED_STATEMENT:
            return uses_arguments_by_walk(static_cast<const LabeledStatement*>(node)->get_statement());
        case ASTNode::Type::EXPRESSION_STATEMENT:
            return uses_arguments_by_walk(static_cast<const ExpressionStatement*>(node)->get_expression());
        case ASTNode::Type::RETURN_STATEMENT: {
            const auto* n = static_cast<const ReturnStatement*>(node);
            return n->get_argument() && uses_arguments_by_walk(n->get_argument());
        }
        case ASTNode::Type::THROW_STATEMENT:
            return uses_arguments_by_walk(static_cast<const ThrowStatement*>(node)->get_expression());
        case ASTNode::Type::VARIABLE_DECLARATION: {
            const auto* n = static_cast<const VariableDeclaration*>(node);
            for (const auto& d : n->get_declarations()) {
                if (d->get_init() && uses_arguments_by_walk(d->get_init())) return true;
            }
            return false;
        }
        case ASTNode::Type::DESTRUCTURING_ASSIGNMENT: {
            const auto* n = static_cast<const DestructuringAssignment*>(node);
            if (n->get_source() && uses_arguments_by_walk(n->get_source())) return true;
            bool found = false;
            n->for_each_expression([&](const ASTNode* e) { if (!found && uses_arguments_by_walk(e)) found = true; });
            return found;
        }
        case ASTNode::Type::ASSIGNMENT_EXPRESSION: {
            const auto* n = static_cast<const AssignmentExpression*>(node);
            return uses_arguments_by_walk(n->get_left()) || uses_arguments_by_walk(n->get_right());
        }
        case ASTNode::Type::UNARY_EXPRESSION:
            return uses_arguments_by_walk(static_cast<const UnaryExpression*>(node)->get_operand());
        case ASTNode::Type::BINARY_EXPRESSION: {
            const auto* n = static_cast<const BinaryExpression*>(node);
            return uses_arguments_by_walk(n->get_left()) || uses_arguments_by_walk(n->get_right());
        }
        case ASTNode::Type::NULLISH_COALESCING_EXPRESSION: {
            const auto* n = static_cast<const NullishCoalescingExpression*>(node);
            return uses_arguments_by_walk(n->get_left()) || uses_arguments_by_walk(n->get_right());
        }
        case ASTNode::Type::CONDITIONAL_EXPRESSION: {
            const auto* n = static_cast<const ConditionalExpression*>(node);
            return uses_arguments_by_walk(n->get_test()) || uses_arguments_by_walk(n->get_consequent()) ||
                   uses_arguments_by_walk(n->get_alternate());
        }
        case ASTNode::Type::CALL_EXPRESSION: {
            const auto* n = static_cast<const CallExpression*>(node);
            if (uses_arguments_by_walk(n->get_callee())) return true;
            for (const auto& arg : n->get_arguments()) {
                if (uses_arguments_by_walk(arg.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::NEW_EXPRESSION: {
            const auto* n = static_cast<const NewExpression*>(node);
            if (uses_arguments_by_walk(n->get_constructor())) return true;
            for (const auto& arg : n->get_arguments()) {
                if (uses_arguments_by_walk(arg.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::MEMBER_EXPRESSION: {
            // `x.arguments` is a property NAME, not a use -- only the object
            // side (and a computed key) counts.
            const auto* n = static_cast<const MemberExpression*>(node);
            return uses_arguments_by_walk(n->get_object()) ||
                   (n->is_computed() && uses_arguments_by_walk(n->get_property()));
        }
        case ASTNode::Type::OPTIONAL_CHAINING_EXPRESSION: {
            const auto* n = static_cast<const OptionalChainingExpression*>(node);
            return uses_arguments_by_walk(n->get_object()) ||
                   (n->is_computed() && uses_arguments_by_walk(n->get_property()));
        }
        case ASTNode::Type::SPREAD_ELEMENT:
            return uses_arguments_by_walk(static_cast<const SpreadElement*>(node)->get_argument());
        case ASTNode::Type::TEMPLATE_LITERAL: {
            const auto* n = static_cast<const TemplateLiteral*>(node);
            for (const auto& el : n->get_elements()) {
                if (el.type == TemplateLiteral::Element::Type::EXPRESSION &&
                    uses_arguments_by_walk(el.expression.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::OBJECT_LITERAL: {
            const auto* n = static_cast<const ObjectLiteral*>(node);
            for (const auto& prop : n->get_properties()) {
                if (prop->value && uses_arguments_by_walk(prop->value.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::ARRAY_LITERAL: {
            const auto* n = static_cast<const ArrayLiteral*>(node);
            for (const auto& el : n->get_elements()) {
                if (el && uses_arguments_by_walk(el.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::YIELD_EXPRESSION: {
            const auto* n = static_cast<const YieldExpression*>(node);
            return n->get_argument() && uses_arguments_by_walk(n->get_argument());
        }
        case ASTNode::Type::AWAIT_EXPRESSION: {
            const auto* n = static_cast<const AwaitExpression*>(node);
            return n->get_argument() && uses_arguments_by_walk(n->get_argument());
        }
        case ASTNode::Type::WITH_STATEMENT: {
            // The body has to be walked even though `with` can shadow the name:
            // whether the object supplies `arguments` is only knowable at run
            // time, so the binding must exist either way.
            const auto* n = static_cast<const WithStatement*>(node);
            return uses_arguments_by_walk(n->get_object()) || uses_arguments_by_walk(n->get_body());
        }
        case ASTNode::Type::USING_DECLARATION: {
            const auto* n = static_cast<const UsingDeclaration*>(node);
            for (const auto& b : n->get_bindings()) {
                if (uses_arguments_by_walk(b.initializer.get())) return true;
            }
            return false;
        }

        // Leaves and nodes whose children are reached through their parent's
        // case above (a CatchClause body via TRY_STATEMENT, a CaseClause's
        // statements via SWITCH_STATEMENT), plus the forms that cannot appear
        // inside a function body at all.
        case ASTNode::Type::NUMBER_LITERAL:
        case ASTNode::Type::STRING_LITERAL:
        case ASTNode::Type::BOOLEAN_LITERAL:
        case ASTNode::Type::NULL_LITERAL:
        case ASTNode::Type::BIGINT_LITERAL:
        case ASTNode::Type::UNDEFINED_LITERAL:
        case ASTNode::Type::REGEX_LITERAL:
        case ASTNode::Type::EMPTY_STATEMENT:
        case ASTNode::Type::BREAK_STATEMENT:
        case ASTNode::Type::CONTINUE_STATEMENT:
        case ASTNode::Type::META_PROPERTY:
        case ASTNode::Type::ENGINE_HELPER:
        case ASTNode::Type::PARAMETER:
        case ASTNode::Type::VARIABLE_DECLARATOR:
        case ASTNode::Type::CATCH_CLAUSE:
        case ASTNode::Type::CASE_CLAUSE:
        // A class static block is evaluated with its own binding set, and the
        // spec makes naming `arguments` in one an early error.
        case ASTNode::Type::CLASS_STATIC_BLOCK:
        case ASTNode::Type::PROGRAM:
        case ASTNode::Type::IMPORT_STATEMENT:
        case ASTNode::Type::EXPORT_STATEMENT:
        case ASTNode::Type::IMPORT_SPECIFIER:
        case ASTNode::Type::EXPORT_SPECIFIER:
        case ASTNode::Type::JSX_ELEMENT:
        case ASTNode::Type::JSX_TEXT:
        case ASTNode::Type::JSX_EXPRESSION:
        case ASTNode::Type::JSX_ATTRIBUTE:
            return false;

        // Anything not named above is assumed to reach `arguments`. Callers
        // skip materializing the object on a false, and the fallback for a
        // form this walk has never seen has to be the slow, correct one --
        // a `with` body used to land here and answer false, which left the
        // binding missing entirely.
        default:
            return true;
    }
}

bool uses_arguments(const ASTNode* node) {
    if (!node) return false;
    const uint32_t flags = node->subtree_flags();
    if (!(flags & kSubtreeComputed)) return uses_arguments_by_walk(node);
    const bool bit = (flags & kSubtreeArguments) != 0;
#ifdef QUANTA_VALIDATE_BYTECODE
    if (std::getenv("QUANTA_SUBTREE_CHECK")) {
        const bool walked = uses_arguments_by_walk(node);
        if (bit != walked) {
            std::fprintf(stderr, "[subtree] uses_arguments bit=%d walk=%d satir=%u\n",
                         bit ? 1 : 0, walked ? 1 : 0, node->get_start().line);
        }
    }
#endif
    return bit;
}

}

bool BytecodeCompiler::references_arguments(const ASTNode* node) {
    return uses_arguments(node);
}

namespace {

// What a scan could not see through. Over-collection is harmless (a register
// candidate merely stays in the environment); under-collection is a
// correctness bug, so every caller asks one question -- opaque() -- rather
// than remembering which flags exist. A caller that only checked some of them
// is what let a method whose body reaches `super` through a direct eval keep
// being treated as super-free.
struct ScanOpacity {
    bool saw_eval = false;   // a direct eval's text can name anything
    bool saw_class = false;  // a class body this scan does not enter
    bool unknown = false;    // a node shape outside the switch
    // Policy rather than opacity, but it rides here because this is the one
    // thing already threaded through every recursive step: whether a `yield`
    // (or `await`) counts as a closure boundary. A suspension does not capture
    // anything -- an arrow inside the yielded expression still marks itself
    // through its own case -- so the one caller asking purely about CLOSURE
    // observability (loop_vars_may_be_captured, i.e. whether per-iteration
    // loop environments are needed at all) sets this. Every other caller
    // leaves it false and keeps the conservative reading, where a name read
    // across a suspension counts as escaping and stays environment-resident.
    bool yield_transparent = false;
    bool opaque() const { return saw_eval || saw_class || unknown; }
};

// Selective env_mode capture analysis. Collects every identifier that
// occurs inside a closure-creating node (function/arrow/async): if such a
// name is one of this function's locals, it must stay Environment-resident.
// Over-collection is harmless (a register candidate merely stays in the
// env); under-collection is a correctness bug, so anything this scanner
// cannot see through reports a fallback flag and the caller compiles the
// whole function in full env_mode:
//  - saw_eval: a closure mentions `eval` -- its program text can reference
//    any local invisibly.
//  - saw_class: class bodies (methods, fields, heritage) are not traversed
//    here yet.
//  - unknown: an AST node type outside this switch.
void collect_closure_names(const ASTNode* node, bool inside_closure,
                           std::unordered_set<std::string>& out,
                           ScanOpacity& op,
                           bool suspendable = false, bool* super_only = nullptr) {
    // `super_only` turns the name set off: the caller that asks for a whole
    // body with inside_closure set does so to learn three flags and whether
    // `super` appears, and building a set of every identifier to answer one
    // membership question is most of what that walk costs.
    auto add_name = [&](const std::string& n) {
        if (super_only) { if (n == "super") *super_only = true; return; }
        out.insert(n);
    };
    // A body stepped over is one this walk cannot see through directly, but
    // the parse already answered this exact question once, while the body
    // was still in hand, and filed it under the same source position a
    // stepped-over body carries forward (see captures_outer/super_anywhere
    // -- same idiom, generalized to the other two flags this scan also
    // needs). Only a body with no such record at all falls back to unknown.
    auto try_recorded_scope = [&](auto* n) {
        ScriptUnit* unit = n->owning_unit();
        const BodyScopeInfo* info = unit ? unit->scope_info_at(n->body_source_first()) : nullptr;
        if (!info) return false;
        if (super_only) {
            if (info->super_anywhere) *super_only = true;
        } else {
            // all_names, not captured: captured is THIS body's own "what do
            // things nested inside me reach for" answer, but the caller here
            // is asking the other question -- what THIS body itself reaches
            // for, since collect_closure_names just walked in from outside
            // it. A direct `return i;` is exactly that case: nothing nested
            // in this function captures `i`, but the function itself does.
            for (const auto& name : info->all_names) out.insert(name);
        }
        if (info->eval_anywhere) op.saw_eval = true;
        if (info->class_expression) op.saw_class = true;
        return true;
    };
    if (!node) return;
    auto walk_params = [&](const std::vector<std::unique_ptr<Parameter>>& ps) {
        for (const auto& p : ps) {
            if (p->has_default())
                collect_closure_names(p->get_default_value(), true, out, op, suspendable, super_only);
            if (p->has_destructuring())
                collect_closure_names(p->get_destructuring_pattern(), true, out, op, suspendable, super_only);
        }
    };
    switch (node->get_type()) {
        case ASTNode::Type::NUMBER_LITERAL:
        case ASTNode::Type::STRING_LITERAL:
        case ASTNode::Type::BOOLEAN_LITERAL:
        case ASTNode::Type::NULL_LITERAL:
        case ASTNode::Type::UNDEFINED_LITERAL:
        case ASTNode::Type::BIGINT_LITERAL:
        case ASTNode::Type::REGEX_LITERAL:
        // Names an engine operation, not a binding: nothing to collect, and
        // no scope it could reach out of.
        case ASTNode::Type::ENGINE_HELPER:
        case ASTNode::Type::EMPTY_STATEMENT:
        case ASTNode::Type::BREAK_STATEMENT:
        case ASTNode::Type::CONTINUE_STATEMENT:
            return;
        case ASTNode::Type::IDENTIFIER: {
            if (!inside_closure) return;
            const std::string& n = static_cast<const Identifier*>(node)->get_name();
            if (n == "eval") op.saw_eval = true;
            add_name(n);
            return;
        }
        case ASTNode::Type::FUNCTION_EXPRESSION: {
            const auto* n = static_cast<const FunctionExpression*>(node);
            walk_params(n->get_params());
            // A body stepped over is one this scan cannot see through, and a
            // name it would have found is one a register must not take.
            if (!n->get_body()) { if (!try_recorded_scope(n)) op.unknown = true; return; }
            collect_closure_names(n->get_body(), true, out, op, suspendable, super_only);
            return;
        }
        case ASTNode::Type::FUNCTION_DECLARATION: {
            const auto* n = static_cast<const FunctionDeclaration*>(node);
            walk_params(n->get_params());
            if (!n->get_body()) { if (!try_recorded_scope(n)) op.unknown = true; return; }
            collect_closure_names(n->get_body(), true, out, op, suspendable, super_only);
            return;
        }
        case ASTNode::Type::ARROW_FUNCTION_EXPRESSION: {
            const auto* n = static_cast<const ArrowFunctionExpression*>(node);
            walk_params(n->get_params());
            if (!n->get_body()) { if (!try_recorded_scope(n)) op.unknown = true; return; }
            collect_closure_names(n->get_body(), true, out, op, suspendable, super_only);
            return;
        }
        case ASTNode::Type::ASYNC_FUNCTION_EXPRESSION: {
            const auto* n = static_cast<const AsyncFunctionExpression*>(node);
            walk_params(n->get_params());
            if (!n->get_body()) { if (!try_recorded_scope(n)) op.unknown = true; return; }
            collect_closure_names(n->get_body(), true, out, op, suspendable, super_only);
            return;
        }
        case ASTNode::Type::CLASS_DECLARATION: {
            // Everything a class body holds -- the extends clause, computed
            // keys, method bodies, field initializers -- closes over the
            // enclosing scope, so it is walked as one closure. Giving up here
            // instead forced full env_mode on the whole enclosing function,
            // which cost it every register it had. A node this walk does not
            // understand still reaches the default case and sets `unknown`,
            // which is the answer a class used to give unconditionally.
            const auto* n = static_cast<const ClassDeclaration*>(node);
            // The expression form is handed to the tree-walker whole (see
            // named_evaluation_needs_delegate), and delegating needs env_mode,
            // so it keeps answering the way every class used to.
            if (n->is_expression()) { op.saw_class = true; return; }
            collect_closure_names(n->get_superclass(), true, out, op, suspendable, super_only);
            collect_closure_names(n->get_body(), true, out, op, suspendable, super_only);
            return;
        }
        // The three shapes a class body is made of. Same traversal
        // assigns_to_identifier already uses for them.
        case ASTNode::Type::METHOD_DEFINITION: {
            const auto* n = static_cast<const MethodDefinition*>(node);
            if (n->is_computed()) collect_closure_names(n->get_key(), true, out, op, suspendable, super_only);
            collect_closure_names(n->get_value(), true, out, op, suspendable, super_only);
            return;
        }
        case ASTNode::Type::CLASS_FIELD: {
            const auto* n = static_cast<const ClassField*>(node);
            if (n->is_computed()) collect_closure_names(n->get_key(), true, out, op, suspendable, super_only);
            collect_closure_names(n->get_value(), true, out, op, suspendable, super_only);
            return;
        }
        case ASTNode::Type::CLASS_STATIC_BLOCK: {
            const auto* n = static_cast<const ClassStaticBlock*>(node);
            collect_closure_names(n->get_body(), true, out, op, suspendable, super_only);
            return;
        }
        case ASTNode::Type::BLOCK_STATEMENT: {
            const auto* n = static_cast<const BlockStatement*>(node);
            for (const auto& stmt : n->get_statements())
                collect_closure_names(stmt.get(), inside_closure, out, op, suspendable, super_only);
            return;
        }
        case ASTNode::Type::IF_STATEMENT: {
            const auto* n = static_cast<const IfStatement*>(node);
            collect_closure_names(n->get_test(), inside_closure, out, op, suspendable, super_only);
            collect_closure_names(n->get_consequent(), inside_closure, out, op, suspendable, super_only);
            collect_closure_names(n->get_alternate(), inside_closure, out, op, suspendable, super_only);
            return;
        }
        case ASTNode::Type::WHILE_STATEMENT: {
            const auto* n = static_cast<const WhileStatement*>(node);
            collect_closure_names(n->get_test(), inside_closure, out, op, suspendable, super_only);
            collect_closure_names(n->get_body(), inside_closure, out, op, suspendable, super_only);
            return;
        }
        case ASTNode::Type::DO_WHILE_STATEMENT: {
            const auto* n = static_cast<const DoWhileStatement*>(node);
            collect_closure_names(n->get_body(), inside_closure, out, op, suspendable, super_only);
            collect_closure_names(n->get_test(), inside_closure, out, op, suspendable, super_only);
            return;
        }
        case ASTNode::Type::FOR_STATEMENT: {
            const auto* n = static_cast<const ForStatement*>(node);
            collect_closure_names(n->get_init(), inside_closure, out, op, suspendable, super_only);
            collect_closure_names(n->get_test(), inside_closure, out, op, suspendable, super_only);
            collect_closure_names(n->get_update(), inside_closure, out, op, suspendable, super_only);
            collect_closure_names(n->get_body(), inside_closure, out, op, suspendable, super_only);
            return;
        }
        case ASTNode::Type::FOR_OF_STATEMENT: {
            const auto* n = static_cast<const ForOfStatement*>(node);
            collect_closure_names(n->get_left(), inside_closure, out, op, suspendable, super_only);
            collect_closure_names(n->get_right(), inside_closure, out, op, suspendable, super_only);
            collect_closure_names(n->get_body(), inside_closure, out, op, suspendable, super_only);
            return;
        }
        case ASTNode::Type::FOR_IN_STATEMENT: {
            const auto* n = static_cast<const ForInStatement*>(node);
            collect_closure_names(n->get_left(), inside_closure, out, op, suspendable, super_only);
            collect_closure_names(n->get_right(), inside_closure, out, op, suspendable, super_only);
            collect_closure_names(n->get_body(), inside_closure, out, op, suspendable, super_only);
            return;
        }
        case ASTNode::Type::TRY_STATEMENT: {
            const auto* n = static_cast<const TryStatement*>(node);
            collect_closure_names(n->get_try_block(), inside_closure, out, op, suspendable, super_only);
            if (const ASTNode* cc = n->get_catch_clause())
                collect_closure_names(static_cast<const CatchClause*>(cc)->get_body(), inside_closure, out, op, suspendable, super_only);
            collect_closure_names(n->get_finally_block(), inside_closure, out, op, suspendable, super_only);
            return;
        }
        case ASTNode::Type::SWITCH_STATEMENT: {
            const auto* n = static_cast<const SwitchStatement*>(node);
            collect_closure_names(n->get_discriminant(), inside_closure, out, op, suspendable, super_only);
            for (const auto& c : n->get_cases()) {
                const auto* cc = static_cast<const CaseClause*>(c.get());
                collect_closure_names(cc->get_test(), inside_closure, out, op, suspendable, super_only);
                for (const auto& st : cc->get_consequent())
                    collect_closure_names(st.get(), inside_closure, out, op, suspendable, super_only);
            }
            return;
        }
        case ASTNode::Type::LABELED_STATEMENT:
            collect_closure_names(static_cast<const LabeledStatement*>(node)->get_statement(), inside_closure, out, op, suspendable, super_only);
            return;
        case ASTNode::Type::EXPRESSION_STATEMENT:
            collect_closure_names(static_cast<const ExpressionStatement*>(node)->get_expression(), inside_closure, out, op, suspendable, super_only);
            return;
        case ASTNode::Type::RETURN_STATEMENT: {
            // Suspendable: return's argument also delegates to the tree-walker.
            const auto* n = static_cast<const ReturnStatement*>(node);
            collect_closure_names(n->get_argument(), suspendable ? true : inside_closure,
                                  out, op, suspendable, super_only);
            return;
        }
        case ASTNode::Type::THROW_STATEMENT:
            collect_closure_names(static_cast<const ThrowStatement*>(node)->get_expression(), inside_closure, out, op, suspendable, super_only);
            return;
        case ASTNode::Type::VARIABLE_DECLARATION: {
            const auto* n = static_cast<const VariableDeclaration*>(node);
            for (const auto& d : n->get_declarations())
                collect_closure_names(d->get_init(), inside_closure, out, op, suspendable, super_only);
            return;
        }
        case ASTNode::Type::DESTRUCTURING_ASSIGNMENT: {
            // A pattern is emitted like any other code now, so the names it
            // binds are read the way any name is: nothing here has to be kept
            // in the environment for a walker to resolve by name.
            const auto* n = static_cast<const DestructuringAssignment*>(node);
            collect_closure_names(n->get_source(), inside_closure, out, op, suspendable, super_only);
            n->for_each_expression([&](const ASTNode* e) {
                collect_closure_names(e, inside_closure, out, op, suspendable, super_only);
            });
            return;
        }
        case ASTNode::Type::ASSIGNMENT_EXPRESSION: {
            const auto* n = static_cast<const AssignmentExpression*>(node);
            collect_closure_names(n->get_left(), inside_closure, out, op, suspendable, super_only);
            collect_closure_names(n->get_right(), inside_closure, out, op, suspendable, super_only);
            return;
        }
        case ASTNode::Type::UNARY_EXPRESSION:
            collect_closure_names(static_cast<const UnaryExpression*>(node)->get_operand(), inside_closure, out, op, suspendable, super_only);
            return;
        case ASTNode::Type::BINARY_EXPRESSION: {
            const auto* n = static_cast<const BinaryExpression*>(node);
            collect_closure_names(n->get_left(), inside_closure, out, op, suspendable, super_only);
            collect_closure_names(n->get_right(), inside_closure, out, op, suspendable, super_only);
            return;
        }
        case ASTNode::Type::NULLISH_COALESCING_EXPRESSION: {
            const auto* n = static_cast<const NullishCoalescingExpression*>(node);
            collect_closure_names(n->get_left(), inside_closure, out, op, suspendable, super_only);
            collect_closure_names(n->get_right(), inside_closure, out, op, suspendable, super_only);
            return;
        }
        case ASTNode::Type::CONDITIONAL_EXPRESSION: {
            const auto* n = static_cast<const ConditionalExpression*>(node);
            collect_closure_names(n->get_test(), inside_closure, out, op, suspendable, super_only);
            collect_closure_names(n->get_consequent(), inside_closure, out, op, suspendable, super_only);
            collect_closure_names(n->get_alternate(), inside_closure, out, op, suspendable, super_only);
            return;
        }
        case ASTNode::Type::CALL_EXPRESSION: {
            // Spread args delegate whole (see compile_expression) -- force
            // residency same as a complex object literal above.
            const auto* n = static_cast<const CallExpression*>(node);
            bool forced = inside_closure || spread_call_delegates(n);
            collect_closure_names(n->get_callee(), forced, out, op, suspendable, super_only);
            for (const auto& arg : n->get_arguments())
                collect_closure_names(arg.get(), forced, out, op, suspendable, super_only);
            return;
        }
        case ASTNode::Type::NEW_EXPRESSION: {
            const auto* n = static_cast<const NewExpression*>(node);
            bool forced = inside_closure;
            collect_closure_names(n->get_constructor(), forced, out, op, suspendable, super_only);
            for (const auto& arg : n->get_arguments())
                collect_closure_names(arg.get(), forced, out, op, suspendable, super_only);
            return;
        }
        case ASTNode::Type::MEMBER_EXPRESSION: {
            // `x.name` references only `x` -- a non-computed property is a name.
            const auto* n = static_cast<const MemberExpression*>(node);
            collect_closure_names(n->get_object(), inside_closure, out, op, suspendable, super_only);
            if (n->is_computed())
                collect_closure_names(n->get_property(), inside_closure, out, op, suspendable, super_only);
            return;
        }
        case ASTNode::Type::OPTIONAL_CHAINING_EXPRESSION: {
            const auto* n = static_cast<const OptionalChainingExpression*>(node);
            collect_closure_names(n->get_object(), inside_closure, out, op, suspendable, super_only);
            if (n->is_computed())
                collect_closure_names(n->get_property(), inside_closure, out, op, suspendable, super_only);
            return;
        }
        case ASTNode::Type::SPREAD_ELEMENT:
            collect_closure_names(static_cast<const SpreadElement*>(node)->get_argument(), inside_closure, out, op, suspendable, super_only);
            return;
        // `new.target`/`import.meta` resolve from Context state (see
        // Op::LdaNewTarget/LdaImportMeta and __arrow_new_target__ for an
        // arrow's own inherited copy), not the environment chain -- no outer
        // name to capture, same as a literal.
        case ASTNode::Type::META_PROPERTY:
            return;
        case ASTNode::Type::TEMPLATE_LITERAL: {
            const auto* n = static_cast<const TemplateLiteral*>(node);
            for (const auto& el : n->get_elements())
                if (el.type == TemplateLiteral::Element::Type::EXPRESSION)
                    collect_closure_names(el.expression.get(), inside_closure, out, op, suspendable, super_only);
            return;
        }
        case ASTNode::Type::OBJECT_LITERAL: {
            // A complex literal delegates whole (see compile_expression) --
            // same env-residency forcing as DESTRUCTURING_ASSIGNMENT above.
            const auto* n = static_cast<const ObjectLiteral*>(node);
            bool forced = inside_closure || object_literal_is_complex(n);
            for (const auto& prop : n->get_properties()) {
                if (prop->computed && prop->key)
                    collect_closure_names(prop->key.get(), forced, out, op, suspendable, super_only);
                if (prop->value)
                    collect_closure_names(prop->value.get(), forced, out, op, suspendable, super_only);
            }
            return;
        }
        case ASTNode::Type::ARRAY_LITERAL: {
            const auto* n = static_cast<const ArrayLiteral*>(node);
            bool forced = inside_closure;
            for (const auto& el : n->get_elements())
                collect_closure_names(el.get(), forced, out, op, suspendable, super_only);
            return;
        }
        // Compiled as a closure of its own, not inline here.
        case ASTNode::Type::YIELD_EXPRESSION: {
            const auto* n = static_cast<const YieldExpression*>(node);
            if (n->get_argument())
                collect_closure_names(n->get_argument(),
                                      op.yield_transparent ? inside_closure : true,
                                      out, op, suspendable, super_only);
            return;
        }
        case ASTNode::Type::AWAIT_EXPRESSION: {
            const auto* n = static_cast<const AwaitExpression*>(node);
            if (n->get_argument())
                collect_closure_names(n->get_argument(),
                                      op.yield_transparent ? inside_closure : true,
                                      out, op, suspendable, super_only);
            return;
        }
        case ASTNode::Type::PARAMETER: {
            const auto* n = static_cast<const Parameter*>(node);
            collect_closure_names(n->get_default_value(), inside_closure, out, op, suspendable, super_only);
            collect_closure_names(n->get_destructuring_pattern(), inside_closure, out, op, suspendable, super_only);
            return;
        }
        case ASTNode::Type::VARIABLE_DECLARATOR:
            collect_closure_names(static_cast<const VariableDeclarator*>(node)->get_init(), inside_closure, out, op, suspendable, super_only);
            return;
        case ASTNode::Type::WITH_STATEMENT: {
            const auto* n = static_cast<const WithStatement*>(node);
            collect_closure_names(n->get_object(), inside_closure, out, op, suspendable, super_only);
            collect_closure_names(n->get_body(), inside_closure, out, op, suspendable, super_only);
            return;
        }
        case ASTNode::Type::CATCH_CLAUSE: {
            const auto* n = static_cast<const CatchClause*>(node);
            collect_closure_names(n->get_destructuring_pattern(), inside_closure, out, op, suspendable, super_only);
            collect_closure_names(n->get_body(), inside_closure, out, op, suspendable, super_only);
            return;
        }
        case ASTNode::Type::CASE_CLAUSE: {
            const auto* n = static_cast<const CaseClause*>(node);
            collect_closure_names(n->get_test(), inside_closure, out, op, suspendable, super_only);
            for (const auto& st : n->get_consequent()) {
                collect_closure_names(st.get(), inside_closure, out, op, suspendable, super_only);
            }
            return;
        }
        case ASTNode::Type::USING_DECLARATION: {
            const auto* n = static_cast<const UsingDeclaration*>(node);
            for (const auto& b : n->get_bindings()) {
                collect_closure_names(b.initializer.get(), inside_closure, out, op, suspendable, super_only);
            }
            return;
        }
        case ASTNode::Type::PROGRAM:
            for (const auto& st : static_cast<const Program*>(node)->get_statements()) {
                collect_closure_names(st.get(), inside_closure, out, op, suspendable, super_only);
            }
            return;

        default:
            op.unknown = true;
            return;
    }
}

// Collects a BlockStatement's OWN direct (non-nested) let/const/class names
// into `out` -- used to push exactly one lexical scope frame per block,
// mirroring collect_direct_lexical_decls' scope boundary (a name declared in
// a nested block must NOT be visible to a sibling block or the outer body).
void collect_direct_lexical_names(const BlockStatement* block, std::unordered_set<std::string>& out) {
    for (const auto& stmt : block->get_statements()) {
        if (stmt->get_type() == ASTNode::Type::CLASS_DECLARATION) {
            const auto* cd = static_cast<const ClassDeclaration*>(stmt.get());
            if (!cd->is_expression() && cd->get_id() && !cd->get_id()->get_name().empty()) {
                out.insert(cd->get_id()->get_name());
            }
        } else if (stmt->get_type() == ASTNode::Type::VARIABLE_DECLARATION) {
            const auto* decl = static_cast<const VariableDeclaration*>(stmt.get());
            if (decl->get_kind() == VariableDeclarator::Kind::VAR) continue;
            for (const auto& d : decl->get_declarations()) {
                if (d->get_id() && !d->get_id()->get_name().empty()) out.insert(d->get_id()->get_name());
            }
        }
    }
}

// Free-variable scanner for closure_needs_outer_environment: same node
// coverage as collect_closure_names, but a separate function since it
// answers a stricter question -- of the names this closure's body touches,
// which are NOT bound anywhere in its own scope. `scope_stack` holds one
// frame per active scope (function-level params/var/function at the
// bottom, one more per block/for-header/catch/switch entered); a name is
// free only if no frame has it. `in_arrow`: true once inside a nested
// arrow -- this/arguments/super/new.target are an ordinary function's own
// per-call binding (no environment needed) but an arrow must reach them
// through its captured environment. Anything this scanner can't reason
// about precisely (destructuring, a pattern-target assignment, eval,
// class, suspendable nested functions, spread, unrecognized nodes) sets
// `unknown`, treated as "needs the environment" -- same bailout
// philosophy as collect_closure_names, applied more eagerly since this
// scanner's answer has no runtime fallback if wrong.
void collect_free_names(const ASTNode* node,
                         std::vector<std::unordered_set<std::string>>& scope_stack,
                         bool in_arrow,
                         std::unordered_set<std::string>& free_out,
                         ScanOpacity& op) {
    if (!node || op.unknown) return;
    auto is_bound = [&](const std::string& name) {
        for (const auto& frame : scope_stack) {
            if (frame.count(name)) return true;
        }
        return false;
    };
    // Pushes a fresh function-level frame (own params + own hoisted var/
    // function names, via prescan_declarations -- which itself never
    // descends into a nested closure's body, so this frame is exactly this
    // nested function's own function-scope, nothing more) and recurses into
    // its body with the SAME scope_stack (outer frames stay visible
    // beneath, a real closure chain) plus this new frame on top.
    auto recurse_into_function = [&](const std::vector<std::unique_ptr<Parameter>>& params,
                                      const ASTNode* body, bool nested_is_arrow) {
        for (const auto& p : params) {
            if (p && (p->has_destructuring() || p->has_default())) { op.unknown = true; return; }
        }
        std::vector<DeclInfo> declared;
        if (!prescan_declarations(body, declared)) { op.unknown = true; return; }
        std::unordered_set<std::string> frame;
        for (const auto& p : params) {
            if (p && p->get_name()) frame.insert(p->get_name()->get_name());
        }
        for (const auto& info : declared) {
            if (!info.is_lexical) frame.insert(info.name);  // var/function: function-wide
        }
        scope_stack.push_back(std::move(frame));
        collect_free_names(body, scope_stack, in_arrow || nested_is_arrow, free_out, op);
        scope_stack.pop_back();
    };
    switch (node->get_type()) {
        case ASTNode::Type::NUMBER_LITERAL:
        case ASTNode::Type::STRING_LITERAL:
        case ASTNode::Type::BOOLEAN_LITERAL:
        case ASTNode::Type::NULL_LITERAL:
        case ASTNode::Type::UNDEFINED_LITERAL:
        case ASTNode::Type::BIGINT_LITERAL:
        case ASTNode::Type::REGEX_LITERAL:
        // Names an engine operation, not a binding: nothing to collect, and
        // no scope it could reach out of.
        case ASTNode::Type::ENGINE_HELPER:
        case ASTNode::Type::EMPTY_STATEMENT:
        case ASTNode::Type::BREAK_STATEMENT:
        case ASTNode::Type::CONTINUE_STATEMENT:
            return;
        case ASTNode::Type::IDENTIFIER: {
            const std::string& n = static_cast<const Identifier*>(node)->get_name();
            if (n == "eval") { op.saw_eval = true; return; }
            if (n == "this" || n == "arguments" || n == "super" || n == "new.target") {
                if (in_arrow) free_out.insert(n);  // must reach the defining scope's own binding
                return;  // ordinary function/method: own per-call binding, no environment needed
            }
            if (!is_bound(n)) free_out.insert(n);
            return;
        }
        case ASTNode::Type::FUNCTION_EXPRESSION: {
            const auto* n = static_cast<const FunctionExpression*>(node);
            if (n->is_generator() || n->is_async()) { op.unknown = true; return; }
            recurse_into_function(n->get_params(), n->get_body(), false);
            return;
        }
        case ASTNode::Type::FUNCTION_DECLARATION: {
            const auto* n = static_cast<const FunctionDeclaration*>(node);
            recurse_into_function(n->get_params(), n->get_body(), false);
            return;
        }
        case ASTNode::Type::ARROW_FUNCTION_EXPRESSION: {
            const auto* n = static_cast<const ArrowFunctionExpression*>(node);
            recurse_into_function(n->get_params(), n->get_body(), true);
            return;
        }
        case ASTNode::Type::ASYNC_FUNCTION_EXPRESSION:
            op.unknown = true;
            return;
        case ASTNode::Type::CLASS_DECLARATION:
            op.saw_class = true;
            return;
        case ASTNode::Type::BLOCK_STATEMENT: {
            const auto* n = static_cast<const BlockStatement*>(node);
            std::unordered_set<std::string> frame;
            collect_direct_lexical_names(n, frame);
            scope_stack.push_back(std::move(frame));
            for (const auto& stmt : n->get_statements())
                collect_free_names(stmt.get(), scope_stack, in_arrow, free_out, op);
            scope_stack.pop_back();
            return;
        }
        case ASTNode::Type::IF_STATEMENT: {
            const auto* n = static_cast<const IfStatement*>(node);
            collect_free_names(n->get_test(), scope_stack, in_arrow, free_out, op);
            collect_free_names(n->get_consequent(), scope_stack, in_arrow, free_out, op);
            collect_free_names(n->get_alternate(), scope_stack, in_arrow, free_out, op);
            return;
        }
        case ASTNode::Type::WHILE_STATEMENT: {
            const auto* n = static_cast<const WhileStatement*>(node);
            collect_free_names(n->get_test(), scope_stack, in_arrow, free_out, op);
            collect_free_names(n->get_body(), scope_stack, in_arrow, free_out, op);
            return;
        }
        case ASTNode::Type::DO_WHILE_STATEMENT: {
            const auto* n = static_cast<const DoWhileStatement*>(node);
            collect_free_names(n->get_body(), scope_stack, in_arrow, free_out, op);
            collect_free_names(n->get_test(), scope_stack, in_arrow, free_out, op);
            return;
        }
        case ASTNode::Type::FOR_STATEMENT: {
            const auto* n = static_cast<const ForStatement*>(node);
            std::unordered_set<std::string> frame;
            bool has_frame = false;
            if (n->get_init() && n->get_init()->get_type() == ASTNode::Type::VARIABLE_DECLARATION) {
                const auto* vd = static_cast<const VariableDeclaration*>(n->get_init());
                if (vd->get_kind() != VariableDeclarator::Kind::VAR) {
                    has_frame = true;
                    for (const auto& d : vd->get_declarations()) {
                        if (d->get_id()) frame.insert(d->get_id()->get_name());
                    }
                }
            }
            if (has_frame) scope_stack.push_back(std::move(frame));
            collect_free_names(n->get_init(), scope_stack, in_arrow, free_out, op);
            collect_free_names(n->get_test(), scope_stack, in_arrow, free_out, op);
            collect_free_names(n->get_update(), scope_stack, in_arrow, free_out, op);
            collect_free_names(n->get_body(), scope_stack, in_arrow, free_out, op);
            if (has_frame) scope_stack.pop_back();
            return;
        }
        case ASTNode::Type::FOR_OF_STATEMENT:
        case ASTNode::Type::FOR_IN_STATEMENT: {
            const ASTNode* left = node->get_type() == ASTNode::Type::FOR_OF_STATEMENT
                ? static_cast<const ForOfStatement*>(node)->get_left()
                : static_cast<const ForInStatement*>(node)->get_left();
            const ASTNode* right = node->get_type() == ASTNode::Type::FOR_OF_STATEMENT
                ? static_cast<const ForOfStatement*>(node)->get_right()
                : static_cast<const ForInStatement*>(node)->get_right();
            const ASTNode* body = node->get_type() == ASTNode::Type::FOR_OF_STATEMENT
                ? static_cast<const ForOfStatement*>(node)->get_body()
                : static_cast<const ForInStatement*>(node)->get_body();
            std::unordered_set<std::string> frame;
            bool has_frame = false;
            if (left->get_type() == ASTNode::Type::VARIABLE_DECLARATION) {
                const auto* vd = static_cast<const VariableDeclaration*>(left);
                if (vd->get_kind() != VariableDeclarator::Kind::VAR) {
                    has_frame = true;
                    for (const auto& d : vd->get_declarations()) {
                        if (d->get_id()) frame.insert(d->get_id()->get_name());
                    }
                }
            } else if (left->get_type() == ASTNode::Type::USING_DECLARATION) {
                has_frame = true;
                for (const auto& b : static_cast<const UsingDeclaration*>(left)->get_bindings())
                    frame.insert(b.name);
            } else if (left->get_type() == ASTNode::Type::DESTRUCTURING_ASSIGNMENT) {
                op.unknown = true;
                return;
            } else {
                // Not a declaration: the head ASSIGNS to something that already
                // exists, and the parser allows an identifier, a member
                // expression or an array/object literal pattern here. Those
                // names were never scanned, so `for (captured of xs) {}` looked
                // like a closure with no free names at all while it writes
                // straight through to an outer binding.
                collect_free_names(left, scope_stack, in_arrow, free_out, op);
            }
            collect_free_names(right, scope_stack, in_arrow, free_out, op);
            if (has_frame) scope_stack.push_back(std::move(frame));
            collect_free_names(body, scope_stack, in_arrow, free_out, op);
            if (has_frame) scope_stack.pop_back();
            return;
        }
        case ASTNode::Type::TRY_STATEMENT: {
            const auto* n = static_cast<const TryStatement*>(node);
            collect_free_names(n->get_try_block(), scope_stack, in_arrow, free_out, op);
            if (const ASTNode* cc = n->get_catch_clause()) {
                const auto* clause = static_cast<const CatchClause*>(cc);
                if (clause->get_destructuring_pattern()) { op.unknown = true; return; }
                std::unordered_set<std::string> frame;
                if (!clause->get_parameter_name().empty()) frame.insert(clause->get_parameter_name());
                scope_stack.push_back(std::move(frame));
                collect_free_names(clause->get_body(), scope_stack, in_arrow, free_out, op);
                scope_stack.pop_back();
            }
            collect_free_names(n->get_finally_block(), scope_stack, in_arrow, free_out, op);
            return;
        }
        case ASTNode::Type::SWITCH_STATEMENT: {
            const auto* n = static_cast<const SwitchStatement*>(node);
            collect_free_names(n->get_discriminant(), scope_stack, in_arrow, free_out, op);
            std::unordered_set<std::string> frame;
            for (const auto& c : n->get_cases()) {
                for (const auto& st : static_cast<const CaseClause*>(c.get())->get_consequent()) {
                    if (st->get_type() == ASTNode::Type::VARIABLE_DECLARATION) {
                        const auto* decl = static_cast<const VariableDeclaration*>(st.get());
                        if (decl->get_kind() != VariableDeclarator::Kind::VAR) {
                            for (const auto& d : decl->get_declarations()) {
                                if (d->get_id()) frame.insert(d->get_id()->get_name());
                            }
                        }
                    } else if (st->get_type() == ASTNode::Type::CLASS_DECLARATION) {
                        const auto* cd = static_cast<const ClassDeclaration*>(st.get());
                        if (!cd->is_expression() && cd->get_id() && !cd->get_id()->get_name().empty()) {
                            frame.insert(cd->get_id()->get_name());
                        }
                    }
                }
            }
            scope_stack.push_back(std::move(frame));
            for (const auto& c : n->get_cases()) {
                const auto* cc = static_cast<const CaseClause*>(c.get());
                collect_free_names(cc->get_test(), scope_stack, in_arrow, free_out, op);
                for (const auto& st : cc->get_consequent())
                    collect_free_names(st.get(), scope_stack, in_arrow, free_out, op);
            }
            scope_stack.pop_back();
            return;
        }
        case ASTNode::Type::LABELED_STATEMENT:
            collect_free_names(static_cast<const LabeledStatement*>(node)->get_statement(), scope_stack, in_arrow, free_out, op);
            return;
        case ASTNode::Type::EXPRESSION_STATEMENT:
            collect_free_names(static_cast<const ExpressionStatement*>(node)->get_expression(), scope_stack, in_arrow, free_out, op);
            return;
        case ASTNode::Type::RETURN_STATEMENT:
            collect_free_names(static_cast<const ReturnStatement*>(node)->get_argument(), scope_stack, in_arrow, free_out, op);
            return;
        case ASTNode::Type::THROW_STATEMENT:
            collect_free_names(static_cast<const ThrowStatement*>(node)->get_expression(), scope_stack, in_arrow, free_out, op);
            return;
        case ASTNode::Type::VARIABLE_DECLARATION: {
            const auto* n = static_cast<const VariableDeclaration*>(node);
            for (const auto& d : n->get_declarations()) {
                if (!d->get_id()) { op.unknown = true; return; }  // destructuring declarator
                collect_free_names(d->get_init(), scope_stack, in_arrow, free_out, op);
            }
            return;
        }
        case ASTNode::Type::DESTRUCTURING_ASSIGNMENT:
            op.unknown = true;
            return;
        case ASTNode::Type::ASSIGNMENT_EXPRESSION: {
            const auto* n = static_cast<const AssignmentExpression*>(node);
            if (n->get_left()->get_type() == ASTNode::Type::ARRAY_LITERAL ||
                n->get_left()->get_type() == ASTNode::Type::OBJECT_LITERAL) {
                op.unknown = true;
                return;
            }
            collect_free_names(n->get_left(), scope_stack, in_arrow, free_out, op);
            collect_free_names(n->get_right(), scope_stack, in_arrow, free_out, op);
            return;
        }
        case ASTNode::Type::UNARY_EXPRESSION:
            collect_free_names(static_cast<const UnaryExpression*>(node)->get_operand(), scope_stack, in_arrow, free_out, op);
            return;
        case ASTNode::Type::BINARY_EXPRESSION: {
            const auto* n = static_cast<const BinaryExpression*>(node);
            collect_free_names(n->get_left(), scope_stack, in_arrow, free_out, op);
            collect_free_names(n->get_right(), scope_stack, in_arrow, free_out, op);
            return;
        }
        case ASTNode::Type::NULLISH_COALESCING_EXPRESSION: {
            const auto* n = static_cast<const NullishCoalescingExpression*>(node);
            collect_free_names(n->get_left(), scope_stack, in_arrow, free_out, op);
            collect_free_names(n->get_right(), scope_stack, in_arrow, free_out, op);
            return;
        }
        case ASTNode::Type::CONDITIONAL_EXPRESSION: {
            const auto* n = static_cast<const ConditionalExpression*>(node);
            collect_free_names(n->get_test(), scope_stack, in_arrow, free_out, op);
            collect_free_names(n->get_consequent(), scope_stack, in_arrow, free_out, op);
            collect_free_names(n->get_alternate(), scope_stack, in_arrow, free_out, op);
            return;
        }
        case ASTNode::Type::CALL_EXPRESSION: {
            const auto* n = static_cast<const CallExpression*>(node);
            if (spread_call_delegates(n)) { op.unknown = true; return; }
            collect_free_names(n->get_callee(), scope_stack, in_arrow, free_out, op);
            for (const auto& arg : n->get_arguments())
                collect_free_names(arg.get(), scope_stack, in_arrow, free_out, op);
            return;
        }
        case ASTNode::Type::NEW_EXPRESSION: {
            const auto* n = static_cast<const NewExpression*>(node);
            collect_free_names(n->get_constructor(), scope_stack, in_arrow, free_out, op);
            for (const auto& arg : n->get_arguments())
                collect_free_names(arg.get(), scope_stack, in_arrow, free_out, op);
            return;
        }
        case ASTNode::Type::MEMBER_EXPRESSION: {
            const auto* n = static_cast<const MemberExpression*>(node);
            collect_free_names(n->get_object(), scope_stack, in_arrow, free_out, op);
            if (n->is_computed())
                collect_free_names(n->get_property(), scope_stack, in_arrow, free_out, op);
            return;
        }
        case ASTNode::Type::OPTIONAL_CHAINING_EXPRESSION: {
            const auto* n = static_cast<const OptionalChainingExpression*>(node);
            collect_free_names(n->get_object(), scope_stack, in_arrow, free_out, op);
            if (n->is_computed())
                collect_free_names(n->get_property(), scope_stack, in_arrow, free_out, op);
            return;
        }
        case ASTNode::Type::SPREAD_ELEMENT:
            op.unknown = true;
            return;
        case ASTNode::Type::TEMPLATE_LITERAL: {
            const auto* n = static_cast<const TemplateLiteral*>(node);
            for (const auto& el : n->get_elements())
                if (el.type == TemplateLiteral::Element::Type::EXPRESSION)
                    collect_free_names(el.expression.get(), scope_stack, in_arrow, free_out, op);
            return;
        }
        case ASTNode::Type::OBJECT_LITERAL: {
            const auto* n = static_cast<const ObjectLiteral*>(node);
            if (object_literal_is_complex(n)) { op.unknown = true; return; }
            for (const auto& prop : n->get_properties()) {
                if (prop->computed && prop->key)
                    collect_free_names(prop->key.get(), scope_stack, in_arrow, free_out, op);
                if (prop->value)
                    collect_free_names(prop->value.get(), scope_stack, in_arrow, free_out, op);
            }
            return;
        }
        case ASTNode::Type::ARRAY_LITERAL: {
            const auto* n = static_cast<const ArrayLiteral*>(node);
            for (const auto& el : n->get_elements())
                collect_free_names(el.get(), scope_stack, in_arrow, free_out, op);
            return;
        }
        case ASTNode::Type::YIELD_EXPRESSION:
        case ASTNode::Type::AWAIT_EXPRESSION:
            op.unknown = true;
            return;
        default:
            op.unknown = true;
            return;
    }
}

// True if any name in `vars` could be observably captured by a closure
// anywhere in `roots` -- spec 14.7.4.3/14.7.5.6's CreatePerIterationEnvironment
// (Op::AdvanceLoopEnv) exists ONLY to support this; with no such closure, one
// binding mutated in place each iteration is behaviorally identical. Reuses
// collect_closure_names' own contract (eval/class/unknown-node bailouts all
// count as "captured", same conservative default it already uses elsewhere).
bool loop_vars_may_be_captured(const std::vector<const ASTNode*>& roots,
                                const std::vector<BytecodeChunk::LoopEnvVar>& vars) {
    if (vars.empty()) return false;
    std::unordered_set<std::string> refs;
    ScanOpacity op;
    // A suspension is not a capture: `for (let i...) yield i;` hands the value
    // out and comes back to the same binding, and nothing outside can hold two
    // iterations' worth of it unless a real closure took one -- which the
    // arrow/function cases below still report. Without this a generator paid
    // per-iteration environments for every loop it yields from.
    op.yield_transparent = true;
    for (const ASTNode* root : roots) {
        collect_closure_names(root, /*inside_closure=*/false, refs, op);
    }
    if (op.opaque()) return true;
    for (const auto& v : vars) {
        if (refs.count(v.name)) return true;
    }
    return false;
}

// True if `name` appears as a plain Identifier anywhere in `node` except
// inside `region` (skipped by pointer identity). Proves a nested lexical
// never escapes its own block, safe to keep register-resident. A separate
// function rather than an exclusion param on collect_closure_names: that
// function's contract is relied on everywhere else in this file, and
// threading a new param through its ~40 call sites risks a silent
// regression. No inside_closure tracking needed here -- any reference
// outside region is an escape, closure or not. Unknown node types
// conservatively count as an escape.
bool references_outside(const ASTNode* node, const std::unordered_set<const ASTNode*>& regions,
                        const std::string& name);
inline bool references_outside(const ASTNode* node, const ASTNode* region, const std::string& name) {
    return references_outside(node, std::unordered_set<const ASTNode*>{region}, name);
}

// `regions`: every region that declares `name`. Two sibling loops reusing the
// same counter each own their references, so a mention inside ANY of them is
// not an escape -- checking one region at a time would read a sibling's own
// use as one.
bool references_outside(const ASTNode* node, const std::unordered_set<const ASTNode*>& regions,
                        const std::string& name) {
    if (!node || regions.count(node)) return false;
    auto walk_params = [&](const std::vector<std::unique_ptr<Parameter>>& ps) {
        for (const auto& p : ps) {
            if (p->has_default() && references_outside(p->get_default_value(), regions, name)) return true;
            if (p->has_destructuring() && references_outside(p->get_destructuring_pattern(), regions, name)) return true;
        }
        return false;
    };
    switch (node->get_type()) {
        case ASTNode::Type::NUMBER_LITERAL:
        case ASTNode::Type::STRING_LITERAL:
        case ASTNode::Type::BOOLEAN_LITERAL:
        case ASTNode::Type::NULL_LITERAL:
        case ASTNode::Type::UNDEFINED_LITERAL:
        case ASTNode::Type::BIGINT_LITERAL:
        case ASTNode::Type::REGEX_LITERAL:
        // Names an engine operation, not a binding: nothing to collect, and
        // no scope it could reach out of.
        case ASTNode::Type::ENGINE_HELPER:
        case ASTNode::Type::EMPTY_STATEMENT:
        case ASTNode::Type::BREAK_STATEMENT:
        case ASTNode::Type::CONTINUE_STATEMENT:
            return false;
        case ASTNode::Type::IDENTIFIER:
            return static_cast<const Identifier*>(node)->get_name() == name;
        case ASTNode::Type::FUNCTION_EXPRESSION: {
            const auto* n = static_cast<const FunctionExpression*>(node);
            return walk_params(n->get_params()) || references_outside(n->get_body(), regions, name);
        }
        case ASTNode::Type::FUNCTION_DECLARATION: {
            const auto* n = static_cast<const FunctionDeclaration*>(node);
            return walk_params(n->get_params()) || references_outside(n->get_body(), regions, name);
        }
        case ASTNode::Type::ARROW_FUNCTION_EXPRESSION: {
            const auto* n = static_cast<const ArrowFunctionExpression*>(node);
            return walk_params(n->get_params()) || references_outside(n->get_body(), regions, name);
        }
        case ASTNode::Type::ASYNC_FUNCTION_EXPRESSION: {
            const auto* n = static_cast<const AsyncFunctionExpression*>(node);
            return walk_params(n->get_params()) || references_outside(n->get_body(), regions, name);
        }
        case ASTNode::Type::CLASS_DECLARATION:
            return true;  // opaque to this scan, same conservative treatment as collect_closure_names
        case ASTNode::Type::BLOCK_STATEMENT: {
            const auto* n = static_cast<const BlockStatement*>(node);
            for (const auto& stmt : n->get_statements())
                if (references_outside(stmt.get(), regions, name)) return true;
            return false;
        }
        case ASTNode::Type::IF_STATEMENT: {
            const auto* n = static_cast<const IfStatement*>(node);
            return references_outside(n->get_test(), regions, name) ||
                   references_outside(n->get_consequent(), regions, name) ||
                   references_outside(n->get_alternate(), regions, name);
        }
        case ASTNode::Type::WHILE_STATEMENT: {
            const auto* n = static_cast<const WhileStatement*>(node);
            return references_outside(n->get_test(), regions, name) ||
                   references_outside(n->get_body(), regions, name);
        }
        case ASTNode::Type::DO_WHILE_STATEMENT: {
            const auto* n = static_cast<const DoWhileStatement*>(node);
            return references_outside(n->get_body(), regions, name) ||
                   references_outside(n->get_test(), regions, name);
        }
        case ASTNode::Type::FOR_STATEMENT: {
            const auto* n = static_cast<const ForStatement*>(node);
            return references_outside(n->get_init(), regions, name) ||
                   references_outside(n->get_test(), regions, name) ||
                   references_outside(n->get_update(), regions, name) ||
                   references_outside(n->get_body(), regions, name);
        }
        case ASTNode::Type::FOR_OF_STATEMENT: {
            const auto* n = static_cast<const ForOfStatement*>(node);
            return references_outside(n->get_left(), regions, name) ||
                   references_outside(n->get_right(), regions, name) ||
                   references_outside(n->get_body(), regions, name);
        }
        case ASTNode::Type::FOR_IN_STATEMENT: {
            const auto* n = static_cast<const ForInStatement*>(node);
            return references_outside(n->get_left(), regions, name) ||
                   references_outside(n->get_right(), regions, name) ||
                   references_outside(n->get_body(), regions, name);
        }
        case ASTNode::Type::TRY_STATEMENT: {
            const auto* n = static_cast<const TryStatement*>(node);
            if (references_outside(n->get_try_block(), regions, name)) return true;
            if (const ASTNode* cc = n->get_catch_clause())
                if (references_outside(static_cast<const CatchClause*>(cc)->get_body(), regions, name)) return true;
            return references_outside(n->get_finally_block(), regions, name);
        }
        case ASTNode::Type::SWITCH_STATEMENT: {
            const auto* n = static_cast<const SwitchStatement*>(node);
            if (references_outside(n->get_discriminant(), regions, name)) return true;
            for (const auto& c : n->get_cases()) {
                const auto* cc = static_cast<const CaseClause*>(c.get());
                if (references_outside(cc->get_test(), regions, name)) return true;
                for (const auto& st : cc->get_consequent())
                    if (references_outside(st.get(), regions, name)) return true;
            }
            return false;
        }
        case ASTNode::Type::LABELED_STATEMENT:
            return references_outside(static_cast<const LabeledStatement*>(node)->get_statement(), regions, name);
        case ASTNode::Type::EXPRESSION_STATEMENT:
            return references_outside(static_cast<const ExpressionStatement*>(node)->get_expression(), regions, name);
        case ASTNode::Type::RETURN_STATEMENT:
            return references_outside(static_cast<const ReturnStatement*>(node)->get_argument(), regions, name);
        case ASTNode::Type::THROW_STATEMENT:
            return references_outside(static_cast<const ThrowStatement*>(node)->get_expression(), regions, name);
        case ASTNode::Type::VARIABLE_DECLARATION: {
            const auto* n = static_cast<const VariableDeclaration*>(node);
            for (const auto& d : n->get_declarations())
                if (references_outside(d->get_init(), regions, name)) return true;
            return false;
        }
        case ASTNode::Type::DESTRUCTURING_ASSIGNMENT: {
            const auto* n = static_cast<const DestructuringAssignment*>(node);
            if (references_outside(n->get_source(), regions, name)) return true;
            std::vector<std::string> bound;
            n->collect_bound_names(bound);
            for (const auto& bn : bound) if (bn == name) return true;
            bool found = false;
            n->for_each_expression([&](const ASTNode* e) {
                if (!found && references_outside(e, regions, name)) found = true;
            });
            return found;
        }
        case ASTNode::Type::ASSIGNMENT_EXPRESSION: {
            const auto* n = static_cast<const AssignmentExpression*>(node);
            return references_outside(n->get_left(), regions, name) ||
                   references_outside(n->get_right(), regions, name);
        }
        case ASTNode::Type::UNARY_EXPRESSION:
            return references_outside(static_cast<const UnaryExpression*>(node)->get_operand(), regions, name);
        case ASTNode::Type::BINARY_EXPRESSION: {
            const auto* n = static_cast<const BinaryExpression*>(node);
            return references_outside(n->get_left(), regions, name) ||
                   references_outside(n->get_right(), regions, name);
        }
        case ASTNode::Type::NULLISH_COALESCING_EXPRESSION: {
            const auto* n = static_cast<const NullishCoalescingExpression*>(node);
            return references_outside(n->get_left(), regions, name) ||
                   references_outside(n->get_right(), regions, name);
        }
        case ASTNode::Type::CONDITIONAL_EXPRESSION: {
            const auto* n = static_cast<const ConditionalExpression*>(node);
            return references_outside(n->get_test(), regions, name) ||
                   references_outside(n->get_consequent(), regions, name) ||
                   references_outside(n->get_alternate(), regions, name);
        }
        case ASTNode::Type::CALL_EXPRESSION: {
            const auto* n = static_cast<const CallExpression*>(node);
            if (references_outside(n->get_callee(), regions, name)) return true;
            for (const auto& arg : n->get_arguments())
                if (references_outside(arg.get(), regions, name)) return true;
            return false;
        }
        case ASTNode::Type::NEW_EXPRESSION: {
            const auto* n = static_cast<const NewExpression*>(node);
            if (references_outside(n->get_constructor(), regions, name)) return true;
            for (const auto& arg : n->get_arguments())
                if (references_outside(arg.get(), regions, name)) return true;
            return false;
        }
        case ASTNode::Type::MEMBER_EXPRESSION: {
            const auto* n = static_cast<const MemberExpression*>(node);
            if (references_outside(n->get_object(), regions, name)) return true;
            return n->is_computed() && references_outside(n->get_property(), regions, name);
        }
        case ASTNode::Type::OPTIONAL_CHAINING_EXPRESSION: {
            const auto* n = static_cast<const OptionalChainingExpression*>(node);
            if (references_outside(n->get_object(), regions, name)) return true;
            return n->is_computed() && references_outside(n->get_property(), regions, name);
        }
        case ASTNode::Type::SPREAD_ELEMENT:
            return references_outside(static_cast<const SpreadElement*>(node)->get_argument(), regions, name);
        case ASTNode::Type::TEMPLATE_LITERAL: {
            const auto* n = static_cast<const TemplateLiteral*>(node);
            for (const auto& el : n->get_elements())
                if (el.type == TemplateLiteral::Element::Type::EXPRESSION &&
                    references_outside(el.expression.get(), regions, name)) return true;
            return false;
        }
        case ASTNode::Type::OBJECT_LITERAL: {
            const auto* n = static_cast<const ObjectLiteral*>(node);
            for (const auto& prop : n->get_properties()) {
                if (prop->computed && prop->key && references_outside(prop->key.get(), regions, name)) return true;
                if (prop->value && references_outside(prop->value.get(), regions, name)) return true;
            }
            return false;
        }
        case ASTNode::Type::ARRAY_LITERAL: {
            const auto* n = static_cast<const ArrayLiteral*>(node);
            for (const auto& el : n->get_elements())
                if (references_outside(el.get(), regions, name)) return true;
            return false;
        }
        case ASTNode::Type::YIELD_EXPRESSION: {
            const auto* n = static_cast<const YieldExpression*>(node);
            return n->get_argument() && references_outside(n->get_argument(), regions, name);
        }
        case ASTNode::Type::AWAIT_EXPRESSION: {
            const auto* n = static_cast<const AwaitExpression*>(node);
            return n->get_argument() && references_outside(n->get_argument(), regions, name);
        }
        default:
            return true;  // unrecognized node: conservative, same as collect_closure_names's `unknown`
    }
}

}

bool BytecodeCompiler::references_identifier(const ASTNode* node, const std::string& name) {
    std::unordered_set<std::string> names;
    ScanOpacity op;
    collect_closure_names(node, /*inside_closure=*/true, names, op);
    return op.opaque() || names.count(name) > 0;
}

// Whether a closure literal (params + body) needs its captured environment
// kept alive at all -- false only when collect_free_names (above, still
// callable from here despite its anonymous namespace) positively proves
// every name the body touches resolves within its own scope. External
// linkage (declared in BytecodeCompiler.h): called from FunctionExpression::
// evaluate, which caches the result per AST node instead of recomputing it
// on every instantiation.
bool closure_needs_outer_environment(const ParamList& params,
                                      const ASTNode* body, bool is_arrow) {
    for (size_t pidx = 0; pidx < params.size(); pidx++) {
        if (params.has_pattern(pidx) || params.has_default(pidx)) return true;
    }
    std::vector<DeclInfo> declared;
    if (!prescan_declarations(body, declared)) return true;
    std::vector<std::unordered_set<std::string>> scope_stack;
    std::unordered_set<std::string> frame;
    for (size_t pidx = 0; pidx < params.size(); pidx++) frame.insert(params.name(pidx));
    for (const auto& info : declared) {
        if (!info.is_lexical) frame.insert(info.name);
    }
    scope_stack.push_back(std::move(frame));
    std::unordered_set<std::string> free_names;
    ScanOpacity op;
    collect_free_names(body, scope_stack, is_arrow, free_names, op);
    return op.saw_eval || op.saw_class || op.unknown || !free_names.empty();
}

bool method_body_references_super(const ASTNode* body) {
    if (!body) return true;
    std::unordered_set<std::string> names;
    ScanOpacity op;
    collect_closure_names(body, /*inside_closure=*/true, names, op, /*suspendable=*/false);
    // A direct eval's text can name `super`, and a shape the scan could not
    // read may too. Only a body proved to mention it nowhere may skip the
    // [[HomeObject]] write.
    return op.opaque() || names.count("super") > 0;
}

// Same question as method_body_references_super, asked of the method's own
// FunctionExpression node instead of an already-unwrapped body -- lets a
// caller holding onto the property/class-element value (which is what both
// the object-literal and the class compilers actually have in hand) ask
// directly. Shared so the two compilers' answers can never quietly diverge.
bool method_value_references_super(const ASTNode* fn_node) {
    if (!fn_node || fn_node->get_type() != ASTNode::Type::FUNCTION_EXPRESSION) {
        return true;  // unknown shape: conservative, keep the write
    }
    const auto* fe = static_cast<const FunctionExpression*>(fn_node);
    if (fe->get_body()) return method_body_references_super(fe->get_body());
    // A body already let go of cannot be scanned here, but it was scanned
    // once, when it was first read -- same idiom as captures_outer (see
    // Parser::parse_object_literal's method path): the parse wrote the
    // answer down where a stepped-over body's other facts already live.
    if (ScriptUnit* unit = fe->owning_unit()) {
        if (const BodyScopeInfo* info = unit->scope_info_at(fe->body_source_first())) {
            return info->super_anywhere;
        }
    }
    return true;  // no record: keep the write
}

namespace {

// True if `node` references `super` (property or call) or a private name
// (#x) anywhere within it. Forces env_mode: these forms are delegated whole
// to the tree-walker's own evaluate() (see the MEMBER_EXPRESSION/CALL_EXPRESSION/
// BINARY_EXPRESSION cases in compile_expression), which needs `this`,
// `__super__`/`__home_object__`/`__eval_private_names__` and any locals the
// delegated subtree captures to be resolvable through a real Environment.
// Same descend-into-arrows-not-nested-functions rule as uses_arguments,
// since arrows share the enclosing `this`/super/private context.
bool uses_super_or_private(const ASTNode* node) {
    if (!node) return false;
    switch (node->get_type()) {
        case ASTNode::Type::FUNCTION_EXPRESSION:
        case ASTNode::Type::FUNCTION_DECLARATION:
            return false;
        case ASTNode::Type::ARROW_FUNCTION_EXPRESSION:
            return uses_super_or_private(static_cast<const ArrowFunctionExpression*>(node)->get_body());
        case ASTNode::Type::ASYNC_FUNCTION_EXPRESSION: {
            // Async arrows share the enclosing this/super/private context.
            const auto* n = static_cast<const AsyncFunctionExpression*>(node);
            return n->is_arrow() && uses_super_or_private(n->get_body());
        }
        case ASTNode::Type::CLASS_DECLARATION: {
            const auto* n = static_cast<const ClassDeclaration*>(node);
            return uses_super_or_private(n->get_superclass()) || uses_super_or_private(n->get_body());
        }
        case ASTNode::Type::METHOD_DEFINITION: {
            const auto* n = static_cast<const MethodDefinition*>(node);
            return n->is_computed() && uses_super_or_private(n->get_key());
        }
        case ASTNode::Type::CLASS_FIELD: {
            const auto* n = static_cast<const ClassField*>(node);
            return n->is_computed() && uses_super_or_private(n->get_key());
        }
        case ASTNode::Type::BLOCK_STATEMENT: {
            const auto* n = static_cast<const BlockStatement*>(node);
            for (const auto& stmt : n->get_statements()) {
                if (uses_super_or_private(stmt.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::IF_STATEMENT: {
            const auto* n = static_cast<const IfStatement*>(node);
            return uses_super_or_private(n->get_test()) || uses_super_or_private(n->get_consequent()) ||
                   uses_super_or_private(n->get_alternate());
        }
        case ASTNode::Type::WHILE_STATEMENT: {
            const auto* n = static_cast<const WhileStatement*>(node);
            return uses_super_or_private(n->get_test()) || uses_super_or_private(n->get_body());
        }
        case ASTNode::Type::DO_WHILE_STATEMENT: {
            const auto* n = static_cast<const DoWhileStatement*>(node);
            return uses_super_or_private(n->get_body()) || uses_super_or_private(n->get_test());
        }
        case ASTNode::Type::FOR_STATEMENT: {
            const auto* n = static_cast<const ForStatement*>(node);
            return uses_super_or_private(n->get_init()) || uses_super_or_private(n->get_test()) ||
                   uses_super_or_private(n->get_update()) || uses_super_or_private(n->get_body());
        }
        case ASTNode::Type::FOR_OF_STATEMENT: {
            const auto* n = static_cast<const ForOfStatement*>(node);
            return uses_super_or_private(n->get_right()) || uses_super_or_private(n->get_body());
        }
        case ASTNode::Type::FOR_IN_STATEMENT: {
            const auto* n = static_cast<const ForInStatement*>(node);
            return uses_super_or_private(n->get_right()) || uses_super_or_private(n->get_body());
        }
        case ASTNode::Type::TRY_STATEMENT: {
            const auto* n = static_cast<const TryStatement*>(node);
            if (uses_super_or_private(n->get_try_block())) return true;
            if (const ASTNode* cc = n->get_catch_clause()) {
                if (uses_super_or_private(static_cast<const CatchClause*>(cc)->get_body())) return true;
            }
            return uses_super_or_private(n->get_finally_block());
        }
        case ASTNode::Type::SWITCH_STATEMENT: {
            const auto* n = static_cast<const SwitchStatement*>(node);
            if (uses_super_or_private(n->get_discriminant())) return true;
            for (const auto& c : n->get_cases()) {
                const auto* cc = static_cast<const CaseClause*>(c.get());
                if (cc->get_test() && uses_super_or_private(cc->get_test())) return true;
                for (const auto& s : cc->get_consequent()) {
                    if (uses_super_or_private(s.get())) return true;
                }
            }
            return false;
        }
        case ASTNode::Type::LABELED_STATEMENT:
            return uses_super_or_private(static_cast<const LabeledStatement*>(node)->get_statement());
        case ASTNode::Type::EXPRESSION_STATEMENT:
            return uses_super_or_private(static_cast<const ExpressionStatement*>(node)->get_expression());
        case ASTNode::Type::RETURN_STATEMENT: {
            const auto* n = static_cast<const ReturnStatement*>(node);
            return n->get_argument() && uses_super_or_private(n->get_argument());
        }
        case ASTNode::Type::THROW_STATEMENT:
            return uses_super_or_private(static_cast<const ThrowStatement*>(node)->get_expression());
        case ASTNode::Type::VARIABLE_DECLARATION: {
            const auto* n = static_cast<const VariableDeclaration*>(node);
            for (const auto& d : n->get_declarations()) {
                if (d->get_init() && uses_super_or_private(d->get_init())) return true;
            }
            return false;
        }
        case ASTNode::Type::DESTRUCTURING_ASSIGNMENT: {
            const auto* n = static_cast<const DestructuringAssignment*>(node);
            if (n->get_source() && uses_super_or_private(n->get_source())) return true;
            bool found = false;
            n->for_each_expression([&](const ASTNode* e) { if (!found && uses_super_or_private(e)) found = true; });
            return found;
        }
        case ASTNode::Type::ASSIGNMENT_EXPRESSION: {
            const auto* n = static_cast<const AssignmentExpression*>(node);
            return uses_super_or_private(n->get_left()) || uses_super_or_private(n->get_right());
        }
        case ASTNode::Type::UNARY_EXPRESSION:
            return uses_super_or_private(static_cast<const UnaryExpression*>(node)->get_operand());
        case ASTNode::Type::BINARY_EXPRESSION: {
            const auto* n = static_cast<const BinaryExpression*>(node);
            if (n->get_operator() == BinaryExpression::Operator::IN &&
                n->get_left()->get_type() == ASTNode::Type::IDENTIFIER &&
                !static_cast<const Identifier*>(n->get_left())->get_name().empty() &&
                static_cast<const Identifier*>(n->get_left())->get_name()[0] == '#') {
                return true;
            }
            return uses_super_or_private(n->get_left()) || uses_super_or_private(n->get_right());
        }
        case ASTNode::Type::NULLISH_COALESCING_EXPRESSION: {
            const auto* n = static_cast<const NullishCoalescingExpression*>(node);
            return uses_super_or_private(n->get_left()) || uses_super_or_private(n->get_right());
        }
        case ASTNode::Type::CONDITIONAL_EXPRESSION: {
            const auto* n = static_cast<const ConditionalExpression*>(node);
            return uses_super_or_private(n->get_test()) || uses_super_or_private(n->get_consequent()) ||
                   uses_super_or_private(n->get_alternate());
        }
        case ASTNode::Type::CALL_EXPRESSION: {
            const auto* n = static_cast<const CallExpression*>(node);
            if (n->get_callee()->get_type() == ASTNode::Type::IDENTIFIER &&
                static_cast<const Identifier*>(n->get_callee())->get_name() == "super") {
                return true;
            }
            if (uses_super_or_private(n->get_callee())) return true;
            for (const auto& arg : n->get_arguments()) {
                if (uses_super_or_private(arg.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::NEW_EXPRESSION: {
            const auto* n = static_cast<const NewExpression*>(node);
            if (uses_super_or_private(n->get_constructor())) return true;
            for (const auto& arg : n->get_arguments()) {
                if (uses_super_or_private(arg.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::MEMBER_EXPRESSION: {
            const auto* n = static_cast<const MemberExpression*>(node);
            if (n->get_object()->get_type() == ASTNode::Type::IDENTIFIER &&
                static_cast<const Identifier*>(n->get_object())->get_name() == "super") {
                return true;
            }
            if (!n->is_computed() && n->get_property()->get_type() == ASTNode::Type::IDENTIFIER) {
                const std::string& pname = static_cast<const Identifier*>(n->get_property())->get_name();
                if (!pname.empty() && pname[0] == '#') return true;
            }
            return uses_super_or_private(n->get_object()) ||
                   (n->is_computed() && uses_super_or_private(n->get_property()));
        }
        case ASTNode::Type::OPTIONAL_CHAINING_EXPRESSION: {
            const auto* n = static_cast<const OptionalChainingExpression*>(node);
            return uses_super_or_private(n->get_object()) ||
                   (n->is_computed() && uses_super_or_private(n->get_property()));
        }
        case ASTNode::Type::SPREAD_ELEMENT:
            return uses_super_or_private(static_cast<const SpreadElement*>(node)->get_argument());
        case ASTNode::Type::TEMPLATE_LITERAL: {
            const auto* n = static_cast<const TemplateLiteral*>(node);
            for (const auto& el : n->get_elements()) {
                if (el.type == TemplateLiteral::Element::Type::EXPRESSION &&
                    uses_super_or_private(el.expression.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::OBJECT_LITERAL: {
            const auto* n = static_cast<const ObjectLiteral*>(node);
            for (const auto& prop : n->get_properties()) {
                if (prop->value && uses_super_or_private(prop->value.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::ARRAY_LITERAL: {
            const auto* n = static_cast<const ArrayLiteral*>(node);
            for (const auto& el : n->get_elements()) {
                if (el && uses_super_or_private(el.get())) return true;
            }
            return false;
        }
        default:
            return false;
    }
}

// True if `node` contains a yield/await anywhere in the CURRENT function's own
// code (descends into arrows, stops at nested function/class-body boundaries --
// same rule as uses_arguments, since yield/await belong to the enclosing
// suspendable function).
// Whether a `with` appears anywhere in this body, nested functions excluded --
// theirs is their own body's problem. A shape this walk does not know answers
// no, which only costs the function its compilation: the `with` case refuses
// without full env_mode rather than emitting something wrong.
// The oracle the parser-computed bit is checked against, and the answer
// for any tree the parser did not fill in.
bool contains_with_by_walk(const ASTNode* node) {
    if (!node) return false;
    switch (node->get_type()) {
        case ASTNode::Type::WITH_STATEMENT:
            return true;
        case ASTNode::Type::FUNCTION_EXPRESSION:
        case ASTNode::Type::FUNCTION_DECLARATION:
        case ASTNode::Type::CLASS_DECLARATION:
            return false;
        case ASTNode::Type::ASYNC_FUNCTION_EXPRESSION: {
            const auto* n = static_cast<const AsyncFunctionExpression*>(node);
            return n->is_arrow() && contains_with_by_walk(n->get_body());
        }
        case ASTNode::Type::ARROW_FUNCTION_EXPRESSION:
            return contains_with_by_walk(static_cast<const ArrowFunctionExpression*>(node)->get_body());
        case ASTNode::Type::BLOCK_STATEMENT: {
            const auto* n = static_cast<const BlockStatement*>(node);
            for (const auto& s : n->get_statements()) if (contains_with_by_walk(s.get())) return true;
            return false;
        }
        case ASTNode::Type::IF_STATEMENT: {
            const auto* n = static_cast<const IfStatement*>(node);
            return contains_with_by_walk(n->get_test()) || contains_with_by_walk(n->get_consequent()) ||
                   contains_with_by_walk(n->get_alternate());
        }
        case ASTNode::Type::WHILE_STATEMENT: {
            const auto* n = static_cast<const WhileStatement*>(node);
            return contains_with_by_walk(n->get_test()) || contains_with_by_walk(n->get_body());
        }
        case ASTNode::Type::DO_WHILE_STATEMENT: {
            const auto* n = static_cast<const DoWhileStatement*>(node);
            return contains_with_by_walk(n->get_body()) || contains_with_by_walk(n->get_test());
        }
        case ASTNode::Type::FOR_STATEMENT: {
            const auto* n = static_cast<const ForStatement*>(node);
            return contains_with_by_walk(n->get_init()) || contains_with_by_walk(n->get_test()) ||
                   contains_with_by_walk(n->get_update()) || contains_with_by_walk(n->get_body());
        }
        case ASTNode::Type::FOR_OF_STATEMENT: {
            const auto* n = static_cast<const ForOfStatement*>(node);
            return contains_with_by_walk(n->get_right()) || contains_with_by_walk(n->get_body());
        }
        case ASTNode::Type::FOR_IN_STATEMENT: {
            const auto* n = static_cast<const ForInStatement*>(node);
            return contains_with_by_walk(n->get_right()) || contains_with_by_walk(n->get_body());
        }
        case ASTNode::Type::TRY_STATEMENT: {
            const auto* n = static_cast<const TryStatement*>(node);
            if (contains_with_by_walk(n->get_try_block())) return true;
            if (const ASTNode* cc = n->get_catch_clause()) {
                if (contains_with_by_walk(static_cast<const CatchClause*>(cc)->get_body())) return true;
            }
            return contains_with_by_walk(n->get_finally_block());
        }
        case ASTNode::Type::SWITCH_STATEMENT: {
            const auto* n = static_cast<const SwitchStatement*>(node);
            if (contains_with_by_walk(n->get_discriminant())) return true;
            for (const auto& c : n->get_cases()) {
                const auto* cc = static_cast<const CaseClause*>(c.get());
                if (cc->get_test() && contains_with_by_walk(cc->get_test())) return true;
                for (const auto& s : cc->get_consequent()) if (contains_with_by_walk(s.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::LABELED_STATEMENT:
            return contains_with_by_walk(static_cast<const LabeledStatement*>(node)->get_statement());
        case ASTNode::Type::EXPRESSION_STATEMENT:
            return contains_with_by_walk(static_cast<const ExpressionStatement*>(node)->get_expression());
        case ASTNode::Type::RETURN_STATEMENT: {
            const auto* n = static_cast<const ReturnStatement*>(node);
            return n->get_argument() && contains_with_by_walk(n->get_argument());
        }
        case ASTNode::Type::THROW_STATEMENT:
            return contains_with_by_walk(static_cast<const ThrowStatement*>(node)->get_expression());
        case ASTNode::Type::VARIABLE_DECLARATION: {
            const auto* n = static_cast<const VariableDeclaration*>(node);
            for (const auto& d : n->get_declarations()) {
                if (d->get_init() && contains_with_by_walk(d->get_init())) return true;
            }
            return false;
        }
        case ASTNode::Type::DESTRUCTURING_ASSIGNMENT: {
            const auto* n = static_cast<const DestructuringAssignment*>(node);
            if (n->get_source() && contains_with_by_walk(n->get_source())) return true;
            bool found = false;
            n->for_each_expression([&](const ASTNode* e) { if (!found && contains_with_by_walk(e)) found = true; });
            return found;
        }
        case ASTNode::Type::ASSIGNMENT_EXPRESSION: {
            const auto* n = static_cast<const AssignmentExpression*>(node);
            return contains_with_by_walk(n->get_left()) || contains_with_by_walk(n->get_right());
        }
        case ASTNode::Type::UNARY_EXPRESSION:
            return contains_with_by_walk(static_cast<const UnaryExpression*>(node)->get_operand());
        case ASTNode::Type::BINARY_EXPRESSION: {
            const auto* n = static_cast<const BinaryExpression*>(node);
            return contains_with_by_walk(n->get_left()) || contains_with_by_walk(n->get_right());
        }
        case ASTNode::Type::NULLISH_COALESCING_EXPRESSION: {
            const auto* n = static_cast<const NullishCoalescingExpression*>(node);
            return contains_with_by_walk(n->get_left()) || contains_with_by_walk(n->get_right());
        }
        case ASTNode::Type::CONDITIONAL_EXPRESSION: {
            const auto* n = static_cast<const ConditionalExpression*>(node);
            return contains_with_by_walk(n->get_test()) || contains_with_by_walk(n->get_consequent()) ||
                   contains_with_by_walk(n->get_alternate());
        }
        case ASTNode::Type::CALL_EXPRESSION: {
            const auto* n = static_cast<const CallExpression*>(node);
            if (contains_with_by_walk(n->get_callee())) return true;
            for (const auto& a : n->get_arguments()) if (contains_with_by_walk(a.get())) return true;
            return false;
        }
        case ASTNode::Type::NEW_EXPRESSION: {
            const auto* n = static_cast<const NewExpression*>(node);
            if (contains_with_by_walk(n->get_constructor())) return true;
            for (const auto& a : n->get_arguments()) if (contains_with_by_walk(a.get())) return true;
            return false;
        }
        case ASTNode::Type::MEMBER_EXPRESSION: {
            const auto* n = static_cast<const MemberExpression*>(node);
            return contains_with_by_walk(n->get_object()) ||
                   (n->is_computed() && contains_with_by_walk(n->get_property()));
        }
        case ASTNode::Type::OPTIONAL_CHAINING_EXPRESSION: {
            const auto* n = static_cast<const OptionalChainingExpression*>(node);
            return contains_with_by_walk(n->get_object()) ||
                   (n->is_computed() && contains_with_by_walk(n->get_property()));
        }
        case ASTNode::Type::SPREAD_ELEMENT:
            return contains_with_by_walk(static_cast<const SpreadElement*>(node)->get_argument());
        case ASTNode::Type::TEMPLATE_LITERAL: {
            const auto* n = static_cast<const TemplateLiteral*>(node);
            for (const auto& el : n->get_elements()) {
                if (el.type == TemplateLiteral::Element::Type::EXPRESSION &&
                    contains_with_by_walk(el.expression.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::OBJECT_LITERAL: {
            const auto* n = static_cast<const ObjectLiteral*>(node);
            for (const auto& p : n->get_properties()) {
                if (p->value && contains_with_by_walk(p->value.get())) return true;
                if (p->computed && p->key && contains_with_by_walk(p->key.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::ARRAY_LITERAL: {
            const auto* n = static_cast<const ArrayLiteral*>(node);
            for (const auto& el : n->get_elements()) {
                if (el && contains_with_by_walk(el.get())) return true;
            }
            return false;
        }
        default:
            return false;
    }
}

bool contains_with(const ASTNode* node) {
    if (!node) return false;
    const uint32_t flags = node->subtree_flags();
    if (!(flags & kSubtreeComputed)) return contains_with_by_walk(node);
    const bool bit = (flags & kSubtreeWith) != 0;
#ifdef QUANTA_VALIDATE_BYTECODE
    if (std::getenv("QUANTA_SUBTREE_CHECK")) {
        const bool walked = contains_with_by_walk(node);
        if (bit != walked) {
            std::fprintf(stderr, "[subtree] contains_with bit=%d walk=%d node_type=%d\n",
                         bit ? 1 : 0, walked ? 1 : 0, static_cast<int>(node->get_type()));
        }
    }
#endif
    return bit;
}

// The oracle the parser-computed bit is checked against, and the answer for
// any tree the parser did not fill in (a clone, or a form not migrated).
bool contains_suspend_by_walk(const ASTNode* node) {
    if (!node) return false;
    switch (node->get_type()) {
        case ASTNode::Type::YIELD_EXPRESSION:
        case ASTNode::Type::AWAIT_EXPRESSION:
            return true;
        case ASTNode::Type::FUNCTION_EXPRESSION:
        case ASTNode::Type::FUNCTION_DECLARATION:
        case ASTNode::Type::CLASS_DECLARATION:
            return false;
        case ASTNode::Type::ASYNC_FUNCTION_EXPRESSION: {
            const auto* n = static_cast<const AsyncFunctionExpression*>(node);
            return n->is_arrow() && contains_suspend_by_walk(n->get_body());
        }
        case ASTNode::Type::ARROW_FUNCTION_EXPRESSION: {
            // A `yield` or `await` written in an arrow belongs to the function
            // around it, so a body stepped over has to be assumed to hold one.
            const auto* n = static_cast<const ArrowFunctionExpression*>(node);
            return !n->get_body() || contains_suspend_by_walk(n->get_body());
        }
        case ASTNode::Type::BLOCK_STATEMENT: {
            const auto* n = static_cast<const BlockStatement*>(node);
            for (const auto& s : n->get_statements()) if (contains_suspend_by_walk(s.get())) return true;
            return false;
        }
        case ASTNode::Type::EXPORT_STATEMENT: {
            // `export default await x` and `export var y = await z` suspend the
            // module exactly as the bare expressions would.
            const auto* n = static_cast<const ExportStatement*>(node);
            return contains_suspend_by_walk(n->get_default_export()) ||
                   contains_suspend_by_walk(n->get_declaration());
        }
        case ASTNode::Type::IF_STATEMENT: {
            const auto* n = static_cast<const IfStatement*>(node);
            return contains_suspend_by_walk(n->get_test()) || contains_suspend_by_walk(n->get_consequent()) ||
                   contains_suspend_by_walk(n->get_alternate());
        }
        case ASTNode::Type::WHILE_STATEMENT: {
            const auto* n = static_cast<const WhileStatement*>(node);
            return contains_suspend_by_walk(n->get_test()) || contains_suspend_by_walk(n->get_body());
        }
        case ASTNode::Type::DO_WHILE_STATEMENT: {
            const auto* n = static_cast<const DoWhileStatement*>(node);
            return contains_suspend_by_walk(n->get_body()) || contains_suspend_by_walk(n->get_test());
        }
        case ASTNode::Type::FOR_STATEMENT: {
            const auto* n = static_cast<const ForStatement*>(node);
            return contains_suspend_by_walk(n->get_init()) || contains_suspend_by_walk(n->get_test()) ||
                   contains_suspend_by_walk(n->get_update()) || contains_suspend_by_walk(n->get_body());
        }
        case ASTNode::Type::FOR_OF_STATEMENT: {
            const auto* n = static_cast<const ForOfStatement*>(node);
            return contains_suspend_by_walk(n->get_right()) || contains_suspend_by_walk(n->get_body());
        }
        case ASTNode::Type::FOR_IN_STATEMENT: {
            const auto* n = static_cast<const ForInStatement*>(node);
            return contains_suspend_by_walk(n->get_right()) || contains_suspend_by_walk(n->get_body());
        }
        case ASTNode::Type::TRY_STATEMENT: {
            const auto* n = static_cast<const TryStatement*>(node);
            if (contains_suspend_by_walk(n->get_try_block())) return true;
            if (const ASTNode* cc = n->get_catch_clause()) {
                if (contains_suspend_by_walk(static_cast<const CatchClause*>(cc)->get_body())) return true;
            }
            return contains_suspend_by_walk(n->get_finally_block());
        }
        case ASTNode::Type::SWITCH_STATEMENT: {
            const auto* n = static_cast<const SwitchStatement*>(node);
            if (contains_suspend_by_walk(n->get_discriminant())) return true;
            for (const auto& c : n->get_cases()) {
                const auto* cc = static_cast<const CaseClause*>(c.get());
                if (cc->get_test() && contains_suspend_by_walk(cc->get_test())) return true;
                for (const auto& s : cc->get_consequent()) if (contains_suspend_by_walk(s.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::LABELED_STATEMENT:
            return contains_suspend_by_walk(static_cast<const LabeledStatement*>(node)->get_statement());
        case ASTNode::Type::EXPRESSION_STATEMENT:
            return contains_suspend_by_walk(static_cast<const ExpressionStatement*>(node)->get_expression());
        case ASTNode::Type::RETURN_STATEMENT: {
            const auto* n = static_cast<const ReturnStatement*>(node);
            return n->get_argument() && contains_suspend_by_walk(n->get_argument());
        }
        case ASTNode::Type::THROW_STATEMENT:
            return contains_suspend_by_walk(static_cast<const ThrowStatement*>(node)->get_expression());
        case ASTNode::Type::VARIABLE_DECLARATION: {
            const auto* n = static_cast<const VariableDeclaration*>(node);
            for (const auto& d : n->get_declarations()) {
                if (d->get_init() && contains_suspend_by_walk(d->get_init())) return true;
            }
            return false;
        }
        case ASTNode::Type::DESTRUCTURING_ASSIGNMENT: {
            const auto* n = static_cast<const DestructuringAssignment*>(node);
            if (n->get_source() && contains_suspend_by_walk(n->get_source())) return true;
            bool found = false;
            n->for_each_expression([&](const ASTNode* e) { if (!found && contains_suspend_by_walk(e)) found = true; });
            return found;
        }
        case ASTNode::Type::ASSIGNMENT_EXPRESSION: {
            const auto* n = static_cast<const AssignmentExpression*>(node);
            return contains_suspend_by_walk(n->get_left()) || contains_suspend_by_walk(n->get_right());
        }
        case ASTNode::Type::UNARY_EXPRESSION:
            return contains_suspend_by_walk(static_cast<const UnaryExpression*>(node)->get_operand());
        case ASTNode::Type::BINARY_EXPRESSION: {
            const auto* n = static_cast<const BinaryExpression*>(node);
            return contains_suspend_by_walk(n->get_left()) || contains_suspend_by_walk(n->get_right());
        }
        case ASTNode::Type::NULLISH_COALESCING_EXPRESSION: {
            const auto* n = static_cast<const NullishCoalescingExpression*>(node);
            return contains_suspend_by_walk(n->get_left()) || contains_suspend_by_walk(n->get_right());
        }
        case ASTNode::Type::CONDITIONAL_EXPRESSION: {
            const auto* n = static_cast<const ConditionalExpression*>(node);
            return contains_suspend_by_walk(n->get_test()) || contains_suspend_by_walk(n->get_consequent()) ||
                   contains_suspend_by_walk(n->get_alternate());
        }
        case ASTNode::Type::CALL_EXPRESSION: {
            const auto* n = static_cast<const CallExpression*>(node);
            if (contains_suspend_by_walk(n->get_callee())) return true;
            for (const auto& a : n->get_arguments()) if (contains_suspend_by_walk(a.get())) return true;
            return false;
        }
        case ASTNode::Type::NEW_EXPRESSION: {
            const auto* n = static_cast<const NewExpression*>(node);
            if (contains_suspend_by_walk(n->get_constructor())) return true;
            for (const auto& a : n->get_arguments()) if (contains_suspend_by_walk(a.get())) return true;
            return false;
        }
        case ASTNode::Type::MEMBER_EXPRESSION: {
            const auto* n = static_cast<const MemberExpression*>(node);
            return contains_suspend_by_walk(n->get_object()) ||
                   (n->is_computed() && contains_suspend_by_walk(n->get_property()));
        }
        case ASTNode::Type::OPTIONAL_CHAINING_EXPRESSION: {
            const auto* n = static_cast<const OptionalChainingExpression*>(node);
            return contains_suspend_by_walk(n->get_object()) ||
                   (n->is_computed() && contains_suspend_by_walk(n->get_property()));
        }
        case ASTNode::Type::SPREAD_ELEMENT:
            return contains_suspend_by_walk(static_cast<const SpreadElement*>(node)->get_argument());
        case ASTNode::Type::TEMPLATE_LITERAL: {
            const auto* n = static_cast<const TemplateLiteral*>(node);
            for (const auto& el : n->get_elements()) {
                if (el.type == TemplateLiteral::Element::Type::EXPRESSION &&
                    contains_suspend_by_walk(el.expression.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::OBJECT_LITERAL: {
            const auto* n = static_cast<const ObjectLiteral*>(node);
            for (const auto& p : n->get_properties()) {
                if (p->value && contains_suspend_by_walk(p->value.get())) return true;
                if (p->computed && p->key && contains_suspend_by_walk(p->key.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::ARRAY_LITERAL: {
            const auto* n = static_cast<const ArrayLiteral*>(node);
            for (const auto& el : n->get_elements()) {
                if (el && contains_suspend_by_walk(el.get())) return true;
            }
            return false;
        }
        default:
            return false;
    }
}

bool contains_suspend(const ASTNode* node) {
    if (!node) return false;
    const uint32_t flags = node->subtree_flags();
    if (!(flags & kSubtreeComputed)) return contains_suspend_by_walk(node);
    const bool bit = (flags & kSubtreeSuspend) != 0;
#ifdef QUANTA_VALIDATE_BYTECODE
    // While the parser is taking these facts over, the two answers have to
    // agree. Reported only when asked for: the conformance runner reads
    // stderr and a stray line there reads as a failure.
    if (std::getenv("QUANTA_SUBTREE_CHECK")) {
        const bool walked = contains_suspend_by_walk(node);
        if (bit != walked) {
            std::fprintf(stderr, "[subtree] suspend bit=%d walk=%d node_type=%d\n",
                         bit ? 1 : 0, walked ? 1 : 0, static_cast<int>(node->get_type()));
        }
    }
#endif
    return bit;
}


// True if `node` contains a `let/const/var [a,b]=...` declaration anywhere.
// Forces env_mode: a pattern binds through a real Environment.
bool contains_destructuring(const ASTNode* node) {
    if (!node) return false;
    switch (node->get_type()) {
        case ASTNode::Type::BLOCK_STATEMENT: {
            const auto* n = static_cast<const BlockStatement*>(node);
            for (const auto& stmt : n->get_statements()) {
                if (contains_destructuring(stmt.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::IF_STATEMENT: {
            const auto* n = static_cast<const IfStatement*>(node);
            return contains_destructuring(n->get_consequent()) ||
                   contains_destructuring(n->get_alternate());
        }
        case ASTNode::Type::WHILE_STATEMENT:
            return contains_destructuring(static_cast<const WhileStatement*>(node)->get_body());
        case ASTNode::Type::DO_WHILE_STATEMENT:
            return contains_destructuring(static_cast<const DoWhileStatement*>(node)->get_body());
        case ASTNode::Type::FOR_STATEMENT: {
            const auto* n = static_cast<const ForStatement*>(node);
            return contains_destructuring(n->get_init()) || contains_destructuring(n->get_body());
        }
        case ASTNode::Type::FOR_OF_STATEMENT: {
            const auto* n = static_cast<const ForOfStatement*>(node);
            return n->get_left()->get_type() == ASTNode::Type::DESTRUCTURING_ASSIGNMENT ||
                   contains_destructuring(n->get_body());
        }
        case ASTNode::Type::FOR_IN_STATEMENT: {
            const auto* n = static_cast<const ForInStatement*>(node);
            return n->get_left()->get_type() == ASTNode::Type::DESTRUCTURING_ASSIGNMENT ||
                   contains_destructuring(n->get_body());
        }
        case ASTNode::Type::TRY_STATEMENT: {
            const auto* n = static_cast<const TryStatement*>(node);
            if (contains_destructuring(n->get_try_block())) return true;
            if (const ASTNode* cc = n->get_catch_clause()) {
                if (contains_destructuring(static_cast<const CatchClause*>(cc)->get_body())) return true;
            }
            return contains_destructuring(n->get_finally_block());
        }
        case ASTNode::Type::SWITCH_STATEMENT: {
            const auto* n = static_cast<const SwitchStatement*>(node);
            for (const auto& c : n->get_cases()) {
                for (const auto& s : static_cast<const CaseClause*>(c.get())->get_consequent()) {
                    if (contains_destructuring(s.get())) return true;
                }
            }
            return false;
        }
        case ASTNode::Type::LABELED_STATEMENT:
            return contains_destructuring(static_cast<const LabeledStatement*>(node)->get_statement());
        case ASTNode::Type::VARIABLE_DECLARATION: {
            const auto* n = static_cast<const VariableDeclaration*>(node);
            for (const auto& d : n->get_declarations()) {
                if (d->get_init() && d->get_init()->get_type() == ASTNode::Type::DESTRUCTURING_ASSIGNMENT) {
                    return true;
                }
            }
            return false;
        }
        default:
            return false;
    }
}

// True if `node` contains a let/const anywhere within it, including a named
// catch parameter (also a fresh per-catch binding).
bool contains_lexical_decl(const ASTNode* node) {
    if (!node) return false;
    switch (node->get_type()) {
        case ASTNode::Type::VARIABLE_DECLARATION:
            return static_cast<const VariableDeclaration*>(node)->get_kind() !=
                   VariableDeclarator::Kind::VAR;
        case ASTNode::Type::USING_DECLARATION:
            return true;  // `using` binds like a const
        case ASTNode::Type::CLASS_DECLARATION:
            return true;  // the class name is a lexical binding
        case ASTNode::Type::BLOCK_STATEMENT: {
            const auto* n = static_cast<const BlockStatement*>(node);
            for (const auto& stmt : n->get_statements()) {
                if (contains_lexical_decl(stmt.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::IF_STATEMENT: {
            const auto* n = static_cast<const IfStatement*>(node);
            return contains_lexical_decl(n->get_consequent()) ||
                   contains_lexical_decl(n->get_alternate());
        }
        case ASTNode::Type::WHILE_STATEMENT:
            return contains_lexical_decl(static_cast<const WhileStatement*>(node)->get_body());
        case ASTNode::Type::DO_WHILE_STATEMENT:
            return contains_lexical_decl(static_cast<const DoWhileStatement*>(node)->get_body());
        case ASTNode::Type::FOR_STATEMENT: {
            const auto* n = static_cast<const ForStatement*>(node);
            return contains_lexical_decl(n->get_init()) || contains_lexical_decl(n->get_body());
        }
        case ASTNode::Type::FOR_OF_STATEMENT: {
            const auto* n = static_cast<const ForOfStatement*>(node);
            return contains_lexical_decl(n->get_left()) || contains_lexical_decl(n->get_body());
        }
        case ASTNode::Type::FOR_IN_STATEMENT: {
            const auto* n = static_cast<const ForInStatement*>(node);
            return contains_lexical_decl(n->get_left()) || contains_lexical_decl(n->get_body());
        }
        case ASTNode::Type::TRY_STATEMENT: {
            const auto* n = static_cast<const TryStatement*>(node);
            if (contains_lexical_decl(n->get_try_block())) return true;
            if (const ASTNode* cc = n->get_catch_clause()) {
                const auto* clause = static_cast<const CatchClause*>(cc);
                if (!clause->get_parameter_name().empty()) return true;
                if (contains_lexical_decl(clause->get_body())) return true;
            }
            return contains_lexical_decl(n->get_finally_block());
        }
        case ASTNode::Type::SWITCH_STATEMENT: {
            const auto* n = static_cast<const SwitchStatement*>(node);
            for (const auto& c : n->get_cases()) {
                for (const auto& s : static_cast<const CaseClause*>(c.get())->get_consequent()) {
                    if (contains_lexical_decl(s.get())) return true;
                }
            }
            return false;
        }
        case ASTNode::Type::LABELED_STATEMENT:
            return contains_lexical_decl(static_cast<const LabeledStatement*>(node)->get_statement());
        default:
            return false;
    }
}

// True if a let/const sits outside the function's own flat top-level
// statements -- register mode has no runtime scope to pop there.
// The oracle the parser-computed bit is checked against, and the answer
// for any tree the parser did not fill in.
bool contains_nested_lexical_decl_by_walk(const BlockStatement* top_level_body) {
    for (const auto& stmt : top_level_body->get_statements()) {
        if (stmt->get_type() == ASTNode::Type::VARIABLE_DECLARATION) continue;
        if (contains_lexical_decl(stmt.get())) return true;
    }
    return false;
}

bool contains_nested_lexical_decl(const BlockStatement* top_level_body) {
    const uint32_t flags = top_level_body->subtree_flags();
    if (!(flags & kSubtreeComputed)) return contains_nested_lexical_decl_by_walk(top_level_body);
    const bool bit = (flags & kSubtreeNestedLexical) != 0;
#ifdef QUANTA_VALIDATE_BYTECODE
    if (std::getenv("QUANTA_SUBTREE_CHECK")) {
        const bool walked = contains_nested_lexical_decl_by_walk(top_level_body);
        if (bit != walked) {
            std::fprintf(stderr, "[subtree] nested_lexical bit=%d walk=%d satir=%u\n",
                         bit ? 1 : 0, walked ? 1 : 0, top_level_body->get_start().line);
        }
    }
#endif
    return bit;
}

// Direct (non-recursive) let/const declarations of `node`'s own top-level
// statements -- a nested block/if/loop's own names get their own environment.
// `needs_own_env`: the scope needs its own Environment even without a named
// var here (destructuring and class declarations create bindings themselves).
bool collect_direct_lexical_decls(const ASTNode* node,
                                   std::vector<BytecodeChunk::LoopEnvVar>& vars,
                                   bool& needs_own_env) {
    auto scan_one = [&](const ASTNode* stmt) -> bool {
        if (stmt->get_type() == ASTNode::Type::CLASS_DECLARATION) {
            needs_own_env = true;
            return true;
        }
        if (stmt->get_type() == ASTNode::Type::USING_DECLARATION) {
            // `using x = r` binds like a const: the name is the block's, and
            // nothing may reassign it.
            for (const auto& b : static_cast<const UsingDeclaration*>(stmt)->get_bindings()) {
                if (b.name.empty()) return false;
                vars.push_back({b.name, true, true, false});
            }
            return true;
        }
        if (stmt->get_type() != ASTNode::Type::VARIABLE_DECLARATION) return true;
        const auto* decl = static_cast<const VariableDeclaration*>(stmt);
        if (decl->get_kind() == VariableDeclarator::Kind::VAR) return true;
        bool is_const = decl->get_kind() == VariableDeclarator::Kind::CONST;
        for (const auto& d : decl->get_declarations()) {
            if (d->get_init() && d->get_init()->get_type() == ASTNode::Type::DESTRUCTURING_ASSIGNMENT) {
                needs_own_env = true;
                continue;
            }
            if (!d->get_id()) return false;
            const std::string& name = d->get_id()->get_name();
            if (name.empty()) continue;
            vars.push_back({name, true, is_const, false});
        }
        return true;
    };
    if (node->get_type() == ASTNode::Type::BLOCK_STATEMENT) {
        for (const auto& stmt : static_cast<const BlockStatement*>(node)->get_statements()) {
            if (!scan_one(stmt.get())) return false;
        }
        return true;
    }
    return scan_one(node);
}

// True if a return/break/continue could escape `node` (keeps `finally`
// "always runs" simple; conservative-true just costs a tree-walker fallback).
// `kind_mask` picks which escapes count: bit 0 return, bit 1 break, bit 2
// continue. A finally has to run before any of them, and they are implemented
// one at a time.
bool contains_control_escape(const ASTNode* node, int kind_mask = 7);

bool contains_control_escape(const ASTNode* node, int kind_mask) {
    if (!node) return false;
    switch (node->get_type()) {
        case ASTNode::Type::RETURN_STATEMENT:
            return (kind_mask & 1) != 0;
        case ASTNode::Type::BREAK_STATEMENT:
            return (kind_mask & 2) != 0;
        case ASTNode::Type::CONTINUE_STATEMENT:
            return (kind_mask & 4) != 0;
        case ASTNode::Type::BLOCK_STATEMENT: {
            const auto* n = static_cast<const BlockStatement*>(node);
            for (const auto& stmt : n->get_statements()) {
                if (contains_control_escape(stmt.get(), kind_mask)) return true;
            }
            return false;
        }
        case ASTNode::Type::IF_STATEMENT: {
            const auto* n = static_cast<const IfStatement*>(node);
            return contains_control_escape(n->get_consequent(), kind_mask) ||
                   contains_control_escape(n->get_alternate(), kind_mask);
        }
        case ASTNode::Type::WHILE_STATEMENT:
            return contains_control_escape(static_cast<const WhileStatement*>(node)->get_body());
        case ASTNode::Type::DO_WHILE_STATEMENT:
            return contains_control_escape(static_cast<const DoWhileStatement*>(node)->get_body());
        case ASTNode::Type::FOR_STATEMENT:
            return contains_control_escape(static_cast<const ForStatement*>(node)->get_body());
        case ASTNode::Type::FOR_OF_STATEMENT:
            return contains_control_escape(static_cast<const ForOfStatement*>(node)->get_body());
        case ASTNode::Type::FOR_IN_STATEMENT:
            return contains_control_escape(static_cast<const ForInStatement*>(node)->get_body());
        case ASTNode::Type::TRY_STATEMENT: {
            const auto* n = static_cast<const TryStatement*>(node);
            if (contains_control_escape(n->get_try_block(), kind_mask)) return true;
            if (const ASTNode* cc = n->get_catch_clause()) {
                if (contains_control_escape(static_cast<const CatchClause*>(cc)->get_body())) return true;
            }
            return contains_control_escape(n->get_finally_block(), kind_mask);
        }
        case ASTNode::Type::SWITCH_STATEMENT: {
            const auto* n = static_cast<const SwitchStatement*>(node);
            for (const auto& c : n->get_cases()) {
                for (const auto& s : static_cast<const CaseClause*>(c.get())->get_consequent()) {
                    if (contains_control_escape(s.get(), kind_mask)) return true;
                }
            }
            return false;
        }
        case ASTNode::Type::LABELED_STATEMENT:
            return contains_control_escape(static_cast<const LabeledStatement*>(node)->get_statement());
        default:
            return false;
    }
}

// Literal `.#name` on a non-super base -- the only form that is a private
// reference (a computed key spelling "#x" is an ordinary property).
bool member_is_private(const MemberExpression* mem) {
    if (mem->is_computed()) return false;
    if (mem->get_object()->get_type() == ASTNode::Type::IDENTIFIER &&
        static_cast<const Identifier*>(mem->get_object())->get_name() == "super") {
        return false;
    }
    if (mem->get_property()->get_type() != ASTNode::Type::IDENTIFIER) return false;
    const std::string& name = static_cast<const Identifier*>(mem->get_property())->get_name();
    return !name.empty() && name[0] == '#';
}

// NamedEvaluation candidates: AssignmentExpression::evaluate infers the
// function/class name from the LHS identifier, so those assignments stay
// on the tree-walker. (Named function expressions delegate too -- the
// runtime empty-name check can't run here, so be conservative.)
bool is_named_evaluation_rhs(const ASTNode* node) {
    if (!node) return false;
    auto t = node->get_type();
    return t == ASTNode::Type::FUNCTION_EXPRESSION ||
           t == ASTNode::Type::ARROW_FUNCTION_EXPRESSION ||
           t == ASTNode::Type::ASYNC_FUNCTION_EXPRESSION ||
           t == ASTNode::Type::CLASS_DECLARATION;
}

// A class cannot take its inferred name from Op::SetFunctionNameIfUnnamed:
// ClassDefinitionEvaluation applies the name before running static
// initializers, which can observe it (`class { static f = this.name }`),
// whereas the opcode only runs once the class is already built.
bool named_evaluation_is_class(const ASTNode* node) {
    return node && node->get_type() == ASTNode::Type::CLASS_DECLARATION;
}

// So the name goes on the node instead, as the tree-walker does before
// evaluating. A site's inferred name never varies between evaluations, so
// stamping it once while compiling says what the tree-walker says every time.
// A number written as a key is the string ToString gives it. The integer case
// is spelled out because it is the common one; everything else goes through
// the engine's own conversion, since C++'s default formatting differs from
// JavaScript's on the exponent forms (0.0000001 is "1e-7", not "1e-07").
std::string numeric_key_text(double n) {
    if (n == std::floor(n) && n >= LLONG_MIN && n <= LLONG_MAX) {
        return std::to_string(static_cast<long long>(n));
    }
    return Value(n).to_string();
}

void stamp_inferred_class_name(const ASTNode* init, const std::string& name) {
    if (!named_evaluation_is_class(init)) return;
    auto* cd = const_cast<ClassDeclaration*>(static_cast<const ClassDeclaration*>(init));
    if (cd->is_expression() && cd->get_id() && cd->get_id()->get_name().empty()) {
        cd->set_inferred_name(name);
    }
}

// Maps each nested lexical/class declaration to its "declaring region": a
// plain block/if-branch/while-body/etc is its own BlockStatement; a
// C-style `for (let i=...)` header is the WHOLE ForStatement (test/update/
// body legitimately see it, not an escape); a switch-case lexical is the
// WHOLE SwitchStatement (spec: one shared BlockDeclarationInstantiation).
// `var` is never a candidate (function-scoped). for-of/for-in headers get
// no entry (region_of.find fails -> caller keeps them env-resident,
// unchanged from today -- extending this there is future work). Catch
// parameters aren't visited (handled separately by the caller: Annex B's
// `catch(e){var e;}` needs two independently-mutable same-named bindings,
// which no register can model).
void collect_lexical_regions(const ASTNode* node, const ASTNode* current_region,
                              std::unordered_map<std::string, const ASTNode*>& out) {
    if (!node) return;
    switch (node->get_type()) {
        case ASTNode::Type::CLASS_DECLARATION: {
            const auto* cd = static_cast<const ClassDeclaration*>(node);
            if (!cd->is_expression() && cd->get_id() && !cd->get_id()->get_name().empty()) {
                out.emplace(cd->get_id()->get_name(), current_region);
            }
            return;
        }
        case ASTNode::Type::VARIABLE_DECLARATION: {
            const auto* decl = static_cast<const VariableDeclaration*>(node);
            if (decl->get_kind() == VariableDeclarator::Kind::VAR) return;
            for (const auto& d : decl->get_declarations()) {
                if (d->get_id() && !d->get_id()->get_name().empty()) {
                    out.emplace(d->get_id()->get_name(), current_region);
                }
            }
            return;
        }
        case ASTNode::Type::BLOCK_STATEMENT: {
            const auto* n = static_cast<const BlockStatement*>(node);
            for (const auto& stmt : n->get_statements()) collect_lexical_regions(stmt.get(), node, out);
            return;
        }
        case ASTNode::Type::IF_STATEMENT: {
            const auto* n = static_cast<const IfStatement*>(node);
            collect_lexical_regions(n->get_consequent(), current_region, out);
            collect_lexical_regions(n->get_alternate(), current_region, out);
            return;
        }
        case ASTNode::Type::WHILE_STATEMENT:
            collect_lexical_regions(static_cast<const WhileStatement*>(node)->get_body(), current_region, out);
            return;
        case ASTNode::Type::DO_WHILE_STATEMENT:
            collect_lexical_regions(static_cast<const DoWhileStatement*>(node)->get_body(), current_region, out);
            return;
        case ASTNode::Type::FOR_STATEMENT: {
            const auto* n = static_cast<const ForStatement*>(node);
            if (n->get_init() && n->get_init()->get_type() == ASTNode::Type::VARIABLE_DECLARATION) {
                const auto* vd = static_cast<const VariableDeclaration*>(n->get_init());
                if (vd->get_kind() != VariableDeclarator::Kind::VAR) {
                    for (const auto& d : vd->get_declarations()) {
                        if (d->get_id()) out.emplace(d->get_id()->get_name(), node);
                    }
                }
            }
            collect_lexical_regions(n->get_body(), current_region, out);
            return;
        }
        case ASTNode::Type::FOR_OF_STATEMENT:
            collect_lexical_regions(static_cast<const ForOfStatement*>(node)->get_body(), current_region, out);
            return;
        case ASTNode::Type::FOR_IN_STATEMENT:
            collect_lexical_regions(static_cast<const ForInStatement*>(node)->get_body(), current_region, out);
            return;
        case ASTNode::Type::TRY_STATEMENT: {
            const auto* n = static_cast<const TryStatement*>(node);
            collect_lexical_regions(n->get_try_block(), current_region, out);
            if (const ASTNode* cc = n->get_catch_clause())
                collect_lexical_regions(static_cast<const CatchClause*>(cc)->get_body(), current_region, out);
            collect_lexical_regions(n->get_finally_block(), current_region, out);
            return;
        }
        case ASTNode::Type::SWITCH_STATEMENT: {
            const auto* n = static_cast<const SwitchStatement*>(node);
            for (const auto& c : n->get_cases()) {
                for (const auto& s : static_cast<const CaseClause*>(c.get())->get_consequent())
                    collect_lexical_regions(s.get(), node, out);
            }
            return;
        }
        case ASTNode::Type::LABELED_STATEMENT:
            collect_lexical_regions(static_cast<const LabeledStatement*>(node)->get_statement(), current_region, out);
            return;
        default:
            return;
    }
}

// Multi-region variant of collect_lexical_regions above: that function's
// plain out.emplace() silently keeps only the FIRST region for a repeated
// name (env_resident's escape check only ever needs one region to prove
// "escapes or doesn't"). This variant instead records EVERY declaring
// region per name, plus each newly-introduced region's immediate enclosing
// region (parent_of) -- the raw material compute_sibling_safe_names needs
// to prove two same-named declarations are non-overlapping siblings, not
// one nested inside the other. Structurally identical traversal to
// collect_lexical_regions; kept as a separate function rather than
// generalizing that one, matching this file's own precedent (references_
// outside is a separate function from collect_closure_names for the same
// reason -- an existing, relied-upon contract shouldn't grow a parameter
// for one new caller).
void collect_lexical_regions_multi(
        const ASTNode* node, const ASTNode* current_region,
        std::unordered_map<std::string, std::vector<const ASTNode*>>& out,
        std::unordered_map<const ASTNode*, const ASTNode*>& parent_of) {
    if (!node) return;
    switch (node->get_type()) {
        case ASTNode::Type::CLASS_DECLARATION: {
            const auto* cd = static_cast<const ClassDeclaration*>(node);
            if (!cd->is_expression() && cd->get_id() && !cd->get_id()->get_name().empty()) {
                out[cd->get_id()->get_name()].push_back(current_region);
            }
            return;
        }
        case ASTNode::Type::VARIABLE_DECLARATION: {
            const auto* decl = static_cast<const VariableDeclaration*>(node);
            if (decl->get_kind() == VariableDeclarator::Kind::VAR) return;
            for (const auto& d : decl->get_declarations()) {
                if (d->get_id() && !d->get_id()->get_name().empty()) {
                    out[d->get_id()->get_name()].push_back(current_region);
                }
            }
            return;
        }
        case ASTNode::Type::BLOCK_STATEMENT: {
            const auto* n = static_cast<const BlockStatement*>(node);
            parent_of[node] = current_region;
            for (const auto& stmt : n->get_statements())
                collect_lexical_regions_multi(stmt.get(), node, out, parent_of);
            return;
        }
        case ASTNode::Type::IF_STATEMENT: {
            const auto* n = static_cast<const IfStatement*>(node);
            collect_lexical_regions_multi(n->get_consequent(), current_region, out, parent_of);
            collect_lexical_regions_multi(n->get_alternate(), current_region, out, parent_of);
            return;
        }
        case ASTNode::Type::WHILE_STATEMENT:
            collect_lexical_regions_multi(static_cast<const WhileStatement*>(node)->get_body(),
                                           current_region, out, parent_of);
            return;
        case ASTNode::Type::DO_WHILE_STATEMENT:
            collect_lexical_regions_multi(static_cast<const DoWhileStatement*>(node)->get_body(),
                                           current_region, out, parent_of);
            return;
        case ASTNode::Type::FOR_STATEMENT: {
            const auto* n = static_cast<const ForStatement*>(node);
            parent_of[node] = current_region;
            if (n->get_init() && n->get_init()->get_type() == ASTNode::Type::VARIABLE_DECLARATION) {
                const auto* vd = static_cast<const VariableDeclaration*>(n->get_init());
                if (vd->get_kind() != VariableDeclarator::Kind::VAR) {
                    for (const auto& d : vd->get_declarations()) {
                        if (d->get_id()) out[d->get_id()->get_name()].push_back(node);
                    }
                }
            }
            // The body is walked as a child of THIS statement, not of the
            // enclosing one: a for-head lexical's region is the for statement,
            // so anything nested inside it has to come out as a descendant.
            // Passing current_region here made an inner `for (let i...)` look
            // disjoint from an outer one with the same name, which is
            // shadowing, not sibling reuse.
            collect_lexical_regions_multi(n->get_body(), node, out, parent_of);
            return;
        }
        case ASTNode::Type::FOR_OF_STATEMENT:
            collect_lexical_regions_multi(static_cast<const ForOfStatement*>(node)->get_body(),
                                           current_region, out, parent_of);
            return;
        case ASTNode::Type::FOR_IN_STATEMENT:
            collect_lexical_regions_multi(static_cast<const ForInStatement*>(node)->get_body(),
                                           current_region, out, parent_of);
            return;
        case ASTNode::Type::TRY_STATEMENT: {
            const auto* n = static_cast<const TryStatement*>(node);
            collect_lexical_regions_multi(n->get_try_block(), current_region, out, parent_of);
            if (const ASTNode* cc = n->get_catch_clause()) {
                collect_lexical_regions_multi(static_cast<const CatchClause*>(cc)->get_body(),
                                               current_region, out, parent_of);
            }
            collect_lexical_regions_multi(n->get_finally_block(), current_region, out, parent_of);
            return;
        }
        case ASTNode::Type::SWITCH_STATEMENT: {
            const auto* n = static_cast<const SwitchStatement*>(node);
            parent_of[node] = current_region;
            for (const auto& c : n->get_cases()) {
                for (const auto& s : static_cast<const CaseClause*>(c.get())->get_consequent())
                    collect_lexical_regions_multi(s.get(), node, out, parent_of);
            }
            return;
        }
        case ASTNode::Type::LABELED_STATEMENT:
            collect_lexical_regions_multi(static_cast<const LabeledStatement*>(node)->get_statement(),
                                           current_region, out, parent_of);
            return;
        default:
            return;
    }
}

// Walks up from `node` via parent_of looking for `ancestor`. nullptr (the
// implicit function/script root) is a valid terminal parent, never itself
// looked up.
bool is_ancestor_region(const ASTNode* ancestor, const ASTNode* node,
                         const std::unordered_map<const ASTNode*, const ASTNode*>& parent_of) {
    for (const ASTNode* cur = node; cur;) {
        auto it = parent_of.find(cur);
        cur = (it != parent_of.end()) ? it->second : nullptr;
        if (cur == ancestor) return true;
    }
    return false;
}

// Which repeated-name lexicals (global_decl_count_ > 1, normally refused by
// record_env_slot_info's guard) are still safe to slot-index: those whose
// declaring regions are ALL pairwise disjoint (siblings -- neither nested in
// the other), e.g. two separate top-level `for(let i...)` loops reusing "i".
// Deliberately skips re-checking escape-to-outside (references_outside):
// compilation is single-pass AST order, so one sibling's accesses always
// finish compiling (bytes already emitted) before the next sibling's
// record_env_slot_info call overwrites env_slot_info_[name] -- no
// retroactive corruption is possible. An escaping access sits at a
// different env_depth_ than any of these regions, so emit_read_local/
// emit_write_local's own (unchanged) depth check already falls back to
// LdaEnv/StaEnv there, same as any other guard miss. `roots`: one function
// body for compile(), every top-level statement for compile_script() (no
// single BlockStatement root there) -- scanned into shared maps so a name
// repeated ACROSS statements is still compared.
std::unordered_set<std::string> compute_sibling_safe_names(
        const std::vector<const ASTNode*>& roots) {
    std::unordered_map<std::string, std::vector<const ASTNode*>> regions;
    std::unordered_map<const ASTNode*, const ASTNode*> parent_of;
    for (const ASTNode* root : roots) collect_lexical_regions_multi(root, nullptr, regions, parent_of);
    std::unordered_set<std::string> safe;
    for (const auto& entry : regions) {
        const auto& list = entry.second;
        if (list.size() < 2) continue;
        bool all_disjoint = true;
        for (size_t i = 0; i < list.size() && all_disjoint; i++) {
            for (size_t j = i + 1; j < list.size(); j++) {
                if (list[i] == list[j] || is_ancestor_region(list[i], list[j], parent_of) ||
                    is_ancestor_region(list[j], list[i], parent_of)) {
                    all_disjoint = false;
                    break;
                }
            }
        }
        if (all_disjoint) safe.insert(entry.first);
    }
    return safe;
}

// Masks the active optional-chain collector// Masks the active optional-chain collector while compiling a subexpression
// that is NOT part of the chain's spine (computed keys, call arguments):
// a nested `a?.b` inside must short-circuit only itself, not the outer chain.
struct ChainMaskScope {
    std::vector<size_t>*& slot;
    std::vector<size_t>* saved;
    explicit ChainMaskScope(std::vector<size_t>*& s) : slot(s), saved(s) { slot = nullptr; }
    ~ChainMaskScope() { slot = saved; }
};

// True if `node`'s object/callee spine contains an optional-chaining link
// (a?.b, a?.b.c(), ...).
bool chain_contains_optional(const ASTNode* node) {
    if (!node) return false;
    switch (node->get_type()) {
        case ASTNode::Type::OPTIONAL_CHAINING_EXPRESSION:
            return true;
        case ASTNode::Type::MEMBER_EXPRESSION:
            return chain_contains_optional(static_cast<const MemberExpression*>(node)->get_object());
        case ASTNode::Type::CALL_EXPRESSION: {
            const auto* call = static_cast<const CallExpression*>(node);
            return call->is_optional() || chain_contains_optional(call->get_callee());
        }
        default:
            return false;
    }
}

}

bool BytecodeCompiler::expression_suspends(const ASTNode* node) {
    return contains_suspend(node);
}

// Annex B B.3.3: the var-scoped binding an if/label function declaration would
// create is not created at all when a lexical declaration or a parameter
// already owns the name. Dropping the entry here is what keeps the closure out
// of that binding's slot -- compile_if_branch reads the same answer back and
// binds the function in a scope of its own instead.
void drop_shadowed_annexb_fn_vars(std::vector<DeclInfo>& declared,
                                   const std::vector<std::string>& param_names,
                                   std::unordered_set<std::string>& kept,
                                   const std::unordered_set<std::string>& outer_lexicals = {}) {
    std::unordered_set<std::string> taken(param_names.begin(), param_names.end());
    taken.insert(outer_lexicals.begin(), outer_lexicals.end());
    for (const auto& info : declared) {
        if (info.is_lexical && !info.is_annexb_fn) taken.insert(info.name);
    }
    std::vector<DeclInfo> out;
    out.reserve(declared.size());
    for (auto& info : declared) {
        if (info.is_annexb_fn) {
            if (taken.count(info.name)) continue;
            kept.insert(info.name);
        }
        out.push_back(std::move(info));
    }
    declared = std::move(out);
}

std::unique_ptr<BytecodeChunk> BytecodeCompiler::compile(
    const ASTNode* body, const ParamList& params,
    bool suspendable, bool is_arrow, bool is_strict,
    const std::vector<std::string>* env_bound, bool outer_with, bool allow_arguments,
    const BodyScopeInfo* scope_info) {
    if (!body) return nullptr;
    // A concise arrow body is an expression, not a block: `() => e` is
    // `() => { return e; }` with the statement left implicit. Without this it
    // never compiled at all, so every call to one ran in the tree-walker.
    const bool concise = body->get_type() != ASTNode::Type::BLOCK_STATEMENT;
    if (params.size() > 64) return nullptr;

    // Default/destructured/rest parameters force env_mode: rest needs a
    // fresh (not run()-auto-bound) slot, and destructuring only knows how
    // to bind through a real Environment.
    bool has_complex_params = false;
    for (size_t pidx = 0; pidx < params.size(); pidx++) {
        if (params.has_default(pidx) || params.has_pattern(pidx) || params.is_rest(pidx)) {
            has_complex_params = true;
            break;
        }
    }
    // Rest is always the last parameter (grammar); everything before it
    // occupies a fixed position that run() auto-binds by index.
    bool has_rest = !params.empty() && params.is_rest(params.size() - 1);
    size_t fixed_param_count = has_rest ? params.size() - 1 : params.size();

    std::vector<std::string> param_names;  // excludes rest -- see CreateRestArray below
    std::string rest_name;
    bool arguments_is_param = false;
    for (size_t pidx = 0; pidx < params.size(); pidx++) {
        const std::string& pname = params.name(pidx);
        if (pname == "arguments") arguments_is_param = true;
        if (params.is_rest(pidx)) {
            rest_name = pname;
        } else {
            param_names.push_back(pname);
        }
    }
    if (has_complex_params) {
        // Duplicate simple parameter names are only valid for an all-simple
        // list (spec); reject up front instead of relying on declare_local's
        // own duplicate check, which exists for a different reason.
        std::unordered_set<std::string> seen;
        for (const auto& n : param_names) {
            if (!seen.insert(n).second) return nullptr;
        }
        if (has_rest && !seen.insert(rest_name).second) return nullptr;
    }

    std::vector<DeclInfo> declared;
    if (!prescan_declarations(body, declared)) return nullptr;
    std::unordered_set<std::string> annexb_fn_vars;
    drop_shadowed_annexb_fn_vars(declared, param_names, annexb_fn_vars);

    // `arguments` forces env_mode too: its mapped accessors (sloppy mode,
    // simple params) read/write the parameter bindings through the context,
    // which register-mode parameters don't have. A default/destructuring
    // expression can reference it too (spec: it exists during parameter
    // evaluation already).
    bool needs_arguments = uses_arguments(body);
    for (size_t pidx = 0; pidx < params.size(); pidx++) {
        if (params.has_default(pidx) && uses_arguments(params.default_value(pidx))) needs_arguments = true;
        if (params.has_pattern(pidx) && uses_arguments(params.pattern(pidx))) needs_arguments = true;
    }
    // Shadowing is covered too: a true duplicate name always has a nested
    // occurrence (a top-level dup is already a parser SyntaxError).
    // super/private-name access also forces env_mode: those forms delegate
    // to the tree-walker's own evaluate() and need `this`/`__super__`/brand
    // bindings and any captured locals resolvable through a real Environment.
    // Suspendable bodies always use env_mode: locals must survive across the
    // fiber suspension that delegated yield/await expressions perform.
    bool has_closures = contains_closure(body);
    bool has_nested_lex = !concise && contains_nested_lexical_decl(static_cast<const BlockStatement*>(body));
    // Bare destructuring assignments and complex object literals delegate to
    // the tree-walker, so they need env_mode like a suspendable's yield/await.
    bool has_delegated_expr = contains_delegated_expr(body);
    // Private access no longer forces env_mode: GetPrivate/SetPrivate resolve
    // brands through the CallStack, not the env chain. `super` still needs the
    // env (__super__/__home_object__ bindings); detect it from a whole-body
    // name sweep, whose opacity flags force full env_mode just like the
    // selective scan below. Delegated private forms that do need an env
    // (#x in obj, delete this.#x) were refused by the member guards and
    // fall back to the tree-walker.
    std::unordered_set<std::string> all_names;
    ScanOpacity an_op;
    bool an_super = false;
    collect_closure_names(body, /*inside_closure=*/true, all_names, an_op, suspendable, &an_super);
    // A direct eval can name `arguments` from inside its own source, which no
    // scan of this body can read, so mentioning eval is reason enough.
    if (an_op.saw_eval) needs_arguments = true;
    // A sloppy function keeps its top-level lexical declarations one scope in
    // from the variable environment, purely so a direct eval can tell that a
    // `var` it wants to introduce collides with one (spec FDI step 29). Only a
    // body with an eval needs the extra scope, and only where the entry
    // sequence is not already building one for the parameters.
    bool lex_scope_split = false;
    if (an_op.saw_eval && !is_strict) {
        for (const auto& info : declared) {
            if (info.is_lexical) { lex_scope_split = true; break; }
        }
    }

    // Eval inside a parameter initializer answers to EvalDeclarationInstantiation
    // rules the entry sequence does not set up: it reads the parameter names and
    // the arguments conflict off the calling context.
    bool param_eval = false;
    for (size_t pidx = 0; pidx < params.size(); pidx++) {
        const ASTNode* pe = params.has_default(pidx) ? params.default_value(pidx)
                          : params.has_pattern(pidx) ? params.pattern(pidx) : nullptr;
        if (!pe) continue;
        std::unordered_set<std::string> pn;
        ScanOpacity pe_op;
        collect_closure_names(pe, /*inside_closure=*/true, pn, pe_op);
        if (pe_op.saw_eval) param_eval = true;
    }
    if (param_eval) needs_arguments = true;
    // Whether a `var arguments` inside such an eval collides. A non-arrow always
    // has the implicit binding; an arrow only when a parameter carries the name.
    bool param_args_conflict = !is_arrow;
    if (!param_args_conflict) {
        for (size_t pidx = 0; pidx < params.size(); pidx++) {
            if (params.is_rest(pidx) || params.has_pattern(pidx)) continue;
            if (params.name(pidx) == "arguments") {
                param_args_conflict = true;
                break;
            }
        }
    }

    // FunctionDeclarationInstantiation decides whether the implicit object
    // exists at all: a parameter named `arguments`, or a lexical one over a
    // simple parameter list, takes its place. A `var arguments` does not --
    // the object is still created and the var names it rather than declaring a
    // second binding, so it has to exist even where nothing reads it.
    bool arguments_is_var = false;
    for (const auto& info : declared) {
        if (info.name != "arguments" || info.is_lexical) continue;
        arguments_is_var = true;
    }
    bool arguments_is_lexical = false;
    for (const auto& info : declared) {
        if (info.name == "arguments" && info.is_lexical) arguments_is_lexical = true;
    }
    if (arguments_is_param || (arguments_is_lexical && !has_complex_params)) {
        needs_arguments = false;
    } else if (arguments_is_var) {
        needs_arguments = true;
    }

    // Destructuring does not go here. It delegates to the tree-walker, so the
    // names it binds have to be Environment-resident -- but only those names.
    // Demanding a full environment for the whole function meant one pattern
    // anywhere in a body put every local in the environment and dropped every
    // call to the slow path; measured, that was the single largest reason
    // calls miss the register path. The selective scan below marks exactly the
    // bound names instead (see collect_closure_names' DESTRUCTURING_ASSIGNMENT
    // case, which already inserts them).
    const bool has_destructuring = contains_destructuring(body);
    // A `with` puts an object on the scope chain, so every name inside it has
    // to resolve by walking that chain -- no register, no slot index.
    // A direct eval reads and writes the caller's scope by name, so nothing
    // this function owns may sit in a register.
    bool full_env = has_complex_params || needs_arguments ||
                    an_op.opaque() || an_super || contains_with(body);

    // Selective env_mode: only names a closure (or a suspendable body's own
    // yield/await/return delegate) can observe, or that need a runtime
    // scope (nested lexicals, catch params, hoisted function names), stay
    // Environment-resident; every other local gets a register. Any
    // opacity -- eval inside a closure, class bodies, an AST form the
    // scanner doesn't know -- falls back to full env_mode.
    std::unordered_set<std::string> env_resident;
    // Hoisted out of the selective block: the declaration pass below needs it
    // too, to know that a repeat declaration is a disjoint sibling reusing a
    // register rather than a shadow it must refuse.
    std::unordered_set<std::string> sibling_safe;
    bool selective = false;
    if (!full_env && (has_closures || has_nested_lex || suspendable || has_delegated_expr ||
                      has_destructuring)) {
        ScanOpacity op;
        // A suspendable body's own yield/await is not a closure boundary for
        // this walk: the question is which names THIS body's machinery needs
        // out of the environment, and a register survives the fiber suspend
        // a yield/await performs exactly as intact as it survives for every
        // OTHER local the body holds (which already isn't forced env-
        // resident merely for living across a yield). Only a REAL nested
        // closure moves a name here, via the unconditional inside_closure
        // propagation into a nested FUNCTION_EXPRESSION/arrow body below --
        // unrelated to this flag. Harmless for a non-suspendable body: no
        // yield/await node exists there to read it.
        op.yield_transparent = true;
        // The parse works this set out while it reads the body and leaves it
        // on the unit, keyed by where the body opens. Reading it back is what
        // lets a body be compiled without walking it -- which a body kept only
        // as a range cannot be. The walk stays for a body the parse left no
        // record of: a synthesized one, or a top level that is not a function.
        // QUANTA_SCOPE_CHECK reports where the two disagree.
        const bool use_parse = scope_info != nullptr;
        if (use_parse) {
            env_resident = scope_info->captured;
            if (scope_info->eval_in_nested) op.saw_eval = true;
            if (scope_info->class_expression) op.saw_class = true;
        } else {
            collect_closure_names(body, /*inside_closure=*/false, env_resident,
                                  op, suspendable);
        }
        if (scope_info) {
#ifdef QUANTA_VALIDATE_BYTECODE
            if (std::getenv("QUANTA_SCOPE_CHECK")) {
                std::unordered_set<std::string> walked;
                ScanOpacity walked_op;
                collect_closure_names(body, /*inside_closure=*/false, walked,
                                      walked_op, suspendable);
                for (const auto& n : walked) {
                    if (!scope_info->captured.count(n)) {
                        std::fprintf(stderr, "[scope] eksik isim '%s' satir=%u\n",
                                     n.c_str(), body->get_start().line);
                    }
                }
                for (const auto& n : scope_info->captured) {
                    if (!walked.count(n)) {
                        std::fprintf(stderr, "[scope] fazla isim '%s' satir=%u\n",
                                     n.c_str(), body->get_start().line);
                    }
                }
            }
#endif
        }
        if (op.opaque()) {
            full_env = true;
        } else {
            if (!concise) {
                for (const auto& stmt : static_cast<const BlockStatement*>(body)->get_statements()) {
                    if (stmt->get_type() != ASTNode::Type::FUNCTION_DECLARATION) continue;
                    const auto* fd = static_cast<const FunctionDeclaration*>(stmt.get());
                    if (fd->get_id()) env_resident.insert(fd->get_id()->get_name());
                }
            }
            selective = true;
        }
    }

    if (selective) {
        // Catch params stay Environment-resident (the tree-walker's catch
        // machinery binds them by name). A nested/loop-header lexical only
        // needs the env when a closure can see it or the name shadows
        // another declaration -- otherwise it gets a register with a TDZ
        // re-arm at its block's entry (see BLOCK_STATEMENT).
        std::vector<BytecodeChunk::LoopEnvVar> direct_vars_pre;
        bool unused_pre = false;
        collect_direct_lexical_decls(body, direct_vars_pre, unused_pre);
        std::unordered_set<std::string> direct_pre;
        for (const auto& v : direct_vars_pre) direct_pre.insert(v.name);
        std::vector<DeclInfo> declared_pre;
        if (!prescan_declarations(body, declared_pre)) return nullptr;
        std::unordered_map<std::string, int> decl_count;
        for (const auto& info : declared_pre) decl_count[info.name]++;
        // See collect_lexical_regions/references_outside's doc comments.
        std::unordered_map<std::string, const ASTNode*> region_of;
        collect_lexical_regions(body, nullptr, region_of);
        // A name declared more than once is usually shadow-duplicated, and one
        // register cannot stand for two bindings that are live at the same
        // time. Two sibling loops reusing `i` are not that: their regions are
        // disjoint, so the register is only ever holding one of them. This is
        // the same disjointness record_env_slot_info already trusts to hand
        // out a slot index; here it decides whether a slot is needed at all.
        sibling_safe = compute_sibling_safe_names({body});
        std::unordered_map<std::string, std::vector<const ASTNode*>> regions_multi;
        std::unordered_map<const ASTNode*, const ASTNode*> parent_multi;
        if (!sibling_safe.empty()) {
            collect_lexical_regions_multi(body, nullptr, regions_multi, parent_multi);
        }
        for (const auto& info : declared_pre) {
            if (info.is_catch_param) {
                env_resident.insert(info.name);
            } else if (info.is_lexical && direct_pre.count(info.name) &&
                       decl_count[info.name] > 1 && !sibling_safe.count(info.name)) {
                // Declared directly here AND again in a region nested inside
                // it: two bindings, one name, both live. A register can hold
                // only one of them, and refusing on that basis handed the
                // whole function to the tree-walker over a single shadowed
                // loop variable. The environment chain already keeps repeated
                // declarations apart, so let this one live there.
                env_resident.insert(info.name);
            } else if (info.is_lexical && !direct_pre.count(info.name)) {
                if (decl_count[info.name] > 1 && !env_resident.count(info.name) &&
                    sibling_safe.count(info.name)) {
                    // Disjoint siblings, so each region may hold the register in
                    // turn -- but only if none of them lets the name outlive its
                    // own region, which is the same honesty check the
                    // singly-declared path below makes.
                    auto rit = regions_multi.find(info.name);
                    bool escapes = rit == regions_multi.end();
                    if (!escapes) {
                        std::unordered_set<const ASTNode*> own(rit->second.begin(), rit->second.end());
                        escapes = references_outside(body, own, info.name);
                    }
                    if (escapes) env_resident.insert(info.name);
                } else if (decl_count[info.name] > 1 || env_resident.count(info.name) > 0) {
                    env_resident.insert(info.name);  // shadow-duplicated or closure-captured
                } else {
                    auto it = region_of.find(info.name);
                    if (it == region_of.end() || references_outside(body, it->second, info.name)) {
                        env_resident.insert(info.name);  // escapes its own scope -- keep it honest
                    }
                }
            }
        }
        // Nothing ended up captured: the whole function is register-pure
        // (closures still pin the env for CreateClosure delegation; a
        // suspendable body or a delegated expr -- e.g. `[]=x;`, no targets --
        // keeps env_mode on regardless, see env_mode2).
        // A nested block's `let`/`const` may legally repeat a parameter's
        // name -- only one at the function's own top level is a SyntaxError.
        // Those are two bindings sharing a name, so the parameter cannot stay
        // in a register: a read inside that block would find the register
        // instead of the binding that shadows it. Keeping the name in the
        // environment lets the chain tell the two apart, which is what the
        // shadow-duplicated case above already relies on.
        for (const auto& info : declared_pre) {
            if (!info.is_lexical) continue;
            for (const auto& p : param_names) {
                if (p == info.name) { env_resident.insert(info.name); break; }
            }
        }
        // A const that is assigned somewhere cannot live in a register: the
        // refusal that assignment has to raise is carried by the environment
        // binding's mutable flag, and a register has nowhere to put it.
        for (const auto& info : declared_pre) {
            if (info.is_const && assigns_to_identifier(body, info.name)) {
                env_resident.insert(info.name);
            }
        }
        if (env_resident.empty() && !has_closures && !suspendable && !has_delegated_expr) {
            selective = false;
        }
    }
    // Suspendable bodies, delegated expressions and nested closures all keep
    // env_mode on: Op::CreateClosure and
    // Op::DeclareFunction each refuse to emit without it.
    // An assigned const cannot live in a register: the refusal the assignment
    // owes is carried by the environment binding's mutable flag, and a register
    // has nowhere to put it. Wanting one is itself a reason to have an
    // environment, so this is settled before env_mode is.
    for (const auto& info : declared) {
        if (info.is_const && assigns_to_identifier(body, info.name)) {
            env_resident.insert(info.name);
        }
    }
    // Names the caller already bound in the environment (a suspendable body's
    // parameters) must resolve there: a register standing for one would be a
    // second, empty storage, since this chunk declares no parameters of its own.
    std::unordered_set<std::string> env_bound_set;
    if (env_bound) {
        for (const auto& n : *env_bound) {
            if (n.empty()) continue;
            env_bound_set.insert(n);
            // Under full_env every name already resolves through the
            // environment; forcing the selective set here would turn that off
            // and leave everything NOT in this list holding a register, where
            // a closure over it can no longer find it.
            if (!full_env) {
                env_resident.insert(n);
                selective = true;
            }
        }
    }

    // The register file is kMaxRegisters wide and every parameter and top-level
    // declaration wants one of them. A bundled script's outer wrapper declares
    // more names than that, and declare_local's refusal then handed the whole
    // function -- along with everything nested inside it -- to the tree-walker.
    // The environment has no such ceiling, so the names past the budget go
    // there instead: slower per access than a register, but the alternative is
    // not compiling at all. The reserve leaves room for the temporaries
    // expression evaluation allocates on top of the named locals.
    if (!full_env) {
        constexpr size_t kTempReserve = 48;
        std::unordered_set<std::string> counted;
        size_t used = param_names.size() + kTempReserve;
        for (const auto& info : declared) {
            if (env_resident.count(info.name) || !counted.insert(info.name).second) continue;
            if (used < static_cast<size_t>(kMaxRegisters)) {
                used++;
                continue;
            }
            env_resident.insert(info.name);
            selective = true;
        }
    }
    // A register carries no mutability, so a const that is ever assigned has to
    // live in the Environment, where the binding's own flag makes the store
    // throw. Refusing instead handed the whole body to the tree-walker.
    for (const auto& info : declared) {
        if (!info.is_const || env_resident.count(info.name)) continue;
        if (!assigns_to_identifier(body, info.name)) continue;
        env_resident.insert(info.name);
        selective = true;
    }
    const bool env_mode = full_env || has_closures || !env_resident.empty() ||
                          suspendable || has_delegated_expr;
    BytecodeCompiler compiler(param_names, env_mode, selective ? &env_resident : nullptr);
    compiler.outer_with_ = outer_with;
    compiler.annexb_fn_vars_ = std::move(annexb_fn_vars);
    // Every lexically declared name, known before a single instruction is
    // emitted. Learning it while compiling made the answer depend on source
    // order: a reference written ABOVE the block that declares the name saw
    // nothing block-scoped yet and read the register anyway.
    for (const auto& info : declared) {
        if (info.is_lexical || info.is_catch_param) {
            compiler.block_scoped_names_.insert(info.name);
        }
    }
    if (compiler.failed_) return nullptr;
    compiler.allow_arguments_ = needs_arguments || allow_arguments;
    compiler.eval_in_body_ = an_op.saw_eval;
    compiler.suspendable_ = suspendable;
    if (has_rest) {
        if (!env_mode || !compiler.env_names_.insert(rest_name).second) return nullptr;
    }

    // Slot-index eligibility bookkeeping for Op::LdaEnvSlot/StaEnvSlot/
    // StaEnvSlotInit (BytecodeCompiler.h's EnvSlotInfo doc comment has the
    // full rationale). global_decl_count_ covers every name that could ever
    // become an Environment binding in this function -- body declarations,
    // params, rest -- so uniqueness is checked function-wide, matching the
    // fact that Environment::find_binding_env's chain walk (the path being
    // bypassed) is itself function-wide, not per-scope.
    if (env_mode) {
        for (const auto& info : declared) compiler.global_decl_count_[info.name]++;
        for (const auto& p : param_names) compiler.global_decl_count_[p]++;
        if (has_rest) compiler.global_decl_count_[rest_name]++;
        compiler.sibling_safe_names_ = compute_sibling_safe_names({body});
    }
    // flat_slot_counter mirrors the actual runtime seeding order of the
    // function's own (depth-0) Environment: params first (in param_names
    // order -- seeded either by VM::run from chunk.env_params in full-env
    // mode, or by this function's own selective-mode StaEnvInit loop below,
    // both in the same order), then env_locals (in push order, right
    // below), then a rest slot if present.
    // Mirrors Environment::SlotMap::kMaxReservedSlots, which is what decides
    // how many of these indices an environment actually sizes itself for. The
    // two are checked independently at runtime -- an index the environment did
    // not reserve simply fails its guard and falls back to the name path -- so
    // they bound each other rather than having to agree.
    // A closure made while the parameters run captures the environment they
    // bind in, and the spec keeps that one apart from the body's. Op::BindEnvLocals
    // opens a child scope for the body then, so the closure never sees a name
    // the body declares. Only this shape pays for it.
    bool split_param_scope = false;
    bool param_expressions = false;
    for (size_t pidx = 0; pidx < params.size(); pidx++) {
        if (params.is_rest(pidx)) continue;
        if (params.has_default(pidx) || params.has_pattern(pidx)) param_expressions = true;
        if ((params.has_default(pidx) && contains_closure(params.default_value(pidx))) ||
            (params.has_pattern(pidx) && contains_closure(params.pattern(pidx)))) {
            split_param_scope = true;
        }
    }
    // A lexical `arguments` over a parameter list with expressions does not
    // replace the implicit object -- both exist, one per scope -- so it needs
    // the split for the same reason.
    if (arguments_is_lexical && param_expressions) split_param_scope = true;

    constexpr size_t kEnvSlotPredictMax = 32;
    size_t flat_slot_counter = 0;
    if (env_mode) {
        // A suspendable body's parameters are bound by the caller, into the
        // very environment this chunk runs in, before anything here binds
        // anything -- so they hold the first slots and the body's own names
        // start after them. Counted here rather than assumed away: leaving
        // the counter at zero predicted the body's first local onto the slot
        // a parameter already held, which the read then rejected by name, so
        // a generator with parameters lost the slot path for its own locals
        // as well as for them. A generator that also binds `arguments` or a
        // self-reference puts those in between; the prediction misses and the
        // read takes the name path, which is what it does today regardless.
        if (env_bound) {
            for (const auto& n : *env_bound) {
                if (n.empty()) continue;
                // Named here as environment-resident so a read reaches for the
                // slot at all: the chunk declares no parameters of its own, so
                // without this the name is not one this compiler knows and
                // every read of it walked the scope chain by name.
                compiler.env_names_.insert(n);
                if (flat_slot_counter < kEnvSlotPredictMax) {
                    auto cit = compiler.global_decl_count_.find(n);
                    if (cit == compiler.global_decl_count_.end() || cit->second <= 1) {
                        compiler.env_slot_info_[n] = {static_cast<uint8_t>(flat_slot_counter), 0};
                    }
                }
                flat_slot_counter++;
            }
        }
        for (const auto& p : param_names) {
            bool resident = !selective || env_resident.count(p) > 0;
            if (!resident) continue;
            if (flat_slot_counter < kEnvSlotPredictMax) {
                auto cit = compiler.global_decl_count_.find(p);
                if (cit != compiler.global_decl_count_.end() && cit->second == 1) {
                    compiler.env_slot_info_[p] = {static_cast<uint8_t>(flat_slot_counter), 0};
                }
            }
            flat_slot_counter++;
        }
    }

    // Nested lexical names and catch parameters get their own environment
    // elsewhere -- only direct top-level names get a function-entry binding.
    std::unordered_set<std::string> direct_lexical_names;
    if (env_mode) {
        std::vector<BytecodeChunk::LoopEnvVar> direct_vars;
        bool unused_needs_env = false;
        collect_direct_lexical_decls(body, direct_vars, unused_needs_env);
        for (const auto& v : direct_vars) direct_lexical_names.insert(v.name);
    }

    for (const auto& info : declared) {
        if (info.name == "arguments") {
            // The var form names the object rather than introducing a binding
            // -- except where the scopes are split, since then the body has one
            // of its own, seeded from the parameter scope's.
            if (!info.is_lexical && !split_param_scope) continue;
            // A lexical one only replaces the object when the parameter list is
            // simple; otherwise both exist, one per scope, which needs the two
            // Op::BindEnvLocals opens.
            if (needs_arguments && !split_param_scope) return nullptr;
        }
        bool aliases_param = false;
        bool nested_lexical_shadows_param = false;
        for (const auto& p : param_names) {
            if (p != info.name) continue;
            // `var x` where x is already a parameter does not introduce a
            // second binding: FunctionDeclarationInstantiation reuses the
            // parameter's, which keeps the argument's value rather than being
            // re-initialized to undefined. So there is nothing to declare, and
            // references resolve to the parameter's register on their own.
            //
            // A lexical of the same name is NOT the SyntaxError it looks
            // like: only one at the function's own top level is, and the
            // parser already rejects that. This is a nested block's own
            // binding, declared by the block that owns it, so there is
            // nothing to declare at function level. It needs the name to
            // live in an environment, where the chain keeps the two bindings
            // apart -- a register cannot hold both.
            if (info.is_lexical) {
                if (!env_mode || !(full_env || env_resident.count(info.name))) return nullptr;
                nested_lexical_shadows_param = true;
                break;
            }
            // With the scopes split the body's is a second binding, seeded
            // from the parameter's value (spec FDI) -- not the same one.
            aliases_param = !split_param_scope;
            break;
        }
        if (nested_lexical_shadows_param || aliases_param) continue;
        if (has_rest && rest_name == info.name) return nullptr;
        bool resident = env_mode && (!selective || env_resident.count(info.name) > 0);
        // An environment-resident const carries its own immutability (the
        // binding's mutable flag, which the store opcode checks); a register
        // cannot, so an assigned const that did not become resident still
        // refuses rather than compile as mutable.
        // Made resident above precisely so this cannot happen.
        if (info.is_const && !resident && assigns_to_identifier(body, info.name)) return nullptr;
        if (resident) {
            // A repeat declare_local (shadowed name) is fine -- the Environment
            // chain resolves each occurrence to its own scope at runtime.
            compiler.declare_local(info.name);
            if (info.is_const) compiler.const_locals_.insert(info.name);
            // A name the caller already bound is not declared again here: the
            // entry sequence would create a second binding, holding undefined,
            // in front of the one that carries the argument.
            const bool caller_bound = !info.is_lexical && env_bound_set.count(info.name) > 0;
            if (!caller_bound && !info.is_catch_param &&
                (!info.is_lexical || direct_lexical_names.count(info.name))) {
                compiler.chunk_->ensure_env().env_locals.push_back({info.name, info.is_lexical, info.is_const});
                // A predicted slot names a position in the environment the code
                // runs in; with the scopes split the body's is a different one.
                if (!split_param_scope && !(lex_scope_split && info.is_lexical) &&
                    flat_slot_counter < kEnvSlotPredictMax) {
                    auto cit = compiler.global_decl_count_.find(info.name);
                    if (cit != compiler.global_decl_count_.end() && cit->second == 1) {
                        compiler.env_slot_info_[info.name] = {static_cast<uint8_t>(flat_slot_counter), 0};
                    }
                }
                flat_slot_counter++;
            }
        } else {
            if (!compiler.declare_local(info.name)) {
                // A repeated `var` is not a second binding at all: var is
                // function-scoped and hoisted, so both declarations name the
                // one variable and the register already standing for it is the
                // right one. Two counted loops in a row, each opening with
                // `var i`, is the ordinary way to write that, and refusing on
                // it handed the whole function to the tree-walker.
                //
                // Otherwise: disjoint siblings share one register on purpose,
                // the second declaration finding the first one's and reusing
                // it, which is the whole point of proving the regions never
                // overlap. Any other repeat is a real shadow, and refusing to
                // compile is right.
                const bool same_var = !info.is_lexical && compiler.lookup_local(info.name) >= 0;
                if (!same_var &&
                    (!sibling_safe.count(info.name) || compiler.lookup_local(info.name) < 0)) {
                    return nullptr;
                }
            }
            if (info.is_const) compiler.const_locals_.insert(info.name);
            if (info.is_lexical) {
                compiler.lexical_registers_.insert(compiler.lookup_local(info.name));
            }
        }
    }
    // A destructuring parameter has no name of its own, so its bound names are
    // absent from param_names (which is positional). Declaring them here is
    // what lets emit_pattern_bind write them. They are deliberately NOT pushed
    // into env_locals: BindEnvLocals runs after the parameter patterns have
    // bound and would reset them to undefined; StaEnvInit creates the binding.
    if (env_mode) {
        for (size_t pidx = 0; pidx < params.size(); pidx++) {
            if (!params.has_pattern(pidx)) continue;
            const ASTNode* pat = params.pattern(pidx);
            if (!pat || pat->get_type() != ASTNode::Type::DESTRUCTURING_ASSIGNMENT) continue;
            std::vector<std::string> bound;
            static_cast<const DestructuringAssignment*>(pat)->collect_bound_names(bound);
            for (const auto& bn : bound) compiler.declare_local(bn);
        }
    }

    compiler.temp_watermark_ = compiler.next_register_;

    if (!env_mode || selective) {
        // Every let/const register starts in TDZ; the declaring statement's
        // own Star lifts it later. (Env-resident names have no register.)
        for (const auto& info : declared) {
            if (!info.is_lexical) continue;
            int reg = compiler.lookup_local(info.name);
            if (reg < 0 || compiler.env_names_.count(info.name)) continue;
            compiler.emit(Op::LdaTdz);
            compiler.emit(Op::Star);
            compiler.emit_u8(static_cast<uint8_t>(reg));
        }
    }
    if (selective) {
        // Captured params: registers hold the raw arguments (run() fills
        // them); seed the env binding every read/write will resolve to.
        for (size_t i = 0; i < param_names.size(); i++) {
            if (!env_resident.count(param_names[i])) continue;
            compiler.emit(Op::Ldar);
            compiler.emit_u8(static_cast<uint8_t>(i));
            compiler.emit_write_local(param_names[i], /*is_declaration=*/true);
        }
    }
    // Non-rest parameters' function-entry bindings are data-driven from the
    // chunk (env_params), set up once by VM::run -- no bytecode needed for
    // those. Rest gets its own immediately-initialized slot here instead,
    // since CreateRestArray below fills it, not run().
    if (has_rest) {
        compiler.chunk_->ensure_env().env_locals.push_back({rest_name, false, false});
        if (flat_slot_counter < kEnvSlotPredictMax) {
            auto cit = compiler.global_decl_count_.find(rest_name);
            if (cit != compiler.global_decl_count_.end() && cit->second == 1) {
                compiler.env_slot_info_[rest_name] = {static_cast<uint8_t>(flat_slot_counter), 0};
            }
        }
        flat_slot_counter++;
    }

    // Parameter lists with initializers follow spec FDI ordering (see
    // BytecodeChunk::env_params_tdz): params seed uninitialized, and each
    // one initializes left to right from its register-held raw argument.
    bool params_tdz = false;
    for (size_t pidx = 0; pidx < params.size(); pidx++) {
        if (params.is_rest(pidx)) continue;
        if (params.has_default(pidx) || params.has_pattern(pidx)) params_tdz = true;
    }
    // Op::BindEnvLocals already decides where the body's bindings go, and it
    // makes one scope for all of them; the lexicals-only split is the entry
    // path's, so the two do not combine.
    if (lex_scope_split && params_tdz) return nullptr;

    // Defaults and destructuring resolve once at entry, before the body --
    // in TDZ mode the raw values come from registers (the env binding is
    // still uninitialized); otherwise run() already bound plain parameters.
    //
    // Registers 0..fixed_param_count-1 hold those raw arguments until each
    // parameter has read its own, so a pattern's temps must not be handed one
    // a later parameter has yet to consume. In full env_mode next_register_
    // starts at 0 (locals live in the environment), which is exactly where
    // those arguments sit.
    int saved_next_register = compiler.next_register_;
    if (params_tdz && compiler.next_register_ < static_cast<int>(fixed_param_count)) {
        compiler.next_register_ = static_cast<int>(fixed_param_count);
        if (compiler.next_register_ > compiler.temp_watermark_) {
            compiler.temp_watermark_ = compiler.next_register_;
        }
    }
    if (param_eval) {
        compiler.emit(Op::EnterParamEval);
        compiler.emit_u8(static_cast<uint8_t>(1 | (param_args_conflict ? 2 : 0)));
    }
    {
        uint8_t param_index = 0;
        for (size_t pidx = 0; pidx < params.size(); pidx++) {
            if (params.is_rest(pidx)) continue;  // handled below, after BindEnvLocals
            if (!params_tdz && !params.has_default(pidx) && !params.has_pattern(pidx)) {
                param_index++;
                continue;
            }
            const std::string& pname = params.name(pidx);
            if (params_tdz) {
                compiler.emit(Op::Ldar);
                compiler.emit_u8(param_index);
            } else {
                compiler.emit_read_local(pname);
            }
            if (params.has_default(pidx)) {
                // A class default has to be named before its static
                // initializers run, which Op::SetFunctionNameIfUnnamed is too
                // late for -- same reason the assignment forms delegate.
                stamp_inferred_class_name(params.default_value(pidx), pname);
                size_t skip = compiler.emit_jump(Op::JumpIfNotUndefined);
                if (!compiler.compile_expression(params.default_value(pidx))) return nullptr;
                if (!params.has_pattern(pidx) && is_named_evaluation_rhs(params.default_value(pidx))) {
                    compiler.emit(Op::SetFunctionNameIfUnnamed);
                    compiler.emit_u16(compiler.add_name(pname));
                }
                if (!compiler.patch_jump(skip)) return nullptr;
            }
            if (params.has_pattern(pidx)) {
                auto* destr = const_cast<DestructuringAssignment*>(
                    static_cast<const DestructuringAssignment*>(params.pattern(pidx)));
                const ASTNode* lit = destr->get_pattern_literal();
                if (!compiler.pattern_is_emittable(lit, true)) return nullptr;
                if (!compiler.emit_pattern_bind(lit, true, false)) return nullptr;
            } else if (params_tdz) {
                compiler.emit_write_local(pname, /*is_declaration=*/true);
            } else {
                compiler.emit_write_local(pname, false);
            }
            param_index++;
        }
    }
    // The rest parameter is bound with the rest of the list, before the body's
    // bindings exist: a `var` a direct eval in its initializer declares belongs
    // among the parameters, which is where the list's own closures look.
    if (has_rest) {
        const size_t last = params.size() - 1;
        compiler.emit(Op::CreateRestArray);
        compiler.emit_u8(static_cast<uint8_t>(fixed_param_count));
        if (params.has_pattern(last)) {
            auto* destr = const_cast<DestructuringAssignment*>(
                static_cast<const DestructuringAssignment*>(params.pattern(last)));
            const ASTNode* lit = destr->get_pattern_literal();
            if (!compiler.pattern_is_emittable(lit, true)) return nullptr;
            if (!compiler.emit_pattern_bind(lit, true, false)) return nullptr;
        } else {
            // Declaring, not assigning: the rest parameter's binding is created
            // here, like every other parameter's. Assigning instead reached for
            // a binding nothing had made, which only surfaced once some other
            // parameter had a default -- that is what puts the list in an
            // Environment, where the difference between creating a slot and
            // writing one is observable.
            compiler.emit_write_local(rest_name, /*is_declaration=*/true);
        }
    }
    if (params_tdz) {
        compiler.emit(Op::BindEnvLocals);
        compiler.emit_u8(split_param_scope ? 1 : 0);
        if (split_param_scope) compiler.env_depth_++;
    }
    // Every raw argument has been read by now, so those registers are free
    // again for the body's temps.
    compiler.next_register_ = saved_next_register;
    if (param_eval) {
        compiler.emit(Op::EnterParamEval);
        compiler.emit_u8(0);
    }

    if (concise) {
        if (!compiler.compile_expression(body)) return nullptr;
        compiler.emit(Op::Return);
        compiler.chunk_->register_count = static_cast<uint16_t>(compiler.temp_watermark_);
        compiler.chunk_->parameter_count = static_cast<uint8_t>(param_names.size());
        compiler.chunk_->env_mode = env_mode;
        compiler.chunk_->env_params_tdz = params_tdz;
        compiler.chunk_->lex_scope_split = lex_scope_split;
        compiler.chunk_->needs_arguments = needs_arguments;
        if (env_mode && !selective) compiler.chunk_->ensure_env().env_params = param_names;
        if (env_mode) compiler.chunk_->ensure_env().env_slot_total = static_cast<uint16_t>(flat_slot_counter);
        if (compiler.chunk_->uses_lookup_cache) {
            compiler.chunk_->lookup_cache = FixedArray<BytecodeChunk::LookupCacheEntry>::filled(
                static_cast<uint32_t>(compiler.names_.size()), BytecodeChunk::LookupCacheEntry{});
        }
        compiler.fuse_store_pairs();
        compiler.chunk_->code = FixedArray<uint8_t>::from(std::move(compiler.code_));
        compiler.chunk_->constants = FixedArray<Value>::from(std::move(compiler.constants_));
        compiler.chunk_->names = intern_name_pool(std::move(compiler.names_));
        compiler.chunk_->feedback = FixedArray<FeedbackSlot>::from(std::move(compiler.feedback_));
#ifdef QUANTA_VALIDATE_BYTECODE
            if (compiler.chunk_) validate_chunk_registers(*compiler.chunk_, std::string());
#endif
        return compiler.failed_ ? nullptr : std::move(compiler.chunk_);
    }

    const auto* block = static_cast<const BlockStatement*>(body);

    // Hoist top-level function declarations (incl. generator/async forms):
    // FunctionDeclaration::evaluate creates the function AND binds its name,
    // so a plain delegation before the body gives `g(); function g() {}` the
    // right order. Block-nested declarations (Annex B territory) still bail
    // in compile_statement.
    for (const auto& stmt : block->get_statements()) {
        if (stmt->get_type() != ASTNode::Type::FUNCTION_DECLARATION) continue;
        if (!env_mode) return nullptr;
        if (compiler.chunk_->ensure_closures().size() >= 0xFFFF) return nullptr;
        compiler.hoisted_fn_decls_.insert(stmt.get());
        compiler.chunk_->ensure_closures().push_back(closure_template_for(stmt.get()));
        compiler.emit(Op::DeclareFunction);
        compiler.emit_u16(static_cast<uint16_t>(compiler.chunk_->ensure_closures().size() - 1));
    }

    // A `using` at the top of the body disposes when the body ends, exactly as
    // one inside a block does; the body is that block's scope here.
    // The body is a scope too: its own let/const are visible for the whole of
    // it, unlike the ones a nested block owns.
    compiler.lexical_scopes_.emplace_back();
    {
        std::vector<BytecodeChunk::LoopEnvVar> body_lexicals;
        bool unused_own_env = false;
        if (collect_direct_lexical_decls(block, body_lexicals, unused_own_env)) {
            for (const auto& v : body_lexicals) compiler.lexical_scopes_.back().push_back(v.name);
        }
        for (const auto& pn : param_names) compiler.lexical_scopes_.back().push_back(pn);
    }
    bool body_has_using = false;
    for (const auto& stmt : block->get_statements()) {
        if (stmt->get_type() == ASTNode::Type::USING_DECLARATION) { body_has_using = true; break; }
    }
    FinallyScope body_escaped;
    if (body_has_using) {
        const bool ok = compiler.emit_dispose_scope_body(block, [&]() -> bool {
            for (const auto& stmt : block->get_statements()) {
                if (!compiler.compile_statement(stmt.get())) return false;
            }
            return true;
        }, body_escaped);
        if (!ok) return nullptr;
    } else {
        for (const auto& stmt : block->get_statements()) {
            if (!compiler.compile_statement(stmt.get())) return nullptr;
        }
    }

    // Falling off the end returns undefined.
    compiler.emit(Op::LdaUndefined);
    compiler.emit(Op::Return);
    if (body_has_using && !compiler.emit_finally_pads(body_escaped)) return nullptr;

    compiler.chunk_->register_count = static_cast<uint16_t>(compiler.temp_watermark_);
    compiler.chunk_->parameter_count = static_cast<uint8_t>(param_names.size());
    compiler.chunk_->env_mode = env_mode;
    compiler.chunk_->env_params_tdz = params_tdz;
    compiler.chunk_->lex_scope_split = lex_scope_split;
    compiler.chunk_->needs_arguments = needs_arguments;
    if (env_mode && !selective) compiler.chunk_->ensure_env().env_params = param_names;
    if (env_mode) compiler.chunk_->ensure_env().env_slot_total = static_cast<uint16_t>(flat_slot_counter);
    if (compiler.chunk_->uses_lookup_cache) {
        compiler.chunk_->lookup_cache = FixedArray<BytecodeChunk::LookupCacheEntry>::filled(
            static_cast<uint32_t>(compiler.names_.size()), BytecodeChunk::LookupCacheEntry{});
    }
    compiler.fuse_store_pairs();
    compiler.chunk_->code = FixedArray<uint8_t>::from(std::move(compiler.code_));
    compiler.chunk_->constants = FixedArray<Value>::from(std::move(compiler.constants_));
    compiler.chunk_->names = intern_name_pool(std::move(compiler.names_));
    compiler.chunk_->feedback = FixedArray<FeedbackSlot>::from(std::move(compiler.feedback_));
#ifdef QUANTA_VALIDATE_BYTECODE
        if (compiler.chunk_) validate_chunk_registers(*compiler.chunk_, std::string());
#endif
    return std::move(compiler.chunk_);
}

bool BytecodeCompiler::module_body_suspends(
    const std::vector<std::unique_ptr<ASTNode>>& statements) {
    for (const auto& st : statements) {
        if (contains_suspend(st.get())) return true;
    }
    return false;
}

std::unique_ptr<BytecodeChunk> BytecodeCompiler::compile_module_body(
    const std::vector<std::unique_ptr<ASTNode>>& statements, bool outer_with) {
    if (!module_body_suspends(statements)) return nullptr;
    return compile_script(statements, outer_with, /*track_completion=*/false,
                          /*suspendable=*/true);
}

std::unique_ptr<BytecodeChunk> BytecodeCompiler::compile_script(
    const std::vector<std::unique_ptr<ASTNode>>& statements, bool outer_with,
    bool track_completion, bool suspendable) {
    // Scan the whole program once. Modules and anything opaque to the
    // scanners tree-walk; class definitions and closure-side eval only
    // disable the nested-register refinement (top-level names are outer
    // bindings either way, so correctness doesn't depend on the scan).
    std::unordered_set<std::string> closure_names;
    ScanOpacity op;
    for (const auto& st : statements) {

        collect_closure_names(st.get(), /*inside_closure=*/false, closure_names,
                              op);
    }
    // A closure's eval text can name anything, which is why it only turns the
    // nested-register refinement off: every top-level name is an outer binding
    // reached by name either way, so what eval can reach does not change.
    bool refine = !op.saw_class && !op.unknown && !op.saw_eval;

    // Top-level names are pre-hoisted outer bindings (vars on the global,
    // let/const/class in the script env, function declarations already
    // evaluated) -- the compiler must treat them as non-locals.
    std::unordered_set<std::string> top_names;
    // The subset that is lexical: Annex B skips its var binding for those.
    std::unordered_set<std::string> top_lexicals;
    for (const auto& st : statements) {
        const ASTNode* eff = st.get();
        // `export let x = 1` declares x at this level; the export wrapper is
        // only the record kept about it (same unwrapping the lexical hoist
        // does before this chunk runs).
        if (eff->get_type() == ASTNode::Type::EXPORT_STATEMENT) {
            const auto* ex = static_cast<const ExportStatement*>(eff);
            eff = ex->is_declaration_export() ? ex->get_declaration() : nullptr;
            if (!eff) continue;
        }
        if (eff->get_type() == ASTNode::Type::VARIABLE_DECLARATION) {
            const auto* vd = static_cast<const VariableDeclaration*>(eff);
            const bool lexical = vd->get_kind() != VariableDeclarator::Kind::VAR;
            for (const auto& d : vd->get_declarations()) {
                if (!d->get_id()) continue;
                top_names.insert(d->get_id()->get_name());
                if (lexical) top_lexicals.insert(d->get_id()->get_name());
            }
        } else if (eff->get_type() == ASTNode::Type::CLASS_DECLARATION) {
            const auto* cd = static_cast<const ClassDeclaration*>(eff);
            if (cd->get_id()) { top_names.insert(cd->get_id()->get_name());
                                top_lexicals.insert(cd->get_id()->get_name()); }
        } else if (eff->get_type() == ASTNode::Type::FUNCTION_DECLARATION) {
            const auto* fd = static_cast<const FunctionDeclaration*>(eff);
            if (fd->get_id()) top_names.insert(fd->get_id()->get_name());
        }
    }

    // Nested declarations follow the same selective rules as function
    // bodies: captured/shadowed/catch names stay env-resident, the rest get
    // registers (top-level `var`s inside blocks/loops are global writes --
    // they stay non-local too). Only statements that are NOT themselves a
    // top-level declaration get scanned: a nested declarator can share a
    // name with a top-level one (shadowing, e.g. `let x` at script scope and
    // `for (let x ...)`), and that nested binding needs its own local/env
    // slot -- name-matching it against top_names would wrongly treat it as
    // already handled by hoisting and never assign it a home at all (see
    // scope-lex-const.js / let-outer-inner-let-bindings.js).
    std::vector<DeclInfo> declared;
    for (const auto& st : statements) {
        auto t = st->get_type();
        if (t == ASTNode::Type::VARIABLE_DECLARATION || t == ASTNode::Type::CLASS_DECLARATION ||
            t == ASTNode::Type::FUNCTION_DECLARATION) {
            continue;  // top-level decl itself: already hoisted, no nested content
        }
        if (!prescan_declarations(st.get(), declared)) return nullptr;
    }
    std::unordered_set<std::string> annexb_fn_vars;
    drop_shadowed_annexb_fn_vars(declared, {}, annexb_fn_vars, top_lexicals);
    std::unordered_map<std::string, int> decl_count;
    for (const auto& info : declared) decl_count[info.name]++;
    // Same region-escape analysis as compile()'s selective env_mode scan
    // (see collect_lexical_regions/references_outside's doc comments) --
    // applied across every top-level statement, since a script has no
    // single BlockStatement root to scan from.
    std::unordered_map<std::string, const ASTNode*> region_of;
    for (const auto& st : statements) collect_lexical_regions(st.get(), nullptr, region_of);
    auto escapes_script = [&](const std::unordered_set<const ASTNode*>& regions,
                              const std::string& name) {
        for (const auto& st : statements) {
            if (references_outside(st.get(), regions, name)) return true;
        }
        return false;
    };
    // Same disjoint-sibling reasoning as compile()'s: a name declared twice is
    // usually shadowing, which one register cannot represent, but two sibling
    // loops reusing `i` never hold it at the same time.
    std::vector<const ASTNode*> sib_roots;
    for (const auto& st : statements) sib_roots.push_back(st.get());
    std::unordered_set<std::string> sibling_safe = compute_sibling_safe_names(sib_roots);
    std::unordered_map<std::string, std::vector<const ASTNode*>> regions_multi;
    std::unordered_map<const ASTNode*, const ASTNode*> parent_multi;
    if (!sibling_safe.empty()) {
        for (const ASTNode* r : sib_roots) {
            collect_lexical_regions_multi(r, nullptr, regions_multi, parent_multi);
        }
    }
    std::unordered_set<std::string> env_resident;
    for (const auto& info : declared) {
        if (!info.is_lexical && !info.is_catch_param) continue;  // nested var: global
        bool sibling_ok = refine && !info.is_catch_param && !closure_names.count(info.name) &&
                          !top_names.count(info.name) && decl_count[info.name] > 1 &&
                          sibling_safe.count(info.name);
        if (sibling_ok) {
            auto rit = regions_multi.find(info.name);
            if (rit == regions_multi.end()) {
                env_resident.insert(info.name);
            } else {
                std::unordered_set<const ASTNode*> own(rit->second.begin(), rit->second.end());
                if (escapes_script(own, info.name)) env_resident.insert(info.name);
            }
        } else if (info.is_catch_param || !refine || closure_names.count(info.name) ||
            decl_count[info.name] > 1 || top_names.count(info.name)) {
            env_resident.insert(info.name);
        } else {
            auto it = region_of.find(info.name);
            std::unordered_set<const ASTNode*> one{it == region_of.end() ? nullptr : it->second};
            if (it == region_of.end() || escapes_script(one, info.name)) {
                env_resident.insert(info.name);
            }
        }
    }

    // A `with` anywhere puts an object on the chain, and then no name can live
    // in a register -- the same reason compile() forces full env mode for one.
    bool script_has_with = false;
    for (const auto& st : statements) {
        if (contains_with(st.get())) { script_has_with = true; break; }
    }
    // Same register budget compile() keeps: past it a name lives in the
    // Environment instead, which is slower per access but is the difference
    // between compiling the script and not compiling it at all.
    {
        constexpr size_t kTempReserve = 48;
        std::unordered_set<std::string> counted;
        size_t used = kTempReserve;
        for (const auto& info : declared) {
            if (env_resident.count(info.name) || !counted.insert(info.name).second) continue;
            if (used < static_cast<size_t>(kMaxRegisters)) { used++; continue; }
            env_resident.insert(info.name);
        }
    }
    // Same reason as compile(): an assigned const needs the Environment's
    // mutability flag, so it becomes resident rather than refusing the chunk.
    for (const auto& info : declared) {
        if (!info.is_const || env_resident.count(info.name)) continue;
        for (const auto& st : statements) {
            if (assigns_to_identifier(st.get(), info.name)) { env_resident.insert(info.name); break; }
        }
    }
    BytecodeCompiler compiler({}, /*env_mode=*/true,
                              script_has_with ? nullptr : &env_resident);
    if (compiler.failed_) return nullptr;
    compiler.suspendable_ = suspendable;
    // Every lexically declared name, known before a single instruction is
    // emitted. Learning it while compiling made the answer depend on source
    // order: a reference written ABOVE the block that declares the name saw
    // nothing block-scoped yet and read the register anyway.
    for (const auto& info : declared) {
        if (info.is_lexical || info.is_catch_param) {
            compiler.block_scoped_names_.insert(info.name);
        }
    }
    compiler.script_mode_ = true;
    // No implicit `arguments` object exists up here, so the name is an
    // ordinary chain lookup -- which is exactly what an eval inside a function
    // needs to reach its caller's.
    compiler.allow_arguments_ = true;
    compiler.outer_with_ = outer_with;
    compiler.annexb_fn_vars_ = std::move(annexb_fn_vars);
    if (track_completion) {
        compiler.completion_reg_ = compiler.alloc_temp();
        if (compiler.failed_) return nullptr;
        compiler.emit_completion_reset();
    }
    // The sweep above walks with inside_closure off, so a direct eval written
    // at the top level never reaches its flag. Ask again for that one: every
    // name up here is already an outer binding, which is exactly what a direct
    // eval needs, but its assignments still have to resolve ahead of the right
    // side (see Op::ResolveBindingEnv).
    {
        std::unordered_set<std::string> names;
        ScanOpacity top_op;
        for (const auto& st : statements) {
            collect_closure_names(st.get(), /*inside_closure=*/true, names, top_op);
        }
        compiler.eval_in_body_ = top_op.saw_eval;
    }
    // Same global_decl_count_ bookkeeping as compile()'s param/declared/rest
    // loop above -- without it, script-level nested lexicals never qualify
    // for LdaEnvSlot/StaEnvSlot (the uniqueness check always misses).
    for (const auto& info : declared) compiler.global_decl_count_[info.name]++;
    {
        std::vector<const ASTNode*> roots;
        roots.reserve(statements.size());
        for (const auto& st : statements) roots.push_back(st.get());
        compiler.sibling_safe_names_ = compute_sibling_safe_names(roots);
    }
    for (const auto& info : declared) {
        if (!info.is_lexical && !info.is_catch_param) continue;
        // The same refusal compile() makes: a register carries no mutability,
        // so a const that is ever assigned would compile to a plain Star and
        // be overwritten in silence. compile_script was missing this, which
        // left `{ const c = 1; c = 2; }` at script level writing through.
        if (info.is_const && !env_resident.count(info.name)) {
            for (const auto& st : statements) {
                if (assigns_to_identifier(st.get(), info.name)) return nullptr;
            }
        }
        if (env_resident.count(info.name)) {
            compiler.declare_local(info.name);
            if (info.is_const) compiler.const_locals_.insert(info.name);
        } else {
            if (!compiler.declare_local(info.name)) {
                // A repeated `var` is not a second binding at all: var is
                // function-scoped and hoisted, so both declarations name the
                // one variable and the register already standing for it is the
                // right one. Two counted loops in a row, each opening with
                // `var i`, is the ordinary way to write that, and refusing on
                // it handed the whole function to the tree-walker.
                //
                // Otherwise: disjoint siblings share one register on purpose,
                // the second declaration finding the first one's and reusing
                // it, which is the whole point of proving the regions never
                // overlap. Any other repeat is a real shadow, and refusing to
                // compile is right.
                const bool same_var = !info.is_lexical && compiler.lookup_local(info.name) >= 0;
                if (!same_var &&
                    (!sibling_safe.count(info.name) || compiler.lookup_local(info.name) < 0)) {
                    return nullptr;
                }
            }
            if (info.is_const) compiler.const_locals_.insert(info.name);
            if (info.is_lexical) {
                compiler.lexical_registers_.insert(compiler.lookup_local(info.name));
            }
        }
    }
    compiler.temp_watermark_ = compiler.next_register_;
    for (const auto& info : declared) {
        if (!info.is_lexical) continue;
        int reg = compiler.lookup_local(info.name);
        if (reg < 0 || compiler.env_names_.count(info.name)) continue;
        compiler.emit(Op::LdaTdz);
        compiler.emit(Op::Star);
        compiler.emit_u8(static_cast<uint8_t>(reg));
    }

    bool script_has_using = false;
    for (const auto& st : statements) {
        if (st->get_type() == ASTNode::Type::USING_DECLARATION) { script_has_using = true; break; }
    }
    FinallyScope script_escaped;
    auto emit_statements = [&]() -> bool {
        for (const auto& st : statements) {
            if (st->get_type() == ASTNode::Type::FUNCTION_DECLARATION) continue;  // pre-evaluated
            if (!compiler.compile_statement(st.get())) return false;
        }
        return true;
    };
    if (script_has_using) {
        if (!compiler.emit_dispose_scope_body(nullptr, emit_statements, script_escaped)) return nullptr;
        if (!compiler.emit_finally_pads(script_escaped)) return nullptr;
    } else if (!emit_statements()) {
        return nullptr;
    }
    if (compiler.completion_reg_ >= 0) {
        compiler.emit(Op::Ldar);
        compiler.emit_u8(static_cast<uint8_t>(compiler.completion_reg_));
    } else {
        compiler.emit(Op::LdaUndefined);
    }
    compiler.emit(Op::Return);

    compiler.chunk_->register_count = static_cast<uint16_t>(compiler.temp_watermark_);
    compiler.chunk_->parameter_count = 0;
    compiler.chunk_->env_mode = true;
    compiler.chunk_->script_mode = true;
    if (compiler.chunk_->uses_lookup_cache) {
        compiler.chunk_->lookup_cache = FixedArray<BytecodeChunk::LookupCacheEntry>::filled(
            static_cast<uint32_t>(compiler.names_.size()), BytecodeChunk::LookupCacheEntry{});
    }
    compiler.fuse_store_pairs();
    compiler.chunk_->code = FixedArray<uint8_t>::from(std::move(compiler.code_));
    compiler.chunk_->constants = FixedArray<Value>::from(std::move(compiler.constants_));
    compiler.chunk_->names = intern_name_pool(std::move(compiler.names_));
    compiler.chunk_->feedback = FixedArray<FeedbackSlot>::from(std::move(compiler.feedback_));
#ifdef QUANTA_VALIDATE_BYTECODE
        if (compiler.chunk_) validate_chunk_registers(*compiler.chunk_, std::string());
#endif
    return std::move(compiler.chunk_);
}

std::unique_ptr<BytecodeChunk> BytecodeCompiler::compile_default_value(const ASTNode* expr,
                                                                      bool suspendable) {
    static const std::vector<std::unique_ptr<Parameter>> no_params;
    auto chunk = compile(expr, ParamList::from_nodes(no_params), suspendable, /*is_arrow=*/false, /*is_strict=*/false,
                         /*env_bound=*/nullptr, /*outer_with=*/false, /*allow_arguments=*/true);
    return chunk;
}

std::unique_ptr<BytecodeChunk> BytecodeCompiler::compile_pattern_binder(const ASTNode* pattern) {
    if (!pattern) return nullptr;
    BytecodeCompiler compiler({}, /*env_mode=*/true);
    if (compiler.failed_) return nullptr;
    // Script mode is what makes a leaf bind by name rather than into a
    // register this chunk would have to own, which is exactly how a parameter
    // scope receives them. It also leaves the chunk without an entry
    // environment, so nothing it resolves is remembered -- the chunk is shared
    // by every call, and every call builds the scope again.
    compiler.script_mode_ = true;
    // The arguments object is built before the parameters are bound, so a
    // default inside the pattern reaching for it is an ordinary chain lookup.
    compiler.allow_arguments_ = true;
    // The value arrives in the accumulator; the pattern reads it from there.
    if (!compiler.pattern_is_emittable(pattern, /*is_lexical=*/true, /*is_assignment=*/false)) {
        return nullptr;
    }
    if (!compiler.emit_pattern_bind(pattern, /*is_lexical=*/true, /*is_const=*/false)) {
        return nullptr;
    }
    compiler.emit(Op::LdaUndefined);
    compiler.emit(Op::Return);
    if (compiler.failed_) return nullptr;

    compiler.chunk_->register_count = static_cast<uint16_t>(compiler.temp_watermark_);
    compiler.chunk_->parameter_count = 0;
    compiler.chunk_->env_mode = true;
    compiler.chunk_->script_mode = true;
    if (compiler.chunk_->uses_lookup_cache) {
        compiler.chunk_->lookup_cache = FixedArray<BytecodeChunk::LookupCacheEntry>::filled(
            static_cast<uint32_t>(compiler.names_.size()), BytecodeChunk::LookupCacheEntry{});
    }
    compiler.fuse_store_pairs();
    compiler.chunk_->code = FixedArray<uint8_t>::from(std::move(compiler.code_));
    compiler.chunk_->constants = FixedArray<Value>::from(std::move(compiler.constants_));
    compiler.chunk_->names = intern_name_pool(std::move(compiler.names_));
    compiler.chunk_->feedback = FixedArray<FeedbackSlot>::from(std::move(compiler.feedback_));
    return std::move(compiler.chunk_);
}

BytecodeCompiler::BytecodeCompiler(const std::vector<std::string>& param_names, bool env_mode,
                                   const std::unordered_set<std::string>* env_resident)
    : chunk_(std::make_unique<BytecodeChunk>()), env_mode_(env_mode) {
    if (env_resident) {
        full_env_ = false;
        env_resident_ = *env_resident;
    }
    // A repeated simple parameter name is legal in sloppy code and refers to
    // the last one. Whichever storage it gets, the later occurrence takes the
    // name over; the earlier register still holds its argument, unread.
    if (env_mode_ && full_env_) {
        for (const auto& p : param_names) env_names_.insert(p);
        return;
    }
    // Register (or selective) mode: every param owns register i. A selective
    // env-resident param keeps its register as the entry seed source; all
    // reads/writes go through the env binding (see emit_read_local).
    for (const auto& p : param_names) {
        locals_[p] = next_register_++;
        if (env_mode_ && env_resident_.count(p)) env_names_.insert(p);
    }
}

int BytecodeCompiler::setup_loop_env(std::vector<BytecodeChunk::LoopEnvVar> extra_vars, const ASTNode* body,
                                      bool force_own_env,
                                      const std::vector<const ASTNode*>& extra_capture_roots) {
    if (!env_mode_) return -1;
    std::vector<BytecodeChunk::LoopEnvVar> vars = std::move(extra_vars);
    bool needs_own_env = force_own_env;
    // A block body declares its own lexicals in its OWN environment (see the
    // BLOCK_STATEMENT case), which is entered after the test and left before
    // the update. Taking them here too put them in the loop's own scope, alive
    // and in TDZ, while the test and the update run -- and those sit outside
    // the body, so a name the body shadows still has to mean what it meant
    // outside:
    //     const {length: n} = a;
    //     for (let o = 0; o < n; o++) { const n = a[o]; ... }
    // resolved `o < n` to the body's uninitialised binding and threw. Only the
    // flags that scan raises (a class declaration, a destructuring pattern)
    // still matter here, since they decide whether an env is needed at all.
    if (body && body->get_type() == ASTNode::Type::BLOCK_STATEMENT) {
        std::vector<BytecodeChunk::LoopEnvVar> owned_by_the_block;
        if (!collect_direct_lexical_decls(body, owned_by_the_block, needs_own_env)) return -1;
    } else if (!collect_direct_lexical_decls(body, vars, needs_own_env)) {
        return -1;
    }
    // Register-resident lexicals get their TDZ re-armed at block entry
    // instead of living in the per-iteration env.
    vars.erase(std::remove_if(vars.begin(), vars.end(),
                              [&](const BytecodeChunk::LoopEnvVar& v) {
                                  return !env_names_.count(v.name) && lookup_local(v.name) >= 0;
                              }),
               vars.end());
    if (vars.empty() && !needs_own_env) return -1;
    record_env_slot_info(vars, env_depth_ + 1);
    // force_own_env (destructuring for-of/for-in) needs a genuinely fresh
    // binding every iteration independent of closure capture: Op::
    // A pattern's own binding write silently no-ops re-declaring
    // an already-bound name (see compile_for_each_loop's own doc comment),
    // so without a fresh env each iteration keeps writing into iteration 1's
    // now-permanent binding instead of a new one.
    std::vector<const ASTNode*> capture_roots = extra_capture_roots;
    capture_roots.push_back(body);
    loop_env_needs_fresh_.push_back(force_own_env || loop_vars_may_be_captured(capture_roots, vars));
    chunk_->ensure_env().loop_envs.push_back(std::move(vars));
    return static_cast<int>(chunk_->ensure_env().loop_envs.size() - 1);
}

void BytecodeCompiler::record_env_slot_info(const std::vector<BytecodeChunk::LoopEnvVar>& vars, int depth) {
    // Must match Environment::SlotMap::kInlineCapacity (Context.h) -- not
    // included here to avoid coupling this file to the full Environment
    // definition just for one constant.
    constexpr size_t kInlineCapacity = 4;
    for (size_t pos = 0; pos < vars.size() && pos < kInlineCapacity; pos++) {
        const std::string& name = vars[pos].name;
        auto it = global_decl_count_.find(name);
        if (it != global_decl_count_.end() &&
            (it->second == 1 || sibling_safe_names_.count(name))) {
            env_slot_info_[name] = EnvSlotInfo{static_cast<uint8_t>(pos), depth};
        }
    }
}

std::vector<std::string> BytecodeCompiler::take_pending_labels() {
    std::vector<std::string> labels = std::move(pending_labels_);
    pending_labels_.clear();
    return labels;
}

bool BytecodeCompiler::compile_for_each_loop(const ASTNode* left, const ASTNode* right,
                                              const ASTNode* body, bool is_for_in,
                                              int left_decl_kind, bool is_await) {
    // Supported targets: a simple identifier, or a destructuring pattern WITH
    // a declaration keyword (keywordless `for ({a} of arr)` reports kind -1
    // and needs arbitrary-AssignmentTarget writeback this path doesn't have).
    emit_completion_reset();
    std::string var_name;
    bool declare_fresh = false;
    bool is_const = false;
    bool is_lexical = false;
    const ASTNode* pattern_lit = nullptr;
    const MemberExpression* mem_target = nullptr;
    const UsingDeclaration* using_decl = nullptr;
    if (left->get_type() == ASTNode::Type::VARIABLE_DECLARATION) {
        const auto* vd = static_cast<const VariableDeclaration*>(left);
        if (vd->declaration_count() != 1) return false;
        const auto& d = vd->get_declarations()[0];
        if (!d->get_id()) return false;
        var_name = d->get_id()->get_name();
        declare_fresh = true;
        is_const = vd->get_kind() == VariableDeclarator::Kind::CONST;
        is_lexical = vd->get_kind() != VariableDeclarator::Kind::VAR;
    } else if (left->get_type() == ASTNode::Type::IDENTIFIER) {
        var_name = static_cast<const Identifier*>(left)->get_name();
    } else if (left->get_type() == ASTNode::Type::DESTRUCTURING_ASSIGNMENT ||
               left->get_type() == ASTNode::Type::ARRAY_LITERAL ||
               left->get_type() == ASTNode::Type::OBJECT_LITERAL) {
        // A pattern with a declaration keyword arrives wrapped; without one
        // the head holds the bare literal. The keywordless form assigns to
        // targets that already exist rather than binding new ones, which is
        // the distinction emit_pattern_bind's is_assignment draws.
        pattern_lit = left->get_type() == ASTNode::Type::DESTRUCTURING_ASSIGNMENT
                          ? static_cast<const DestructuringAssignment*>(left)->get_pattern_literal()
                          : left;
        declare_fresh = left_decl_kind >= 0;
        is_const = left_decl_kind == 2;
        is_lexical = left_decl_kind > 0;  // 0 = var, -1 = no keyword
    } else if (left->get_type() == ASTNode::Type::USING_DECLARATION) {
        // `for (using x of it)` binds like a const, and additionally hands the
        // value to a dispose scope that closes at the end of every iteration.
        using_decl = static_cast<const UsingDeclaration*>(left);
        if (using_decl->get_bindings().size() != 1) return false;
        var_name = using_decl->get_bindings()[0].name;
        declare_fresh = true;
        is_const = true;
        is_lexical = true;
    } else if (left->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
        mem_target = static_cast<const MemberExpression*>(left);
        if (member_is_super(mem_target)) {
            if (!super_member_emittable(mem_target)) return false;
        } else if (!member_is_private(mem_target) && !member_is_supported(mem_target)) {
            return false;
        }
    } else {
        return false;  // bare destructuring (no declaration keyword) / member-expression LHS
    }
    // A target that is not a compiler local resolves by name at write time
    // (Op::StaLookup), which is where an outer const's TypeError comes from
    // too. Requiring a local here handed whole functions to the tree-walker
    // over a loop head that writes a global or an outer binding.
    // A keywordless target is an assignment, and assigning to a const has to
    // throw. emit_write_local would emit a plain Star and silently overwrite
    // it, so hand the whole loop to the tree-walker, which raises. A declared
    // target is not that case: `for (const k of ...)` binds k afresh each
    // iteration rather than assigning to an existing const, and refusing it
    // here kept whole functions out of the compiler over their loop heads.
    if (!pattern_lit && !mem_target && !declare_fresh && const_locals_.count(var_name) &&
        !env_names_.count(var_name) && lookup_local(var_name) >= 0) {
        return false;
    }

    // Entered before compiling `right`: a lexical ForDeclaration's bound name
    // is in TDZ even during the head's own iterable/object expression (spec).
    //
    // Destructuring targets must NOT be pre-listed in extra_vars: bind_or_set
    // always create_lexical_binding's fresh (no TDZ-slot-initialize mode), so
    // a pre-declared name would silently no-op after iteration 1.
    // force_own_env still gives lexicals a fresh per-iteration env.
    std::vector<BytecodeChunk::LoopEnvVar> extra_vars;
    if (pattern_lit) {
        // The leaf names still need TDZ bindings before the head's own
        // iterable expression runs (spec 14.7.5.7): a closure made there and
        // called later must see them uninitialized rather than resolve past
        // the loop to whatever the enclosing scope binds. Only the names this
        // chunk does not already own as locals go here -- for the others the
        // binder writes the local it was given.
        if (is_lexical) {
            std::vector<DeclInfo> leaves;
            if (collect_flat_pattern_names(left, true, is_const, leaves)) {
                for (const auto& d : leaves) {
                    if (!d.name.empty() && !env_names_.count(d.name)) continue;
                    if (!d.name.empty()) extra_vars.push_back({d.name, true, is_const, false});
                }
            }
        }
    } else if (mem_target) {
        // Nothing is bound: the target is a property of an existing object.
    } else if (declare_fresh && is_lexical && env_mode_ && env_names_.count(var_name)) {
        // Only a let/const head gets a binding of its own here. A `var` names
        // the one function-scoped binding on every iteration, so listing it
        // would hand each iteration a separate copy -- a closure made in the
        // body would then report the value that iteration saw instead of the
        // last one.
        extra_vars.push_back({var_name, true, is_const, false});
    }
    lexical_scopes_.emplace_back();
    if (is_lexical) {
        if (pattern_lit) {
            std::vector<DeclInfo> leaves;
            if (collect_flat_pattern_names(left, true, is_const, leaves)) {
                for (const auto& d : leaves) {
                    lexical_scopes_.back().push_back(d.name);
                    block_scoped_names_.insert(d.name);
                }
            }
        } else if (!var_name.empty()) {
            lexical_scopes_.back().push_back(var_name);
            block_scoped_names_.insert(var_name);
        }
    }
    struct LexicalScopePop {
        BytecodeCompiler* c;
        ~LexicalScopePop() { c->lexical_scopes_.pop_back(); }
    } lexical_pop{this};
    int loop_env_idx = setup_loop_env(std::move(extra_vars), body, /*force_own_env=*/pattern_lit && is_lexical,
                                       {left, right});
    if (loop_env_idx >= 0) {
        emit(Op::EnterLoopEnv);
        emit_u16(static_cast<uint16_t>(loop_env_idx));
        env_depth_++;
    }

    if (!compile_expression(right)) return false;  // acc = iterable/object
    // for-in enumerates a snapshot of the keys, so one deleted while the body
    // runs would still be visited. Keep the object around to re-ask.
    int forin_obj_reg = -1;
    int forin_key_reg = -1;
    int forin_keys_reg = -1;
    if (is_for_in) {
        forin_obj_reg = alloc_temp();
        if (failed_) return false;
        forin_key_reg = alloc_temp();
        if (failed_) return false;
        // The key list itself, kept for the per-key presence check below: it
        // is how that check recognizes a list still describing the receiver.
        forin_keys_reg = alloc_temp();
        if (failed_) return false;
        emit(Op::CreateForInKeys);  // acc = Array of enumerable key strings
        emit_u8(static_cast<uint8_t>(forin_obj_reg));
        emit(Op::Star);
        emit_u8(static_cast<uint8_t>(forin_keys_reg));
    }

    int next_fn_reg = alloc_temp();
    if (failed_) return false;
    int from_sync_reg = -1;
    if (is_await) {
        from_sync_reg = alloc_temp();
        if (failed_) return false;
        emit(Op::GetAsyncIterator);
        emit_u8(static_cast<uint8_t>(next_fn_reg));
        emit_u8(static_cast<uint8_t>(from_sync_reg));
    } else {
        emit(Op::GetIterator);
        emit_u8(static_cast<uint8_t>(next_fn_reg));
    }
    int iterator_reg = alloc_temp();
    if (failed_) return false;
    emit(Op::Star);
    emit_u8(static_cast<uint8_t>(iterator_reg));

    if (loop_env_idx >= 0 && loop_env_needs_fresh(loop_env_idx)) {
        // Spec 14.7.5.6 ForIn/OfBodyEvaluation step 1: CreatePerIterationEnvironment
        // runs once more right after head evaluation (the object/iterable
        // expression, where a closure may capture the still-TDZ binding),
        // before the first per-iteration binding write -- same reasoning as
        // the C-style FOR_STATEMENT fix above. Skipped when no closure
        // anywhere in left/right/body can observe it.
        emit(Op::AdvanceLoopEnv);
        emit_u16(static_cast<uint16_t>(loop_env_idx));
    }

    size_t loop_start = code_.size();
    emit(is_await ? Op::AsyncIteratorNextOrJump : Op::IteratorNextOrJump);
    emit_u8(static_cast<uint8_t>(iterator_reg));
    emit_u8(static_cast<uint8_t>(next_fn_reg));
    if (is_await) emit_u8(static_cast<uint8_t>(from_sync_reg));
    size_t next_jump = code_.size();
    emit_u16(0);  // patched below to pre_exit (done, or the iterator threw)

    // Same skip the tree-walker performs: a key the object no longer has is
    // passed over rather than bound, which is what `delete` inside the body
    // has to be able to do.
    if (is_for_in) {
        emit(Op::Star);
        emit_u8(static_cast<uint8_t>(forin_key_reg));
        emit(Op::ForInKeyPresent);
        emit_u8(static_cast<uint8_t>(forin_obj_reg));
        emit_u8(static_cast<uint8_t>(forin_keys_reg));
        emit_u8(static_cast<uint8_t>(forin_key_reg));
        if (!emit_jump_back(Op::JumpIfFalse, loop_start)) return false;
        emit(Op::Ldar);
        emit_u8(static_cast<uint8_t>(forin_key_reg));
    }

    // The per-iteration write is inside the protected region, not just the
    // body: spec 14.7.5.6 closes the iterator when binding the value throws
    // (an assignment to a const, a destructuring pattern's getter). The
    // iterator's own next() is deliberately left outside -- a throw from
    // there is not followed by IteratorClose.
    size_t body_start = code_.size();
    if (pattern_lit) {
        if (!pattern_is_emittable(pattern_lit, is_lexical, /*is_assignment=*/!declare_fresh)) {
            return false;
        }
        if (!emit_pattern_bind(pattern_lit, is_lexical, is_const,
                               /*is_assignment=*/!declare_fresh)) {
            return false;
        }
    } else if (mem_target) {
        // The reference is evaluated per iteration, after the value is in
        // hand (spec 14.7.5.6 step 6.f), so the object and key expressions
        // run here rather than once before the loop.
        int val_reg = alloc_temp();
        if (failed_) return false;
        emit(Op::Star);
        emit_u8(static_cast<uint8_t>(val_reg));
        int key_reg = -1;
        int obj_reg = emit_member_target_resolve(mem_target, /*is_assignment=*/true, key_reg);
        if (obj_reg < 0 || failed_) return false;
        emit(Op::Ldar);
        emit_u8(static_cast<uint8_t>(val_reg));
        if (!emit_pattern_target_store(mem_target, /*is_lexical=*/false, /*is_const=*/false,
                                       /*is_assignment=*/true, /*with_target_reg=*/-1,
                                       obj_reg, key_reg)) {
            return false;
        }
        free_temp(val_reg);  // releases obj_reg and key_reg with it (allocated after)
    } else if (!using_decl) {
        if (!declare_fresh && lexical_out_of_scope(var_name)) {
            // The register this name owns belongs to a scope this head is not
            // inside, so the assignment resolves by name instead.
            emit(Op::StaLookup);
            emit_u16(add_name(var_name));
        } else {
            emit_write_local(var_name, /*is_declaration=*/declare_fresh);
        }
    }

    // The resource is parked while the dispose scope opens, then registered
    // from inside it: a value with no dispose method throws, and that throw
    // has to unwind through the scope that would have disposed it.
    int using_val_reg = -1;
    if (using_decl) {
        using_val_reg = alloc_temp();
        if (failed_) return false;
        emit(Op::Star);
        emit_u8(static_cast<uint8_t>(using_val_reg));
    }

    loop_stack_.push_back({0, {}, {}, true, env_depth_, try_env_depth_, false, take_pending_labels(), iterator_reg, is_await});
    if (using_decl) {
        FinallyScope using_escaped;
        auto emit_using_body = [&]() {
            emit(Op::Ldar);
            emit_u8(static_cast<uint8_t>(using_val_reg));
            emit(Op::RegisterDisposable);
            emit_u8(using_decl->is_await() ? 1 : 0);
            emit_write_local(var_name, /*is_declaration=*/true);
            return compile_statement(body);
        };
        if (!emit_dispose_scope_body(body, emit_using_body, using_escaped)) return false;
        // Emitted while the loop is still on the stack: a pad stands in for a
        // break or continue and has to re-issue it against that loop.
        if (!emit_finally_pads(using_escaped)) return false;
    } else if (!compile_statement(body)) {
        return false;
    }
    LoopScope scope = std::move(loop_stack_.back());
    loop_stack_.pop_back();
    size_t body_end = code_.size();

    for (size_t pos : scope.continue_patches) {
        if (!patch_jump(pos)) return false;  // continue lands on the advance step
    }
    if (loop_env_idx >= 0 && loop_env_needs_fresh(loop_env_idx)) {
        emit(Op::AdvanceLoopEnv);
        emit_u16(static_cast<uint16_t>(loop_env_idx));
    }
    if (!emit_jump_back(Op::Jump, loop_start)) return false;

    // break: close the iterator (validating return()'s result, per spec)
    // before falling into the same exit path as "done".
    for (size_t pos : scope.break_patches) {
        if (!patch_jump(pos)) return false;
    }
    emit(is_await ? Op::AsyncIteratorClose : Op::IteratorClose);
    emit_u8(static_cast<uint8_t>(iterator_reg));
    emit_u8(0);  // mode 0: validate, no pending exception

    if (!patch_jump(next_jump)) return false;
    if (loop_env_idx >= 0) { emit(Op::ExitLoopEnv); env_depth_--; }

    // Normal control flow ends here; skip over the exception-cleanup
    // handler below (reached only via CHECK_EXC's handler-table jump).
    size_t skip_cleanup = emit_jump(Op::Jump);
    size_t cleanup_pc = code_.size();
    emit(is_await ? Op::AsyncIteratorClose : Op::IteratorClose);
    emit_u8(static_cast<uint8_t>(iterator_reg));
    emit_u8(1);  // mode 1: acc holds the pending exception -- restore + re-raise
    const size_t handler_idx = chunk_->ensure_handlers().size();
    chunk_->ensure_handlers().push_back({static_cast<uint32_t>(body_start),
                                 static_cast<uint32_t>(body_end),
                                 static_cast<uint32_t>(cleanup_pc)});

    // A `return()` on the generator resumes a suspension in the body by
    // unwinding a C++ exception, which reaches run() rather than this handler,
    // so it needs a landing pad of its own -- the loop's iterator has to be
    // closed before the completion goes on out. Same shape try/finally uses.
    if (suspendable_ && contains_suspend(body)) {
        size_t genreturn_pc = code_.size();
        int gr_temp = alloc_temp();
        if (failed_) return false;
        emit(Op::Star);
        emit_u8(static_cast<uint8_t>(gr_temp));
        emit(is_await ? Op::AsyncIteratorClose : Op::IteratorClose);
        emit_u8(static_cast<uint8_t>(iterator_reg));
        emit_u8(0);  // mode 0: validate, the completion is a return, not a throw
        emit(Op::Ldar);
        emit_u8(static_cast<uint8_t>(gr_temp));
        free_temp(gr_temp);
        emit(Op::ReraiseGeneratorReturn);
        chunk_->ensure_handlers()[handler_idx].genreturn_pc = static_cast<int32_t>(genreturn_pc);
    }
    if (!patch_jump(skip_cleanup)) return false;

    free_temp(next_fn_reg);  // frees next_fn_reg and iterator_reg (contiguous, LIFO)
    return true;
}

bool BytecodeCompiler::is_local(const std::string& name) const {
    return env_names_.count(name) > 0 || locals_.count(name) > 0;
}

int BytecodeCompiler::lookup_local(const std::string& name) const {
    auto it = locals_.find(name);
    return it != locals_.end() ? it->second : -1;
}

bool BytecodeCompiler::declare_local(const std::string& name) {
    if (env_mode_ && (full_env_ || env_resident_.count(name))) {
        return env_names_.insert(name).second;
    }
    if (locals_.count(name)) return false;
    if (next_register_ >= kMaxRegisters) return false;
    locals_[name] = next_register_++;
    // Keeps register_count an upper bound even if a local is ever declared
    // after the setup pass that seeds the watermark.
    if (next_register_ > temp_watermark_) temp_watermark_ = next_register_;
    return true;
}

int BytecodeCompiler::alloc_temp() {
    if (next_register_ >= kMaxRegisters) { failed_ = true; return 0; }
    int reg = next_register_++;
    if (next_register_ > temp_watermark_) temp_watermark_ = next_register_;
    return reg;
}

void BytecodeCompiler::free_temp(int reg) {
    // Temps are strictly LIFO within one expression tree.
    next_register_ = reg;
}

int BytecodeCompiler::plain_local_register(const std::string& name) const {
    if (env_names_.count(name)) return -1;
    int reg = lookup_local(name);
    if (reg < 0) return -1;
    if (lexical_registers_.count(reg) && !initialized_lexicals_.count(reg)) return -1;
    return reg;
}

bool BytecodeCompiler::leaves_locals_untouched(const ASTNode* expr) const {
    if (!expr) return false;
    switch (expr->get_type()) {
        case ASTNode::Type::NUMBER_LITERAL:
        case ASTNode::Type::STRING_LITERAL:
        case ASTNode::Type::BOOLEAN_LITERAL:
        case ASTNode::Type::NULL_LITERAL:
        case ASTNode::Type::UNDEFINED_LITERAL:
            return true;
        case ASTNode::Type::IDENTIFIER:
            // Only a register-resident, initialized local: any other name is a
            // chain lookup, which can run a getter or throw.
            return plain_local_register(
                static_cast<const Identifier*>(expr)->get_name()) >= 0;
        default:
            return false;
    }
}

void BytecodeCompiler::emit_read_local(const std::string& name) {
    if (env_names_.count(name)) {
        auto it = env_slot_info_.find(name);
        if (it != env_slot_info_.end()) {
            if (it->second.depth == env_depth_) {
                emit(Op::LdaEnvSlot);
                emit_u8(it->second.slot);
                emit_u16(add_name(name));
                return;
            }
            // The declaring scope is exactly `hops` Environments out from the
            // one active here -- env_depth_ counts the same EnterLoopEnv/
            // BindEnvLocals nesting the declaration's own depth was recorded
            // against, so the difference is fixed for every instance of this
            // read, the same fact that already lets break/continue unwind by
            // an exact count rather than walking by name. A hop count past
            // what a byte can hold is pathological nesting no real program
            // reaches; the name-walk path below still answers it correctly.
            const int hops = env_depth_ - it->second.depth;
            if (hops > 0 && hops <= 255) {
                emit(Op::LdaEnvSlotAt);
                emit_u8(static_cast<uint8_t>(hops));
                emit_u8(it->second.slot);
                emit_u16(add_name(name));
                return;
            }
        }
        emit(Op::LdaEnv);
        emit_u16(add_name(name));
        return;
    }
    int reg = lookup_local(name);
    // Not register-resident after all: the name lives further out, so read it
    // the way an outer name is read rather than encoding register -1.
    if (reg < 0) { emit(Op::LdaLookup); emit_u16(add_name(name)); return; }
    if (lexical_registers_.count(reg) && !initialized_lexicals_.count(reg)) {
        emit(Op::LdarChecked);
        emit_u8(static_cast<uint8_t>(reg));
        emit_u16(add_name(name));
    } else {
        emit(Op::Ldar);
        emit_u8(static_cast<uint8_t>(reg));
    }
}

void BytecodeCompiler::emit_write_local(const std::string& name, bool is_declaration) {
    if (env_names_.count(name)) {
        auto it = env_slot_info_.find(name);
        if (it != env_slot_info_.end()) {
            if (it->second.depth == env_depth_) {
                emit(is_declaration ? Op::StaEnvSlotInit : Op::StaEnvSlot);
                emit_u8(it->second.slot);
                emit_u16(add_name(name));
                return;
            }
            // A declaration always lands in the scope that was just entered
            // for it, which is the current one -- so a depth mismatch here
            // only ever means a plain write to a binding some number of
            // scopes out, the same fact emit_read_local acts on and for the
            // same reason.
            const int hops = env_depth_ - it->second.depth;
            if (!is_declaration && hops > 0 && hops <= 255) {
                emit(Op::StaEnvSlotAt);
                emit_u8(static_cast<uint8_t>(hops));
                emit_u8(it->second.slot);
                emit_u16(add_name(name));
                return;
            }
        }
        emit(is_declaration ? Op::StaEnvInit : Op::StaEnv);
        emit_u16(add_name(name));
        return;
    }
    int reg = lookup_local(name);
    if (reg < 0) { emit(Op::StaLookup); emit_u16(add_name(name)); return; }
    if (!is_declaration && lexical_registers_.count(reg) && !initialized_lexicals_.count(reg)) {
        emit(Op::StarChecked);
        emit_u8(static_cast<uint8_t>(reg));
        emit_u16(add_name(name));
    } else {
        emit(Op::Star);
        emit_u8(static_cast<uint8_t>(reg));
        if (is_declaration && switch_body_depth_ == 0 && lexical_registers_.count(reg)) {
            initialized_lexicals_.insert(reg);
        }
    }
}

namespace {
Op fused_store_form(Op producer) {
    switch (producer) {
        case Op::Ldar:     return Op::LdarStar;
        case Op::LdaSmi:   return Op::LdaSmiStar;
        case Op::LdaZero:  return Op::LdaZeroStar;
        case Op::LdaConst: return Op::LdaConstStar;
        case Op::LdaThis:  return Op::LdaThisStar;
        case Op::LdaEnv:   return Op::LdaEnvStar;
        case Op::LdaLookup: return Op::LdaLookupStar;
        case Op::LdaEnvSlot: return Op::LdaEnvSlotStar;
        case Op::GetNamed: return Op::GetNamedStar;
        default:           return Op::kCount;
    }
}

// Where an instruction of this kind keeps its jump offset, or -1 for the ones
// that hold none. Both forms are relative to the byte after the offset.
int jump_offset_at(char kind) {
    if (kind == 'o') return 0;
    if (kind == 'j') return 2;
    if (kind == 'J') return 3;
    return -1;
}
}

void BytecodeCompiler::fuse_store_pairs() {
    if (code_.empty()) return;
    const size_t n = code_.size();

    std::vector<uint32_t> starts;
    for (size_t pc = 0; pc < n; ) {
        Op op = static_cast<Op>(code_[pc]);
        if (op >= Op::kCount) return;
        starts.push_back(static_cast<uint32_t>(pc));
        pc += 1 + static_cast<size_t>(op_operand_bytes(op));
        if (pc > n) return;  // the last instruction runs off the end: leave it alone
    }

    // Anything control can arrive at directly keeps its own address. Handler
    // bounds count too: end_pc is exclusive, so swallowing the instruction it
    // names would pull code into a try region that was not in it.
    std::vector<bool> pinned(n + 1, false);
    for (uint32_t s : starts) {
        const int off_at = jump_offset_at(op_operand_kind(static_cast<Op>(code_[s])));
        if (off_at < 0) continue;
        const size_t off_pos = s + 1 + static_cast<size_t>(off_at);
        const int16_t off = static_cast<int16_t>(
            static_cast<uint16_t>(code_[off_pos]) |
            (static_cast<uint16_t>(code_[off_pos + 1]) << 8));
        const ptrdiff_t target = static_cast<ptrdiff_t>(off_pos) + 2 + off;
        if (target < 0 || static_cast<size_t>(target) > n) return;
        pinned[static_cast<size_t>(target)] = true;
    }
    if (chunk_ && chunk_->handlers) {
        for (const auto& e : *chunk_->handlers) {
            for (uint32_t pc : {e.start_pc, e.end_pc, e.handler_pc}) {
                if (pc <= n) pinned[pc] = true;
            }
            if (e.genreturn_pc >= 0 && static_cast<size_t>(e.genreturn_pc) <= n) {
                pinned[static_cast<size_t>(e.genreturn_pc)] = true;
            }
        }
    }

    std::vector<uint8_t> out;
    out.reserve(n);
    // Old offset -> new offset, for every instruction start plus the end.
    std::vector<uint32_t> moved(n + 1, 0);
    // Where each instruction ended up, paired with where it came from, so the
    // jump pass does not have to re-derive which pairs were fused.
    std::vector<std::pair<uint32_t, uint32_t>> placed;  // {new_pc, old_pc}
    placed.reserve(starts.size());
    bool changed = false;

    for (size_t i = 0; i < starts.size(); i++) {
        const uint32_t s = starts[i];
        moved[s] = static_cast<uint32_t>(out.size());
        placed.push_back({static_cast<uint32_t>(out.size()), s});
        const Op op = static_cast<Op>(code_[s]);
        const int operands = op_operand_bytes(op);
        const Op fused = fused_store_form(op);
        if (fused != Op::kCount && i + 1 < starts.size() &&
            static_cast<Op>(code_[starts[i + 1]]) == Op::Star && !pinned[starts[i + 1]]) {
            out.push_back(static_cast<uint8_t>(fused));
            for (int k = 0; k < operands; k++) out.push_back(code_[s + 1 + k]);
            out.push_back(code_[starts[i + 1] + 1]);   // the Star's register
            moved[starts[i + 1]] = static_cast<uint32_t>(out.size());
            i++;
            changed = true;
            continue;
        }
        out.push_back(static_cast<uint8_t>(op));
        for (int k = 0; k < operands; k++) out.push_back(code_[s + 1 + k]);
    }
    moved[n] = static_cast<uint32_t>(out.size());
    if (!changed) return;

    // Every jump was measured against the old layout; re-measure it against
    // the new one. Only shrinking happens here, so an offset that fit before
    // still fits.
    for (const auto& [new_pc, old_pc] : placed) {
        const int off_at = jump_offset_at(op_operand_kind(static_cast<Op>(out[new_pc])));
        if (off_at < 0) continue;
        const size_t old_off_pos = old_pc + 1 + static_cast<size_t>(off_at);
        const int16_t old_off = static_cast<int16_t>(
            static_cast<uint16_t>(code_[old_off_pos]) |
            (static_cast<uint16_t>(code_[old_off_pos + 1]) << 8));
        const size_t old_target = old_off_pos + 2 + old_off;
        const size_t new_off_pos = new_pc + 1 + static_cast<size_t>(off_at);
        const ptrdiff_t delta = static_cast<ptrdiff_t>(moved[old_target]) -
                                static_cast<ptrdiff_t>(new_off_pos + 2);
        if (delta < INT16_MIN || delta > INT16_MAX) { failed_ = true; return; }
        const uint16_t enc = static_cast<uint16_t>(static_cast<int16_t>(delta));
        out[new_off_pos] = static_cast<uint8_t>(enc & 0xFF);
        out[new_off_pos + 1] = static_cast<uint8_t>(enc >> 8);
    }

    if (chunk_ && chunk_->handlers) {
        for (auto& e : *chunk_->handlers) {
            e.start_pc = moved[e.start_pc];
            e.end_pc = moved[e.end_pc];
            e.handler_pc = moved[e.handler_pc];
            if (e.genreturn_pc >= 0) e.genreturn_pc = static_cast<int32_t>(moved[e.genreturn_pc]);
        }
    }
    code_.swap(out);
}

void BytecodeCompiler::emit(Op op) {
    if (op == Op::LdaLookup || op == Op::StaLookup) chunk_->uses_lookup_cache = true;
    if (op == Op::LdaThis) chunk_->uses_this = true;
    code_.push_back(static_cast<uint8_t>(op));
}
void BytecodeCompiler::emit_u8(uint8_t v) { code_.push_back(v); }
void BytecodeCompiler::emit_u16(uint16_t v) {
    code_.push_back(static_cast<uint8_t>(v & 0xFF));
    code_.push_back(static_cast<uint8_t>(v >> 8));
}

uint32_t BytecodeCompiler::add_constant(const Value& v) {
    // A chunk that needs more than 4 billion constants is not one this
    // compiler could hold anyway; the narrow operand's limit is not the pool's.
    if (constants_.size() >= 0xFFFFFFFEu) { failed_ = true; return 0; }
    constants_.push_back(v);
    return static_cast<uint32_t>(constants_.size() - 1);
}

// The narrow operand carries the index directly; past its range the wide form
// does, so a long literal is not a reason to refuse the whole function.
void BytecodeCompiler::emit_load_const(const Value& v) {
    uint32_t idx = add_constant(v);
    if (idx <= 0xFFFFu) {
        emit(Op::LdaConst);
        emit_u16(static_cast<uint16_t>(idx));
        return;
    }
    emit(Op::LdaConstWide);
    emit_u32(idx);
}

void BytecodeCompiler::emit_u32(uint32_t v) {
    code_.push_back(static_cast<uint8_t>(v & 0xFF));
    code_.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    code_.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    code_.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

uint16_t BytecodeCompiler::add_name(const std::string& name) {
    auto it = name_index_.find(name);
    if (it != name_index_.end()) return it->second;
    if (names_.size() >= 0xFFFF) { failed_ = true; return 0; }
    const uint16_t index = static_cast<uint16_t>(names_.size());
    names_.push_back(name);
    name_index_.emplace(name, index);
    return index;
}

uint16_t BytecodeCompiler::alloc_feedback_slot() {
    if (feedback_.size() >= 0xFFFF) { failed_ = true; return 0; }
    feedback_.push_back(FeedbackSlot{});
    return static_cast<uint16_t>(feedback_.size() - 1);
}

uint16_t BytecodeCompiler::alloc_private_feedback() {
    auto& pf = chunk_->ensure_ic_feedback().private_feedback;
    if (pf.size() >= 0xFFFF) { failed_ = true; return 0; }
    pf.push_back(PrivateFeedback{});
    return static_cast<uint16_t>(pf.size() - 1);
}

uint16_t BytecodeCompiler::alloc_keyed_feedback() {
    auto& kf = chunk_->ensure_ic_feedback().keyed_feedback;
    if (kf.size() >= 0xFFFF) { failed_ = true; return 0; }
    kf.push_back(KeyedFeedback{});
    return static_cast<uint16_t>(kf.size() - 1);
}

// True if every leaf of this pattern is a shape emit_pattern_bind can express.
// Checked up front so a refusal costs no half-emitted bytecode.
// Whether an assignment pattern may write this name from a register. A `let`
// still inside its own initialiser (`let y = [y] = []`) has to raise a
// ReferenceError, and a register carries no such state, so that case goes to
// the tree-walker rather than storing in silence.
bool BytecodeCompiler::pattern_target_is_writable(const std::string& name) const {
    if (env_names_.count(name)) return true;
    int reg = lookup_local(name);
    if (reg < 0) return true;  // resolved through the chain at run time
    return !(lexical_registers_.count(reg) && !initialized_lexicals_.count(reg));
}

bool BytecodeCompiler::pattern_is_emittable(const ASTNode* pattern, bool is_lexical, bool is_assignment) const {
    if (!pattern) return false;
    if (pattern->get_type() == ASTNode::Type::OBJECT_LITERAL) {
        for (const auto& prop : static_cast<const ObjectLiteral*>(pattern)->get_properties()) {
            if (!prop->value) return false;
            if (prop->type != ObjectLiteral::PropertyType::Value) return false;
            if (!prop->key) {
                // `{...rest}`: a name this chunk can write, or -- in an
                // assignment -- any member reference the store path resolves.
                // The rest element is always last.
                if (prop->value->get_type() != ASTNode::Type::SPREAD_ELEMENT) return false;
                const ASTNode* rt = static_cast<const SpreadElement*>(prop->value.get())->get_argument();
                if (!rt) return false;
                if (is_assignment && rt->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
                    const auto* m = static_cast<const MemberExpression*>(rt);
                    if (member_is_super(m)) {
                        if (!super_member_emittable(m)) return false;
                    } else if (!member_is_private(m) && !member_is_supported(m)) {
                        return false;
                    }
                    continue;
                }
                if (rt->get_type() != ASTNode::Type::IDENTIFIER) return false;
                const std::string& rn = static_cast<const Identifier*>(rt)->get_name();
                if (!is_assignment && !script_mode_ && !env_names_.count(rn) &&
                    lookup_local(rn) < 0) return false;
                if (is_assignment && !pattern_target_is_writable(rn)) return false;
                continue;
            }
            if (!prop->computed && prop->key->get_type() != ASTNode::Type::IDENTIFIER &&
                prop->key->get_type() != ASTNode::Type::STRING_LITERAL &&
                prop->key->get_type() != ASTNode::Type::NUMBER_LITERAL) return false;
            const ASTNode* target = prop->value.get();
            if (target->get_type() == ASTNode::Type::ASSIGNMENT_EXPRESSION) {
                target = static_cast<const AssignmentExpression*>(target)->get_left();
            }
            if (is_assignment && target->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
                const auto* m = static_cast<const MemberExpression*>(target);
                if (member_is_super(m)) {
                    if (!super_member_emittable(m)) return false;
                } else if (!member_is_private(m) && !member_is_supported(m)) {
                    return false;
                }
                // A yield/await inside the target's own expression suspends in
                // the middle of the pattern, between the reference being
                // resolved and the element being read. The tree-walker keeps
                // that shape.
                // A suspension inside the target's own expression parks the
                // pattern between resolving the reference and reading the
                // element; only a member target's does, since a plain name has
                // no expression of its own.
                (void)0;  // B: guard off
                continue;
            }
            if (target->get_type() == ASTNode::Type::IDENTIFIER) {
                // A declaration writes a name this chunk owns; an assignment can
                // also name something further out, which a chain write reaches.
                const std::string& n = static_cast<const Identifier*>(target)->get_name();
                if (!is_assignment && !script_mode_ && !env_names_.count(n) &&
                    lookup_local(n) < 0) return false;
                if (is_assignment && !pattern_target_is_writable(n)) return false;
                continue;
            }
            if (!pattern_is_emittable(target, is_lexical, is_assignment)) return false;
        }
        return true;
    }
    if (pattern->get_type() == ASTNode::Type::ARRAY_LITERAL) {
        for (const auto& el : static_cast<const ArrayLiteral*>(pattern)->get_elements()) {
            // An elision is either a null slot or an UNDEFINED_LITERAL, the
            // same two spellings array literals use for a hole.
            if (!el || el->get_type() == ASTNode::Type::UNDEFINED_LITERAL) continue;
            const ASTNode* target = el.get();
            if (target->get_type() == ASTNode::Type::SPREAD_ELEMENT) {
                target = static_cast<const SpreadElement*>(target)->get_argument();
            }
            if (target->get_type() == ASTNode::Type::ASSIGNMENT_EXPRESSION) {
                target = static_cast<const AssignmentExpression*>(target)->get_left();
            }
            if (is_assignment && target->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
                const auto* m = static_cast<const MemberExpression*>(target);
                if (member_is_super(m)) {
                    if (!super_member_emittable(m)) return false;
                } else if (!member_is_private(m) && !member_is_supported(m)) {
                    return false;
                }
                // A yield/await inside the target's own expression suspends in
                // the middle of the pattern, between the reference being
                // resolved and the element being read. The tree-walker keeps
                // that shape.
                // A suspension inside the target's own expression parks the
                // pattern between resolving the reference and reading the
                // element; only a member target's does, since a plain name has
                // no expression of its own.
                (void)0;  // B: guard off
                continue;
            }
            if (target->get_type() == ASTNode::Type::IDENTIFIER) {
                const std::string& n = static_cast<const Identifier*>(target)->get_name();
                if (!is_assignment && !script_mode_ && !env_names_.count(n) &&
                    lookup_local(n) < 0) return false;
                if (is_assignment && !pattern_target_is_writable(n)) return false;
                continue;
            }
            if (!pattern_is_emittable(target, is_lexical, is_assignment)) return false;
        }
        return true;
    }
    return false;
}

// A tag's arguments: the template object first, then each substitution. The
// object is a compile-time constant because the site has exactly one, however
// often it runs.
bool BytecodeCompiler::emit_tagged_template_args(const CallExpression* call,
                                                 int& args_start, uint8_t& argc) {
    const auto& args = call->get_arguments();
    if (args.size() != 1 || args[0]->get_type() != ASTNode::Type::TEMPLATE_LITERAL) return false;
    auto* tmpl = const_cast<TemplateLiteral*>(static_cast<const TemplateLiteral*>(args[0].get()));
    Value tmpl_obj = get_template_object(tmpl);
    if (!tmpl_obj.is_object()) return false;

    args_start = next_register_;
    int reg = alloc_temp();
    if (failed_) return false;
    emit_load_const(tmpl_obj);
    emit(Op::Star);
    emit_u8(static_cast<uint8_t>(reg));
    size_t count = 1;
    for (const auto& el : tmpl->get_elements()) {
        if (el.type != TemplateLiteral::Element::Type::EXPRESSION) continue;
        int r = alloc_temp();
        if (failed_) return false;
        if (!compile_expression(el.expression.get())) return false;
        emit(Op::Star);
        emit_u8(static_cast<uint8_t>(r));
        count++;
    }
    if (count > 255) return false;
    argc = static_cast<uint8_t>(count);
    return !failed_;
}

// Annex B B.3.4: a bare function declaration as an if clause or a label's
// body. The name is var-scoped -- declared with the enclosing function's vars
// and initialized to undefined, then given the function object when the
// declaration is reached. Refusing it handed whole functions to the
// tree-walker, which binds the name only where the declaration runs.
// The spec wraps a few statements in UpdateEmpty(_, undefined), so they answer
// undefined rather than letting whatever ran before them stand: an if with an
// untaken branch, a loop that never runs its body, a switch that matches no
// clause, a try whose block completes empty.
// Whether `name` is a block-scoped local whose scope is not open here. Only
// asked where being out of scope is not an error -- `typeof` -- so a name this
// cannot decide about is treated as in scope.
bool BytecodeCompiler::lexical_out_of_scope(const std::string& name) const {
    if (!block_scoped_names_.count(name)) return false;
    for (const auto& scope : lexical_scopes_) {
        for (const auto& n : scope) if (n == name) return false;
    }
    return true;
}

void BytecodeCompiler::emit_completion_reset() {
    if (completion_reg_ < 0) return;
    emit(Op::LdaUndefined);
    emit(Op::Star);
    emit_u8(static_cast<uint8_t>(completion_reg_));
}

// A statement that completed with a value in the accumulator.
void BytecodeCompiler::emit_completion_store() {
    if (completion_reg_ < 0) return;
    emit(Op::Star);
    emit_u8(static_cast<uint8_t>(completion_reg_));
}

bool BytecodeCompiler::compile_if_branch(const ASTNode* branch) {
    if (!branch || branch->get_type() != ASTNode::Type::FUNCTION_DECLARATION) {
        return compile_statement(branch);
    }
    if (!env_mode_) return false;
    if (chunk_->ensure_closures().size() >= 0xFFFF) return false;
    const Identifier* id = static_cast<const FunctionDeclaration*>(branch)->get_id();
    if (!id || id->get_name().empty()) return false;
    const std::string& name = id->get_name();
    if (annexb_fn_vars_.count(name)) {
        chunk_->ensure_closures().push_back(closure_template_for(branch));
        emit(Op::CreateClosure);
        emit_u16(static_cast<uint16_t>(chunk_->ensure_closures().size() - 1));
        emit_write_local(name, /*is_declaration=*/false);
        return !failed_;
    }
    // No var binding to write: a lexical or a parameter holds the name out
    // here, so the function is confined to the clause's own scope.
    record_env_slot_info({}, env_depth_ + 1);
    chunk_->ensure_env().loop_envs.push_back({});
    loop_env_needs_fresh_.push_back(true);
    emit(Op::EnterLoopEnv);
    emit_u16(static_cast<uint16_t>(chunk_->ensure_env().loop_envs.size() - 1));
    env_depth_++;
    hoisted_fn_decls_.insert(branch);
    chunk_->ensure_closures().push_back(closure_template_for(branch));
    emit(Op::DeclareFunction);
    emit_u16(static_cast<uint16_t>(chunk_->ensure_closures().size() - 1));
    emit(Op::ExitLoopEnv);
    env_depth_--;
    return !failed_;
}

// A block whose statements include `using`: the dispose scope opens on entry
// and has to run on every way out -- falling off the end, an exception, a
// generator return(), and each of return/break/continue.
bool BytecodeCompiler::emit_dispose_scope_body(const ASTNode* suspend_scope,
                                               const std::function<bool()>& emit_body,
                                               FinallyScope& escaped) {
    const bool save_env = env_mode_;
    if (save_env) {
        if (++try_env_depth_ > 64) return false;
        emit(Op::SaveEnv);
    }
    emit(Op::PushDisposeScope);

    FinallyScope scope;
    scope.cleanup = FinallyScope::Cleanup::Dispose;
    scope.save_env = save_env;
    scope.loop_depth = loop_stack_.size();
    finally_stack_.push_back(std::move(scope));

    size_t body_start = code_.size();
    bool body_ok = true;
    dispose_scope_depth_++;
    body_ok = emit_body();
    dispose_scope_depth_--;
    size_t body_end = code_.size();
    escaped = std::move(finally_stack_.back());
    finally_stack_.pop_back();
    if (!body_ok) return false;

    if (save_env) {
        emit(Op::PopEnvSave);
        try_env_depth_--;
    }
    emit(Op::DisposeScope);
    emit_u8(0);
    size_t jump_ok = emit_jump(Op::Jump);

    // Unwinding: dispose, then let the completion travel on.
    size_t cleanup_pc = code_.size();
    if (save_env) emit(Op::RestoreEnv);
    emit(Op::DisposeScope);
    emit_u8(1);  // the pending exception is in the accumulator
    const size_t handler_idx = chunk_->ensure_handlers().size();
    chunk_->ensure_handlers().push_back({static_cast<uint32_t>(body_start),
                                 static_cast<uint32_t>(body_end),
                                 static_cast<uint32_t>(cleanup_pc)});

    if (suspendable_ && contains_suspend(suspend_scope)) {
        size_t genreturn_pc = code_.size();
        if (save_env) emit(Op::RestoreEnv);
        int gr_temp = alloc_temp();
        if (failed_) return false;
        emit(Op::Star);
        emit_u8(static_cast<uint8_t>(gr_temp));
        emit(Op::DisposeScope);
        emit_u8(0);
        emit(Op::Ldar);
        emit_u8(static_cast<uint8_t>(gr_temp));
        free_temp(gr_temp);
        emit(Op::ReraiseGeneratorReturn);
        chunk_->ensure_handlers()[handler_idx].genreturn_pc = static_cast<int32_t>(genreturn_pc);
    }

    return patch_jump(jump_ok);
}

// IteratorClose for every for-of/for-in at or above `from`, innermost first.
// Mode 0 leaves the accumulator alone, so a value in flight survives.
void BytecodeCompiler::emit_iterator_closes_above(size_t from) {
    for (int i = static_cast<int>(loop_stack_.size()) - 1; i >= static_cast<int>(from); i--) {
        if (loop_stack_[i].iterator_reg < 0) continue;
        emit(loop_stack_[i].iterator_is_async ? Op::AsyncIteratorClose : Op::IteratorClose);
        emit_u8(static_cast<uint8_t>(loop_stack_[i].iterator_reg));
        emit_u8(0);
    }
}

// What a finally runs: a statement for `try/finally`, one instruction for a
// block that declared `using`.
bool BytecodeCompiler::emit_finally_body(const FinallyScope& scope) {
    switch (scope.cleanup) {
        case FinallyScope::Cleanup::RestoreOnly:
            return !failed_;
        case FinallyScope::Cleanup::Dispose:
            emit(Op::DisposeScope);
            emit_u8(0);
            return !failed_;
        case FinallyScope::Cleanup::Finally:
            break;
    }
    // 14.15.3 step 3: a finally that completes normally hands the result back
    // to the block's own completion, so nothing it evaluates may be recorded.
    const int saved_completion = completion_reg_;
    completion_reg_ = -1;
    const bool ok = compile_statement(scope.finally_node);
    completion_reg_ = saved_completion;
    return ok;
}

// The landing pads an escape out of a finally-bearing region needs. They go
// after the region, not inside it: within its own handler range a throw from
// this copy of the finally would reach that region's catch instead of leaving.
bool BytecodeCompiler::emit_finally_pads(FinallyScope& scope) {
    if (!scope.return_jumps.empty()) {
        size_t skip_pad = emit_jump(Op::Jump);
        for (size_t site : scope.return_jumps) {
            if (!patch_jump(site)) return false;
        }
        if (scope.save_env) emit(Op::RestoreEnv);
        if (!emit_finally_body(scope)) return false;
        emit(Op::Ldar);
        emit_u8(static_cast<uint8_t>(scope.value_reg));
        // Any finally further out takes it from here in turn, and allocates its
        // own parking register while doing so. Freeing this one now would rewind
        // past that register -- free_temp is a LIFO watermark -- and the outer
        // finally body would then reuse it and overwrite the value in flight.
        if (!emit_return_completion(/*has_argument=*/false, /*already_awaited=*/true)) return false;
        if (!patch_jump(skip_pad)) return false;
    }
    // One pad per break/continue target, each running the finally and then
    // performing the jump it was standing in for.
    for (auto& esc : scope.escapes) {
        size_t skip_pad = emit_jump(Op::Jump);
        for (size_t site : esc.sites) {
            if (!patch_jump(site)) return false;
        }
        if (scope.save_env) emit(Op::RestoreEnv);
        if (!emit_finally_body(scope)) return false;
        if (!emit_loop_escape(esc.is_continue, esc.label)) return false;
        if (!patch_jump(skip_pad)) return false;
    }
    return !failed_;
}

// A `break` or `continue`, with any finally between here and its target run
// first. The target decides that: one at or above the try's own loop depth is
// still inside the try, so nothing is being left.
bool BytecodeCompiler::emit_loop_escape(bool is_continue, const std::string& label) {
    if (loop_stack_.empty()) return false;
    int target = -1;
    if (label.empty()) {
        if (is_continue) {
            // A switch isn't a loop -- continue skips past it to the loop below.
            for (int i = static_cast<int>(loop_stack_.size()) - 1; i >= 0; i--) {
                if (!loop_stack_[i].is_switch) { target = i; break; }
            }
        } else {
            target = static_cast<int>(loop_stack_.size()) - 1;
        }
    } else {
        for (int i = static_cast<int>(loop_stack_.size()) - 1; i >= 0 && target < 0; i--) {
            for (const auto& l : loop_stack_[i].labels) {
                if (l == label) { target = i; break; }
            }
        }
        if (is_continue && target >= 0 && loop_stack_[target].is_switch) return false;  // not a loop
    }
    if (target < 0) return false;

    if (!finally_stack_.empty() &&
        static_cast<size_t>(target) < finally_stack_.back().loop_depth) {
        FinallyScope& fs = finally_stack_.back();
        emit_iterator_closes_above(fs.loop_depth);
        for (auto& esc : fs.escapes) {
            if (esc.is_continue == is_continue && esc.label == label) {
                esc.sites.push_back(emit_jump(Op::Jump));
                return !failed_;
            }
        }
        fs.escapes.push_back({is_continue, label, {emit_jump(Op::Jump)}});
        return !failed_;
    }

    // IteratorClose every for-of/for-in strictly inside the target (the target
    // keeps its own: a continue goes on iterating, a break has a landing pad).
    for (int i = static_cast<int>(loop_stack_.size()) - 1; i > target; i--) {
        if (loop_stack_[i].iterator_reg < 0) continue;
        emit(loop_stack_[i].iterator_is_async ? Op::AsyncIteratorClose : Op::IteratorClose);
        emit_u8(static_cast<uint8_t>(loop_stack_[i].iterator_reg));
        emit_u8(0);
    }
    // Unwind Environments/SaveEnv entries between here and the target loop.
    LoopScope& scope = loop_stack_[target];
    for (int i = env_depth_ - scope.base_env_depth; i > 0; i--) {
        emit(Op::ExitLoopEnv);
    }
    for (int i = try_env_depth_ - scope.base_try_depth; i > 0; i--) {
        emit(Op::PopEnvSave);
    }
    if (!is_continue) {
        scope.break_patches.push_back(emit_jump(Op::Jump));
        return !failed_;
    }
    if (scope.continue_is_forward) {
        scope.continue_patches.push_back(emit_jump(Op::Jump));
        return !failed_;
    }
    return emit_jump_back(Op::Jump, scope.continue_target) && !failed_;
}

// Everything a `return` does once its value is in the accumulator. A finally
// between here and the function's edge takes the value first and runs; only
// when none is left does the return actually happen.
bool BytecodeCompiler::emit_return_completion(bool has_argument, bool already_awaited) {
    // Op::SettleReturn does two things, and a finally in the way separates
    // them: the Await belongs to the return statement, the recording to the
    // completion that actually leaves.
    const uint8_t kAwait = 1, kRecord = 2;
    if (!finally_stack_.empty()) {
        if (suspendable_ && !already_awaited && has_argument) {
            emit(Op::SettleReturn);
            emit_u8(kAwait);
        }
        FinallyScope& fs = finally_stack_.back();
        // The for-of loops being left right here are gone from loop_stack_ by
        // the time the pad runs, so they are closed now; the ones outside the
        // finally are closed when the pad performs the rest of the return.
        emit_iterator_closes_above(fs.loop_depth);
        if (fs.value_reg < 0) {
            fs.value_reg = alloc_temp();
            if (failed_) return false;
        }
        emit(Op::Star);
        emit_u8(static_cast<uint8_t>(fs.value_reg));
        fs.return_jumps.push_back(emit_jump(Op::Jump));
        return !failed_;
    }
    if (suspendable_) {
        uint8_t bits = kRecord;
        if (!already_awaited && has_argument) bits |= kAwait;
        emit(Op::SettleReturn);
        emit_u8(bits);
    }
    // Return abruptly completes every enclosing for-of/for-in:
    // IteratorClose innermost-first (mode 0 leaves acc untouched).
    for (auto it = loop_stack_.rbegin(); it != loop_stack_.rend(); ++it) {
        if (it->iterator_reg < 0) continue;
        emit(it->iterator_is_async ? Op::AsyncIteratorClose : Op::IteratorClose);
        emit_u8(static_cast<uint8_t>(it->iterator_reg));
        emit_u8(0);
    }
    emit(Op::Return);
    return !failed_;
}

// `pattern = source` as an expression: the pattern writes through, and the
// expression itself answers with the source, which is why it is parked in a
// register rather than left to the binder's own bookkeeping.
bool BytecodeCompiler::emit_pattern_assign(const ASTNode* pattern, const ASTNode* source) {
    if (!compile_expression(source)) return false;
    int src_reg = alloc_temp();
    if (failed_) return false;
    emit(Op::Star);
    emit_u8(static_cast<uint8_t>(src_reg));
    if (!emit_pattern_bind(pattern, /*is_lexical=*/false, /*is_const=*/false,
                           /*is_assignment=*/true)) {
        return false;
    }
    emit(Op::Ldar);
    emit_u8(static_cast<uint8_t>(src_reg));
    free_temp(src_reg);
    return !failed_;
}

// Whether an assignment target has to be bound to its reference before the
// right side runs. Inside a `with` that applies to every free name; for a
// `with` merely captured in the chain it applies to the names this chunk does
// not own, since the object sits outside the body's own scope.
bool BytecodeCompiler::needs_with_target_resolve(const ASTNode* target, bool is_lexical) const {
    if (is_lexical || !target || target->get_type() != ASTNode::Type::IDENTIFIER) return false;
    if (with_depth_ > 0) return true;
    const std::string& n = static_cast<const Identifier*>(target)->get_name();
    return outer_with_ && (!is_local(n) || lexical_out_of_scope(n));
}

// A pattern target under a `with` is a reference the object has to be asked
// about before the element is read, and the answer decides where the store
// lands (spec ResolveBinding runs ahead of GetV). A lexical target is declared
// inside the with, where no object stands in front of it.
int BytecodeCompiler::emit_with_target_resolve(const ASTNode* target, bool is_lexical) {
    if (!needs_with_target_resolve(target, is_lexical)) return -1;
    emit(Op::ResolveWithTarget);
    emit_u16(add_name(static_cast<const Identifier*>(target)->get_name()));
    int reg = alloc_temp();
    if (failed_) return -1;
    emit(Op::Star);
    emit_u8(static_cast<uint8_t>(reg));
    return reg;
}

// The object and key of a member target, evaluated before the source is read
// so the store that follows has nothing left to compute (spec PutValue takes
// the reference first). A super target resolves its base from [[HomeObject]]
// instead, which is the same reference in a different shape.
int BytecodeCompiler::emit_member_target_resolve(const ASTNode* target, bool is_assignment,
                                                 int& key_reg) {
    key_reg = -1;
    if (!is_assignment || !target || target->get_type() != ASTNode::Type::MEMBER_EXPRESSION) return -1;
    const auto* mem = static_cast<const MemberExpression*>(target);
    const bool super = member_is_super(mem);
    if (super) {
        if (!super_member_emittable(mem)) return -1;
    } else if (!member_is_private(mem) && !member_is_supported(mem)) {
        return -1;
    }
    int obj_reg = -1;
    if (super) {
        obj_reg = alloc_temp();
        if (failed_) return -1;
        emit(Op::ResolveSuperBase);
        emit_u8(static_cast<uint8_t>(obj_reg));
    } else {
        if (!compile_expression(mem->get_object())) { failed_ = true; return -1; }
        obj_reg = alloc_temp();
        if (failed_) return -1;
        emit(Op::Star);
        emit_u8(static_cast<uint8_t>(obj_reg));
    }
    if (mem->is_computed()) {
        if (!compile_expression(mem->get_property())) { failed_ = true; return -1; }
        key_reg = alloc_temp();
        if (failed_) return -1;
        emit(Op::Star);
        emit_u8(static_cast<uint8_t>(key_reg));
    }
    return obj_reg;
}

// Where a pattern element's value lands. A declaration always writes a name
// this chunk owns; an assignment can also name something further out, and only
// a chain write reaches that.
bool BytecodeCompiler::emit_pattern_target_store(const ASTNode* target, bool is_lexical,
                                                 bool is_const, bool is_assignment,
                                                 int with_target_reg, int member_obj_reg,
                                                 int member_key_reg) {
    // The value is in the accumulator, which is where SetNamed/SetKeyed want
    // it; the reference was resolved before the source was read.
    if (member_obj_reg >= 0) {
        const auto* mem = static_cast<const MemberExpression*>(target);
        if (member_is_super(mem)) {
            if (member_key_reg >= 0) {
                emit(Op::SetSuperKeyed);
                emit_u8(static_cast<uint8_t>(member_obj_reg));
                emit_u8(static_cast<uint8_t>(member_key_reg));
            } else {
                emit(Op::SetSuper);
                emit_u8(static_cast<uint8_t>(member_obj_reg));
                emit_u16(add_name(static_cast<const Identifier*>(mem->get_property())->get_name()));
            }
        } else if (member_key_reg >= 0) {
            emit(Op::SetKeyed);
            emit_u8(static_cast<uint8_t>(member_obj_reg));
            emit_u8(static_cast<uint8_t>(member_key_reg));
            emit_u16(alloc_keyed_feedback());
        } else {
            const bool priv = member_is_private(mem);
            emit(priv ? Op::SetPrivate : Op::SetNamed);
            emit_u8(static_cast<uint8_t>(member_obj_reg));
            emit_u16(add_name(static_cast<const Identifier*>(mem->get_property())->get_name()));
            emit_u16(priv ? alloc_private_feedback() : alloc_feedback_slot());
        }
        return !failed_;
    }
    if (target->get_type() == ASTNode::Type::IDENTIFIER) {
        const std::string& name = static_cast<const Identifier*>(target)->get_name();
        if (with_target_reg >= 0) {
            emit(Op::StaWithResolved);
            emit_u8(static_cast<uint8_t>(with_target_reg));
            emit_u16(add_name(name));
            return !failed_;
        }
        if (!env_names_.count(name) && lookup_local(name) < 0) {
            if (is_assignment || !script_mode_) {
                emit(Op::StaLookup);
                emit_u16(add_name(name));
                return !failed_;
            }
            // Top-level declaration: the binding pre-exists -- a var on the
            // global object, a let/const uninitialized in the script env.
            emit(is_lexical ? Op::StaEnvInit : Op::StaLookup);
            emit_u16(add_name(name));
            return !failed_;
        }
        emit_write_local(name, is_assignment ? false : is_lexical);
        return !failed_;
    }
    return emit_pattern_bind(target, is_lexical, is_const, is_assignment);
}

// Object pattern: read each property off the source, apply the element's own
// default when it comes back undefined, then bind or recurse.
bool BytecodeCompiler::emit_pattern_bind(const ASTNode* pattern, bool is_lexical, bool is_const, bool is_assignment) {
    (void)is_const;
    if (pattern->get_type() == ASTNode::Type::ARRAY_LITERAL) {
        return emit_array_pattern_bind(pattern, is_lexical, is_const, is_assignment);
    }
    int src_reg = alloc_temp();
    if (failed_) return false;
    emit(Op::Star);
    emit_u8(static_cast<uint8_t>(src_reg));
    // Destructuring null/undefined is a TypeError, and it has to be raised
    // before any property read.
    emit(Op::CheckObjectCoercible);

    const auto& props = static_cast<const ObjectLiteral*>(pattern)->get_properties();
    bool has_rest = false;
    for (const auto& prop : props) if (!prop->key) has_rest = true;

    // A `...rest` needs the keys the pattern already took, including any
    // computed ones, so they are accumulated into an array as we go.
    int keys_reg = -1, keys_idx_reg = -1, keys_hold_reg = -1;
    if (has_rest) {
        emit(Op::CreateArray);
        emit_u16(0);
        keys_reg = alloc_temp();
        if (failed_) return false;
        emit(Op::Star);
        emit_u8(static_cast<uint8_t>(keys_reg));
        keys_idx_reg = alloc_temp();
        if (failed_) return false;
        emit(Op::LdaZero);
        emit(Op::Star);
        emit_u8(static_cast<uint8_t>(keys_idx_reg));
        keys_hold_reg = alloc_temp();
        if (failed_) return false;
    }
    auto record_key = [&]() {  // key in acc; leaves it there
        if (!has_rest) return;
        // Bumping the counter ends in Star, which lands in the accumulator and
        // would take the key's place -- the caller reads it from there for the
        // GetKeyed that follows. Park the key first and put it back after.
        emit(Op::Star);
        emit_u8(static_cast<uint8_t>(keys_hold_reg));
        emit(Op::DefineElement);
        emit_u8(static_cast<uint8_t>(keys_reg));
        emit_u8(static_cast<uint8_t>(keys_idx_reg));
        emit(Op::Ldar);
        emit_u8(static_cast<uint8_t>(keys_idx_reg));
        emit(Op::Inc);
        emit(Op::Star);
        emit_u8(static_cast<uint8_t>(keys_idx_reg));
        emit(Op::Ldar);
        emit_u8(static_cast<uint8_t>(keys_hold_reg));
    };

    for (const auto& prop : props) {
        if (!prop->key) {  // `...rest`
            const ASTNode* rt = static_cast<const SpreadElement*>(prop->value.get())->get_argument();
            const int rest_with = emit_with_target_resolve(rt, is_lexical);
            int rest_key = -1;
            const int rest_obj = emit_member_target_resolve(rt, is_assignment, rest_key);
            if (failed_) return false;
            emit(Op::CopyRestProperties);
            emit_u8(static_cast<uint8_t>(src_reg));
            emit_u8(static_cast<uint8_t>(keys_reg));
            if (!emit_pattern_target_store(rt, is_lexical, is_const, is_assignment, rest_with,
                                           rest_obj, rest_key)) {
                return false;
            }
            if (rest_key >= 0) free_temp(rest_key);
            if (rest_obj >= 0) free_temp(rest_obj);
            if (rest_with >= 0) free_temp(rest_with);
            continue;
        }
        const ASTNode* target = prop->value.get();
        const ASTNode* default_expr = nullptr;
        if (target->get_type() == ASTNode::Type::ASSIGNMENT_EXPRESSION) {
            const auto* ae = static_cast<const AssignmentExpression*>(target);
            default_expr = ae->get_right();
            target = ae->get_left();
        }

        int with_reg = -1;
        int member_obj = -1;
        int member_key = -1;
        int key_hold = -1;
        if (prop->computed) {
            if (!compile_expression(prop->key.get())) return false;
            emit(Op::ToPropertyKey);
            record_key();
            const bool resolve_target_first =
                needs_with_target_resolve(target, is_lexical) ||
                (is_assignment && target->get_type() == ASTNode::Type::MEMBER_EXPRESSION);
            if (resolve_target_first) {
                // The key is computed and has to survive the target's own
                // evaluation, so it waits in a register. It is released with
                // the target's registers at the end of the element rather than
                // here: freeing it first would hand its number back while the
                // registers allocated after it are still live.
                key_hold = alloc_temp();
                if (failed_) return false;
                emit(Op::Star);
                emit_u8(static_cast<uint8_t>(key_hold));
                with_reg = emit_with_target_resolve(target, is_lexical);
                member_obj = emit_member_target_resolve(target, is_assignment, member_key);
                if (failed_) return false;
                emit(Op::Ldar);
                emit_u8(static_cast<uint8_t>(key_hold));
            }
            emit(Op::GetKeyed);
            emit_u8(static_cast<uint8_t>(src_reg));
            emit_u16(alloc_keyed_feedback());
        } else {
            std::string key;
            if (prop->key->get_type() == ASTNode::Type::IDENTIFIER) {
                key = static_cast<const Identifier*>(prop->key.get())->get_name();
            } else if (prop->key->get_type() == ASTNode::Type::STRING_LITERAL) {
                key = static_cast<const StringLiteral*>(prop->key.get())->get_value();
            } else {
                // Numeric keys use the canonical property-key spelling.
                key = Value(static_cast<const NumberLiteral*>(prop->key.get())->get_value())
                          .to_property_key();
            }
            if (has_rest) {
                emit_load_const(Value(key));
                record_key();
            }
            with_reg = emit_with_target_resolve(target, is_lexical);
            member_obj = emit_member_target_resolve(target, is_assignment, member_key);
            if (failed_) return false;
            emit(Op::GetNamed);
            emit_u8(static_cast<uint8_t>(src_reg));
            emit_u16(add_name(key));
            emit_u16(alloc_feedback_slot());
        }

        if (default_expr) {
            size_t skip = emit_jump(Op::JumpIfNotUndefined);
            if (target->get_type() == ASTNode::Type::IDENTIFIER) {
                stamp_inferred_class_name(default_expr,
                                          static_cast<const Identifier*>(target)->get_name());
            }
            if (!compile_expression(default_expr)) return false;
            // NamedEvaluation: an anonymous function on the right of a
            // pattern default takes the name it is being bound to.
            if (target->get_type() == ASTNode::Type::IDENTIFIER &&
                is_named_evaluation_rhs(default_expr)) {
                emit(Op::SetFunctionNameIfUnnamed);
                emit_u16(add_name(static_cast<const Identifier*>(target)->get_name()));
            }
            if (!patch_jump(skip)) return false;
        }

        if (!emit_pattern_target_store(target, is_lexical, is_const, is_assignment, with_reg,
                                       member_obj, member_key)) {
            return false;
        }
        if (member_key >= 0) free_temp(member_key);
        if (member_obj >= 0) free_temp(member_obj);
        if (with_reg >= 0) free_temp(with_reg);
        if (key_hold >= 0) free_temp(key_hold);
    }
    if (has_rest) { free_temp(keys_idx_reg); free_temp(keys_reg); }
    free_temp(src_reg);
    return !failed_;
}

// Array pattern: the iterator protocol, driven one step per element. A `done`
// register keeps a spent iterator from being stepped again -- once it reports
// done, every remaining element binds undefined (and so gets its own default)
// without another next() call. The whole pattern sits inside a handler that
// closes the iterator if anything in it throws, mirroring what
// compile_for_each_loop does for a for-of body.
bool BytecodeCompiler::emit_array_pattern_bind(const ASTNode* pattern, bool is_lexical, bool is_const, bool is_assignment) {
    int next_fn_reg = alloc_temp();
    if (failed_) return false;
    emit(Op::GetIterator);
    emit_u8(static_cast<uint8_t>(next_fn_reg));
    int iter_reg = alloc_temp();
    if (failed_) return false;
    emit(Op::Star);
    emit_u8(static_cast<uint8_t>(iter_reg));

    int done_reg = alloc_temp();
    if (failed_) return false;
    emit(Op::LdaFalse);
    emit(Op::Star);
    emit_u8(static_cast<uint8_t>(done_reg));

    // Only the spans that must trigger IteratorClose are guarded. A throw out
    // of next() itself (or out of the result's `value` getter) leaves
    // [[Done]] true per spec, and must NOT close -- so the step sequences stay
    // outside every guarded range.
    std::vector<std::pair<size_t, size_t>> guarded;
    // Spans that run after the iterator has been drained: the spec closes it
    // only while it is not done, so a throw out of these just propagates.
    std::vector<std::pair<size_t, size_t>> drained;

    // Leaves the next value in the accumulator, or undefined once exhausted.
    auto step = [&]() -> bool {
        emit(Op::Ldar);
        emit_u8(static_cast<uint8_t>(done_reg));
        size_t to_undef = emit_jump(Op::JumpIfTrue);
        emit(Op::IteratorNextOrJump);
        emit_u8(static_cast<uint8_t>(iter_reg));
        emit_u8(static_cast<uint8_t>(next_fn_reg));
        size_t to_set_done = code_.size();
        emit_u16(0);
        size_t to_have = emit_jump(Op::Jump);
        if (!patch_jump(to_set_done)) return false;
        emit(Op::LdaTrue);
        emit(Op::Star);
        emit_u8(static_cast<uint8_t>(done_reg));
        if (!patch_jump(to_undef)) return false;
        emit(Op::LdaUndefined);
        return patch_jump(to_have);
    };

    const auto& elements = static_cast<const ArrayLiteral*>(pattern)->get_elements();
    for (size_t i = 0; i < elements.size(); ++i) {
        const ASTNode* el = elements[i].get();
        if (!el || el->get_type() == ASTNode::Type::UNDEFINED_LITERAL) {
            // Elision: advance the iterator, bind nothing.
            if (!step()) return false;
            continue;
        }

        if (el->get_type() == ASTNode::Type::SPREAD_ELEMENT) {
            const ASTNode* rest_target = static_cast<const SpreadElement*>(el)->get_argument();
            const size_t rest_ref_span = code_.size();
            const int rest_with = emit_with_target_resolve(rest_target, is_lexical);
            int rest_key = -1;
            const int rest_obj = emit_member_target_resolve(rest_target, is_assignment, rest_key);
            if (failed_) return false;
            // The target's own reference throwing closes the iterator too, so
            // it is guarded like the store is -- separately, because what runs
            // between them (the iterator being drained) must not be.
            if (code_.size() > rest_ref_span) guarded.emplace_back(rest_ref_span, code_.size());
            emit(Op::CreateArray);
            emit_u16(0);
            int arr_reg = alloc_temp();
            if (failed_) return false;
            emit(Op::Star);
            emit_u8(static_cast<uint8_t>(arr_reg));
            int idx_reg = alloc_temp();
            if (failed_) return false;
            emit(Op::LdaZero);
            emit(Op::Star);
            emit_u8(static_cast<uint8_t>(idx_reg));

            emit(Op::Ldar);
            emit_u8(static_cast<uint8_t>(done_reg));
            size_t rest_skip = emit_jump(Op::JumpIfTrue);
            size_t loop_top = code_.size();
            emit(Op::IteratorNextOrJump);
            emit_u8(static_cast<uint8_t>(iter_reg));
            emit_u8(static_cast<uint8_t>(next_fn_reg));
            size_t rest_exit = code_.size();
            emit_u16(0);
            emit(Op::DefineElement);
            emit_u8(static_cast<uint8_t>(arr_reg));
            emit_u8(static_cast<uint8_t>(idx_reg));
            emit(Op::Ldar);
            emit_u8(static_cast<uint8_t>(idx_reg));
            emit(Op::Inc);
            emit(Op::Star);
            emit_u8(static_cast<uint8_t>(idx_reg));
            emit_jump_back(Op::Jump, loop_top);
            if (!patch_jump(rest_exit)) return false;
            // A rest element always drains the iterator, so nothing may step it again.
            emit(Op::LdaTrue);
            emit(Op::Star);
            emit_u8(static_cast<uint8_t>(done_reg));
            if (!patch_jump(rest_skip)) return false;

            emit(Op::Ldar);
            emit_u8(static_cast<uint8_t>(arr_reg));
            size_t rest_span = code_.size();
            if (rest_target->get_type() == ASTNode::Type::IDENTIFIER || rest_obj >= 0) {
                if (!emit_pattern_target_store(rest_target, is_lexical, is_const, is_assignment,
                                               rest_with, rest_obj, rest_key)) {
                    return false;
                }
            } else if (!emit_pattern_bind(rest_target, is_lexical, is_const, is_assignment)) {
                return false;
            }
            if (rest_key >= 0) free_temp(rest_key);
            if (rest_obj >= 0) free_temp(rest_obj);
            if (rest_with >= 0) free_temp(rest_with);
            if (code_.size() > rest_span) drained.emplace_back(rest_span, code_.size());
            free_temp(idx_reg);
            free_temp(arr_reg);
            break;  // a rest element is always last
        }

        const ASTNode* target = el;
        const ASTNode* default_expr = nullptr;
        if (target->get_type() == ASTNode::Type::ASSIGNMENT_EXPRESSION) {
            const auto* ae = static_cast<const AssignmentExpression*>(target);
            default_expr = ae->get_right();
            target = ae->get_left();
        }

        const size_t ref_span = code_.size();
        const int with_reg = emit_with_target_resolve(target, is_lexical);
        int member_key = -1;
        const int member_obj = emit_member_target_resolve(target, is_assignment, member_key);
        if (failed_) return false;
        if (code_.size() > ref_span) guarded.emplace_back(ref_span, code_.size());
        if (!step()) return false;
        size_t span_start = code_.size();
        if (default_expr) {
            size_t skip = emit_jump(Op::JumpIfNotUndefined);
            if (target->get_type() == ASTNode::Type::IDENTIFIER) {
                stamp_inferred_class_name(default_expr,
                                          static_cast<const Identifier*>(target)->get_name());
            }
            if (!compile_expression(default_expr)) return false;
            // NamedEvaluation: an anonymous function on the right of a
            // pattern default takes the name it is being bound to.
            if (target->get_type() == ASTNode::Type::IDENTIFIER &&
                is_named_evaluation_rhs(default_expr)) {
                emit(Op::SetFunctionNameIfUnnamed);
                emit_u16(add_name(static_cast<const Identifier*>(target)->get_name()));
            }
            if (!patch_jump(skip)) return false;
        }
        if (!emit_pattern_target_store(target, is_lexical, is_const, is_assignment, with_reg,
                                       member_obj, member_key)) {
            return false;
        }
        if (member_key >= 0) free_temp(member_key);
        if (member_obj >= 0) free_temp(member_obj);
        if (with_reg >= 0) free_temp(with_reg);
        if (code_.size() > span_start) guarded.emplace_back(span_start, code_.size());
    }

    // Spec: close the iterator only when the pattern stopped before it was done.
    emit(Op::Ldar);
    emit_u8(static_cast<uint8_t>(done_reg));
    size_t skip_close = emit_jump(Op::JumpIfTrue);
    emit(Op::IteratorClose);
    emit_u8(static_cast<uint8_t>(iter_reg));
    emit_u8(0);
    if (!patch_jump(skip_close)) return false;

    size_t skip_cleanup = emit_jump(Op::Jump);
    size_t cleanup_pc = code_.size();
    emit(Op::IteratorClose);
    emit_u8(static_cast<uint8_t>(iter_reg));
    emit_u8(1);  // acc holds the pending exception: close, then re-raise
    std::vector<size_t> guarded_handlers;
    for (const auto& span : guarded) {
        guarded_handlers.push_back(chunk_->ensure_handlers().size());
        chunk_->ensure_handlers().push_back({static_cast<uint32_t>(span.first),
                                             static_cast<uint32_t>(span.second),
                                             static_cast<uint32_t>(cleanup_pc)});
    }
    // A `return()` on the generator resumes a suspension inside the pattern by
    // unwinding a C++ exception, which reaches run() rather than the handler
    // above, so it needs a landing pad of its own -- the iterator still has to
    // be closed before the completion goes on out. Same shape the for-of loop
    // uses. The drained spans get none: their iterator is already done.
    if (suspendable_ && !guarded_handlers.empty() && pattern_contains_suspension(pattern)) {
        size_t genreturn_pc = code_.size();
        int gr_temp = alloc_temp();
        if (failed_) return false;
        emit(Op::Star);
        emit_u8(static_cast<uint8_t>(gr_temp));
        emit(Op::IteratorClose);
        emit_u8(static_cast<uint8_t>(iter_reg));
        emit_u8(0);  // mode 0: validate, the completion is a return, not a throw
        emit(Op::Ldar);
        emit_u8(static_cast<uint8_t>(gr_temp));
        free_temp(gr_temp);
        emit(Op::ReraiseGeneratorReturn);
        for (size_t h : guarded_handlers) {
            chunk_->ensure_handlers()[h].genreturn_pc = static_cast<int32_t>(genreturn_pc);
        }
    }
    if (!drained.empty()) {
        size_t rethrow_pc = code_.size();
        emit(Op::Throw);  // acc holds the pending exception
        for (const auto& span : drained) {
            chunk_->ensure_handlers().push_back({static_cast<uint32_t>(span.first),
                                                 static_cast<uint32_t>(span.second),
                                                 static_cast<uint32_t>(rethrow_pc)});
        }
    }
    if (!patch_jump(skip_cleanup)) return false;

    free_temp(done_reg);
    free_temp(iter_reg);
    free_temp(next_fn_reg);
    return !failed_;
}

bool BytecodeCompiler::member_is_super(const MemberExpression* mem) {
    return mem->get_object()->get_type() == ASTNode::Type::IDENTIFIER &&
           static_cast<const Identifier*>(mem->get_object())->get_name() == "super";
}

// Whether the super opcodes can express this member at all. Checked before any
// emission so a refusal can still fall back to the tree-walker cleanly. The
// super bindings live on the context chain, so this needs a real Environment
// for the same reason the member guards refuse there.
bool BytecodeCompiler::super_member_emittable(const MemberExpression* mem) const {
    if (!env_mode_) return false;
    if (mem->is_computed()) return true;
    if (mem->get_property()->get_type() != ASTNode::Type::IDENTIFIER) return false;
    const std::string& name = static_cast<const Identifier*>(mem->get_property())->get_name();
    return name.empty() || name[0] != '#';
}

// Reads super.<name> / super[<expr>] into the accumulator.
bool BytecodeCompiler::emit_super_load(const MemberExpression* mem) {
    if (!mem->is_computed()) {
        emit(Op::GetSuper);
        emit_u16(add_name(static_cast<const Identifier*>(mem->get_property())->get_name()));
        return !failed_;
    }
    int base_reg = alloc_temp();
    if (failed_) return false;
    emit(Op::ResolveSuperBase);
    emit_u8(static_cast<uint8_t>(base_reg));
    if (!compile_expression(mem->get_property())) return false;
    emit(Op::GetSuperKeyed);
    emit_u8(static_cast<uint8_t>(base_reg));
    free_temp(base_reg);
    return !failed_;
}

bool BytecodeCompiler::member_is_supported(const MemberExpression* mem) const {
    // Callers that can emit super handle it before asking; the rest still delegate.
    if (member_is_super(mem)) return false;
    if (!mem->is_computed()) {
        if (mem->get_property()->get_type() != ASTNode::Type::IDENTIFIER) return false;
        const std::string& name = static_cast<const Identifier*>(mem->get_property())->get_name();
        if (!name.empty() && name[0] == '#') return false;  // private field: needs brand check
    }
    return true;
}

// Hands `node`'s entire evaluation to the tree-walker's own evaluate() --
// same mechanism as Op::CreateClosure, reused here for super/private-name
// forms the register compiler doesn't implement directly. Only valid in
// env_mode, which guarantees `this`/`__super__`/`__eval_private_names__`
// and any locals the delegated subtree captures resolve through a real
// Environment (env_mode is forced whenever uses_super_or_private matches).

int BytecodeCompiler::emit_spread_array(const std::vector<std::unique_ptr<ASTNode>>& elements) {
    // A hole contributes to `length` without creating an own element, which
    // the non-spread path expresses by pre-sizing via CreateArray. Once a
    // spread makes the index dynamic there is no way to say that, so refuse
    // the combination rather than silently dropping a trailing hole.
    for (const auto& el : elements) {
        if (!el || el->get_type() == ASTNode::Type::UNDEFINED_LITERAL) return -1;
    }

    emit(Op::CreateArray);
    emit_u16(0);
    int arr_reg = alloc_temp();
    if (failed_) return -1;
    emit(Op::Star);
    emit_u8(static_cast<uint8_t>(arr_reg));

    // Running write index: every element (spread-expanded or not) appends at
    // this position and bumps it, so a spread of unknown length shifts the
    // ones after it correctly.
    int idx_reg = alloc_temp();
    if (failed_) return -1;
    emit(Op::LdaZero);
    emit(Op::Star);
    emit_u8(static_cast<uint8_t>(idx_reg));

    // acc holds the value to append.
    auto append_acc_and_bump = [&]() {
        emit(Op::DefineElement);
        emit_u8(static_cast<uint8_t>(arr_reg));
        emit_u8(static_cast<uint8_t>(idx_reg));
        emit(Op::Ldar);
        emit_u8(static_cast<uint8_t>(idx_reg));
        emit(Op::Inc);
        emit(Op::Star);
        emit_u8(static_cast<uint8_t>(idx_reg));
    };

    for (const auto& el : elements) {
        if (el->get_type() != ASTNode::Type::SPREAD_ELEMENT) {
            if (!compile_expression(el.get())) return -1;
            append_acc_and_bump();
            continue;
        }

        // One opcode for the whole expansion rather than a bytecode iterator
        // loop: SpreadInto shares the tree-walker's own expansion helper,
        // which bulk-copies a plain Array instead of driving the iterator
        // protocol element by element.
        if (!compile_expression(static_cast<const SpreadElement*>(el.get())->get_argument())) return -1;
        emit(Op::SpreadInto);
        emit_u8(static_cast<uint8_t>(arr_reg));
        emit_u8(static_cast<uint8_t>(idx_reg));
    }

    free_temp(idx_reg);
    if (failed_) return -1;
    return arr_reg;
}

// &&= / ||= / ??=. The RHS (and the write) only runs when the old value
// fails the operator's test; the skip jump leaves the old value in the
// accumulator as the expression result, matching the tree-walker.
bool BytecodeCompiler::compile_logical_assignment(const AssignmentExpression* expr) {
    using AsOp = AssignmentExpression::Operator;
    Op skip_op = expr->get_operator() == AsOp::LOGICAL_AND_ASSIGN ? Op::JumpIfFalse
               : expr->get_operator() == AsOp::LOGICAL_OR_ASSIGN  ? Op::JumpIfTrue
               : Op::JumpIfNotNullish;

    if (expr->get_left()->get_type() == ASTNode::Type::IDENTIFIER) {
        const std::string& name = static_cast<const Identifier*>(expr->get_left())->get_name();
        if (!expr->is_lhs_paren()) stamp_inferred_class_name(expr->get_right(), name);
        // NamedEvaluation applies to the RHS of a logical assignment too
        // (spec 13.15.2 step 1.e.i): `f ??= function(){}` names it "f". The
        // opcode no-ops when the value already has a name.
        const bool name_rhs = is_named_evaluation_rhs(expr->get_right());
        // Inside a `with`, the reference is bound against the object once and
        // both the read and the write go through that binding -- the object
        // may shadow a name this chunk would otherwise own, and asking twice
        // would run the Proxy traps twice.
        if (int with_reg = emit_with_target_resolve(expr->get_left(), /*is_lexical=*/false);
            with_reg >= 0) {
            emit(Op::LdaWithResolved);
            emit_u8(static_cast<uint8_t>(with_reg));
            emit_u16(add_name(name));
            size_t skip = emit_jump(skip_op);
            if (!compile_expression(expr->get_right())) return false;
            if (name_rhs) {
                emit(Op::SetFunctionNameIfUnnamed);
                emit_u16(add_name(name));
            }
            emit(Op::StaWithResolved);
            emit_u8(static_cast<uint8_t>(with_reg));
            emit_u16(add_name(name));
            free_temp(with_reg);
            return patch_jump(skip) && !failed_;
        }
        if (failed_) return false;
        if (is_local(name) && !lexical_out_of_scope(name)) {
            emit_read_local(name);
            size_t skip = emit_jump(skip_op);
            if (!compile_expression(expr->get_right())) return false;
            if (name_rhs) {
                emit(Op::SetFunctionNameIfUnnamed);
                emit_u16(add_name(name));
            }
            emit_write_local(name, /*is_declaration=*/false);
            return patch_jump(skip) && !failed_;
        }
        emit(Op::LdaLookup);
        emit_u16(add_name(name));
        size_t skip = emit_jump(skip_op);
        if (!compile_expression(expr->get_right())) return false;
        if (name_rhs) {
            emit(Op::SetFunctionNameIfUnnamed);
            emit_u16(add_name(name));
        }
        emit(Op::StaLookup);
        emit_u16(add_name(name));
        return patch_jump(skip) && !failed_;
    }

    if (expr->get_left()->get_type() != ASTNode::Type::MEMBER_EXPRESSION) return false;
    // A member target inside a `with` still resolves its object through the
    // chain, which this path does not bind once the way an identifier needs.
    if (with_depth_ > 0) return false;
    const auto* mem = static_cast<const MemberExpression*>(expr->get_left());
    const bool priv = member_is_private(mem);
    if (member_is_super(mem)) {
        if (!super_member_emittable(mem)) return false;
        int base_reg = alloc_temp();
        if (failed_) return false;
        emit(Op::ResolveSuperBase);
        emit_u8(static_cast<uint8_t>(base_reg));

        uint16_t sname = 0;
        int skey_reg = -1;
        if (mem->is_computed()) {
            if (!compile_expression(mem->get_property())) return false;
            skey_reg = alloc_temp();
            if (failed_) return false;
            // GetValue happens before the short-circuit test, so the key is
            // converted once here and reused by the store below.
            emit(Op::ToPropertyKey);
            emit(Op::Star);
            emit_u8(static_cast<uint8_t>(skey_reg));
            emit(Op::GetSuperKeyed);
            emit_u8(static_cast<uint8_t>(base_reg));
        } else {
            sname = add_name(static_cast<const Identifier*>(mem->get_property())->get_name());
            emit(Op::GetSuper);
            emit_u16(sname);
        }
        size_t skip = emit_jump(skip_op);
        if (!compile_expression(expr->get_right())) return false;
        if (skey_reg >= 0) {
            emit(Op::SetSuperKeyed);
            emit_u8(static_cast<uint8_t>(base_reg));
            emit_u8(static_cast<uint8_t>(skey_reg));
            free_temp(skey_reg);
        } else {
            emit(Op::SetSuper);
            emit_u8(static_cast<uint8_t>(base_reg));
            emit_u16(sname);
        }
        free_temp(base_reg);
        return patch_jump(skip) && !failed_;
    }
    if (!priv && !member_is_supported(mem)) return false;
    if (chain_contains_optional(mem)) return false;

    if (!compile_expression(mem->get_object())) return false;
    int obj_reg = alloc_temp();
    if (failed_) return false;
    emit(Op::Star);
    emit_u8(static_cast<uint8_t>(obj_reg));

    if (!mem->is_computed()) {
        uint16_t name_idx = add_name(
            static_cast<const Identifier*>(mem->get_property())->get_name());
        emit(priv ? Op::GetPrivate : Op::GetNamed);
        emit_u8(static_cast<uint8_t>(obj_reg));
        emit_u16(name_idx);
        emit_u16(priv ? alloc_private_feedback() : alloc_feedback_slot());
        size_t skip = emit_jump(skip_op);
        if (!compile_expression(expr->get_right())) return false;
        emit(priv ? Op::SetPrivate : Op::SetNamed);
        emit_u8(static_cast<uint8_t>(obj_reg));
        emit_u16(name_idx);
        emit_u16(priv ? alloc_private_feedback() : alloc_feedback_slot());
        if (!patch_jump(skip)) return false;
        free_temp(obj_reg);
        return !failed_;
    }

    if (!compile_expression(mem->get_property())) return false;
    int key_reg = alloc_temp();
    if (failed_) return false;
    emit(Op::Star);
    emit_u8(static_cast<uint8_t>(key_reg));
    // Spec: CheckObjectCoercible(base) before ToPropertyKey(key) for the
    // GetValue this logical-assignment form performs before its RHS.
    emit(Op::Ldar);
    emit_u8(static_cast<uint8_t>(obj_reg));
    emit(Op::CheckObjectCoercible);
    emit(Op::Ldar);
    emit_u8(static_cast<uint8_t>(key_reg));
    emit(Op::ToPropertyKey);  // once; GetKeyed/SetKeyed below reuse the string
    emit(Op::Star);
    emit_u8(static_cast<uint8_t>(key_reg));
    emit(Op::Ldar);
    emit_u8(static_cast<uint8_t>(key_reg));
    emit(Op::GetKeyed);  // key still in the accumulator after Star
    emit_u8(static_cast<uint8_t>(obj_reg));
    emit_u16(alloc_keyed_feedback());
    size_t skip = emit_jump(skip_op);
    if (!compile_expression(expr->get_right())) return false;
    emit(Op::SetKeyed);
    emit_u8(static_cast<uint8_t>(obj_reg));
    emit_u8(static_cast<uint8_t>(key_reg));
    emit_u16(alloc_keyed_feedback());
    if (!patch_jump(skip)) return false;
    free_temp(key_reg);
    free_temp(obj_reg);
    return !failed_;
}

size_t BytecodeCompiler::emit_jump(Op op) {
    emit(op);
    size_t pos = code_.size();
    emit_u16(0);
    return pos;
}

bool BytecodeCompiler::patch_jump(size_t operand_pos) {
    // Offset is relative to the pc after the 2-byte operand.
    ptrdiff_t offset = static_cast<ptrdiff_t>(code_.size()) -
                       static_cast<ptrdiff_t>(operand_pos + 2);
    if (offset < INT16_MIN || offset > INT16_MAX) { failed_ = true; return false; }
    uint16_t enc = static_cast<uint16_t>(static_cast<int16_t>(offset));
    code_[operand_pos] = static_cast<uint8_t>(enc & 0xFF);
    code_[operand_pos + 1] = static_cast<uint8_t>(enc >> 8);
    return true;
}

bool BytecodeCompiler::emit_jump_back(Op op, size_t target_pc) {
    emit(op);
    ptrdiff_t offset = static_cast<ptrdiff_t>(target_pc) -
                       static_cast<ptrdiff_t>(code_.size() + 2);
    if (offset < INT16_MIN || offset > INT16_MAX) { failed_ = true; return false; }
    emit_u16(static_cast<uint16_t>(static_cast<int16_t>(offset)));
    return true;
}

bool BytecodeCompiler::compile_statement(const ASTNode* node) {
    if (!node || failed_) return false;
    switch (node->get_type()) {
        case ASTNode::Type::EMPTY_STATEMENT:
            return true;

        // Already created and bound by the hoisting pass in compile();
        // block-nested declarations (Annex B) keep bailing to the tree-walker.
        case ASTNode::Type::FUNCTION_DECLARATION: {
            // Bound at the entry of a scope this compiler opened. Anything
            // else -- a switch case's, whose scope it does not open -- refuses.
            return hoisted_fn_decls_.count(node) > 0;
        }

        case ASTNode::Type::BLOCK_STATEMENT: {
            const auto* block = static_cast<const BlockStatement*>(node);
            // A block with its own direct let/const gets its own Environment
            // -- a nested block's own names are its own scope's concern.
            int block_env_idx = -1;
            lexical_scopes_.emplace_back();
            {
                std::vector<BytecodeChunk::LoopEnvVar> vars;
                bool needs_own_env = false;
                if (!collect_direct_lexical_decls(block, vars, needs_own_env)) return false;
                for (const auto& v : vars) {
                    lexical_scopes_.back().push_back(v.name);
                    block_scoped_names_.insert(v.name);
                }
                // A function declaration binds in the block -- BlockStatement's
                // own scope rule counts one -- so the compiled block needs the
                // same environment for Op::DeclareFunction to land in.
                for (const auto& st : block->get_statements()) {
                    if (st->get_type() == ASTNode::Type::FUNCTION_DECLARATION) {
                        needs_own_env = true;
                        break;
                    }
                }
                std::vector<BytecodeChunk::LoopEnvVar> env_vars;
                for (const auto& v : vars) {
                    int reg = env_names_.count(v.name) ? -1 : lookup_local(v.name);
                    if (reg >= 0) {
                        // Register-resident block lexical: re-arm its TDZ on
                        // (re-)entry so a loop iteration can't see the last one.
                        emit(Op::LdaTdz);
                        emit(Op::Star);
                        emit_u8(static_cast<uint8_t>(reg));
                    } else {
                        env_vars.push_back(v);
                    }
                }
                if (env_mode_ && (!env_vars.empty() || needs_own_env)) {
                    record_env_slot_info(env_vars, env_depth_ + 1);
                    chunk_->ensure_env().loop_envs.push_back(std::move(env_vars));
                    // A plain block never emits AdvanceLoopEnv (entered/exited
                    // once, no per-iteration refresh) -- this entry only needs
                    // to keep loop_env_needs_fresh_ the same size as
                    // chunk_'s loop_envs so OTHER indices into it stay valid.
                    loop_env_needs_fresh_.push_back(true);
                    block_env_idx = static_cast<int>(chunk_->ensure_env().loop_envs.size() - 1);
                    emit(Op::EnterLoopEnv);
                    emit_u16(static_cast<uint16_t>(block_env_idx));
                    env_depth_++;
                }
            }
            // Mirrors BlockStatement::evaluate's first pass: every function
            // declaration in the block is bound before any statement runs, so
            // a call ahead of the declaration finds it.
            for (const auto& st : block->get_statements()) {
                if (st->get_type() != ASTNode::Type::FUNCTION_DECLARATION) continue;
                if (!env_mode_) return false;
                if (chunk_->ensure_closures().size() >= 0xFFFF) return false;
                hoisted_fn_decls_.insert(st.get());
                chunk_->ensure_closures().push_back(closure_template_for(st.get()));
                emit(Op::DeclareFunction);
                emit_u16(static_cast<uint16_t>(chunk_->ensure_closures().size() - 1));
            }
            bool has_using = false;
            for (const auto& st : block->get_statements()) {
                if (st->get_type() == ASTNode::Type::USING_DECLARATION) { has_using = true; break; }
            }
            FinallyScope escaped;
            if (has_using) {
                if (!emit_dispose_scope_body(block, [&]() -> bool {
                        for (const auto& st : block->get_statements()) {
                            if (!compile_statement(st.get())) return false;
                        }
                        return true;
                    }, escaped)) return false;
            } else {
                for (const auto& stmt : block->get_statements()) {
                    if (!compile_statement(stmt.get())) return false;
                }
            }
            lexical_scopes_.pop_back();
            if (block_env_idx >= 0) { emit(Op::ExitLoopEnv); env_depth_--; }
            // The pads go last, where env_depth_ is back to what an escape
            // leaving this block should unwind from.
            if (has_using) return emit_finally_pads(escaped);
            return true;
        }

        case ASTNode::Type::USING_DECLARATION: {
            const auto* decl = static_cast<const UsingDeclaration*>(node);
            if (dispose_scope_depth_ == 0) return false;
            for (const auto& b : decl->get_bindings()) {
                if (b.name.empty()) return false;
                if (!env_names_.count(b.name) && lookup_local(b.name) < 0) return false;
                if (b.initializer) {
                    if (!compile_expression(b.initializer.get())) return false;
                    if (is_named_evaluation_rhs(b.initializer.get())) {
                        emit(Op::SetFunctionNameIfUnnamed);
                        emit_u16(add_name(b.name));
                    }
                } else {
                    emit(Op::LdaUndefined);
                }
                // Registration comes before the binding: a value with no
                // dispose method throws, and the name must not exist after it.
                emit(Op::RegisterDisposable);
                emit_u8(decl->is_await() ? 1 : 0);
                emit_write_local(b.name, /*is_declaration=*/true);
            }
            return !failed_;
        }

        case ASTNode::Type::WITH_STATEMENT: {
            emit_completion_reset();
            const auto* stmt = static_cast<const WithStatement*>(node);
            // Every name inside resolves by walking the chain, which is what
            // full env_mode gives; without it a local would sit in a register
            // the with object could never shadow.
            if (!env_mode_ || !full_env_) return false;
            if (!compile_expression(stmt->get_object())) return false;
            if (++try_env_depth_ > 64) return false;
            emit(Op::SaveEnv);
            emit(Op::PushWithEnv);

            FinallyScope scope;
            scope.cleanup = FinallyScope::Cleanup::RestoreOnly;
            scope.save_env = true;
            scope.loop_depth = loop_stack_.size();
            finally_stack_.push_back(std::move(scope));

            size_t body_start = code_.size();
            with_depth_++;
            bool body_ok = compile_statement(stmt->get_body());
            with_depth_--;
            size_t body_end = code_.size();
            FinallyScope escaped = std::move(finally_stack_.back());
            finally_stack_.pop_back();
            if (!body_ok) return false;

            // Leaving is the restore, on every path.
            emit(Op::RestoreEnv);
            try_env_depth_--;
            size_t jump_ok = emit_jump(Op::Jump);

            size_t cleanup_pc = code_.size();
            emit(Op::RestoreEnv);
            emit(Op::Throw);
            const size_t handler_idx = chunk_->ensure_handlers().size();
            chunk_->ensure_handlers().push_back({static_cast<uint32_t>(body_start),
                                         static_cast<uint32_t>(body_end),
                                         static_cast<uint32_t>(cleanup_pc)});

            if (suspendable_ && contains_suspend(stmt->get_body())) {
                size_t genreturn_pc = code_.size();
                emit(Op::RestoreEnv);
                emit(Op::ReraiseGeneratorReturn);
                chunk_->ensure_handlers()[handler_idx].genreturn_pc = static_cast<int32_t>(genreturn_pc);
            }

            if (!patch_jump(jump_ok)) return false;
            return emit_finally_pads(escaped);
        }

        case ASTNode::Type::IMPORT_STATEMENT:
            // Instantiated before the body ran, same as a function declaration.
            return script_mode_;

        case ASTNode::Type::EXPORT_STATEMENT: {
            if (!script_mode_) return false;  // only a Program has module declarations
            if (chunk_->ensure_exports().size() >= 0xFFFF) return false;
            const auto* ex = static_cast<const ExportStatement*>(node);
            BytecodeChunk::ExportRecord rec;
            rec.is_re_export = ex->is_re_export();
            rec.source_module = ex->get_source_module();

            // `export let x = 1` is a declaration with a record kept about it,
            // so the declaration is compiled here like any other and the record
            // says only which of its names leave the module.
            if (ex->is_declaration_export() && ex->get_declaration()) {
                const ASTNode* decl = ex->get_declaration();
                // A function declaration was already instantiated during
                // hoisting, same as a bare one at this level.
                if (decl->get_type() != ASTNode::Type::FUNCTION_DECLARATION &&
                    !compile_statement(decl)) {
                    return false;
                }
                switch (decl->get_type()) {
                    case ASTNode::Type::FUNCTION_DECLARATION: {
                        const auto* fd = static_cast<const FunctionDeclaration*>(decl);
                        if (fd->get_id()) {
                            rec.entries.push_back({fd->get_id()->get_name(),
                                                   fd->get_id()->get_name(), true});
                        }
                        break;
                    }
                    case ASTNode::Type::CLASS_DECLARATION: {
                        const auto* cd = static_cast<const ClassDeclaration*>(decl);
                        if (cd->get_id() && !cd->get_id()->get_name().empty()) {
                            rec.entries.push_back({cd->get_id()->get_name(),
                                                   cd->get_id()->get_name(), true});
                        }
                        break;
                    }
                    case ASTNode::Type::VARIABLE_DECLARATION: {
                        const auto* vd = static_cast<const VariableDeclaration*>(decl);
                        for (const auto& d : vd->get_declarations()) {
                            if (d->get_init() && d->get_init()->get_type() ==
                                                     ASTNode::Type::DESTRUCTURING_ASSIGNMENT) {
                                std::vector<std::string> bound;
                                static_cast<const DestructuringAssignment*>(d->get_init())
                                    ->collect_bound_names(bound);
                                for (const auto& bn : bound) rec.entries.push_back({bn, bn, true});
                                continue;
                            }
                            if (!d->get_id()) return false;
                            const std::string& n = d->get_id()->get_name();
                            if (!n.empty()) rec.entries.push_back({n, n, true});
                        }
                        break;
                    }
                    default:
                        return false;
                }
            }

            for (const auto& sp : ex->get_specifiers()) {
                rec.entries.push_back({sp->get_exported_name(), sp->get_local_name(), false});
            }

            if (ex->is_default_export() && ex->get_default_export()) {
                rec.is_default = true;
                rec.default_is_hoistable = ex->default_is_hoistable();
                const ASTNode* def = ex->get_default_export();
                switch (def->get_type()) {
                    case ASTNode::Type::FUNCTION_EXPRESSION: {
                        const auto* fe = static_cast<const FunctionExpression*>(def);
                        if (fe->is_named()) rec.default_local = fe->get_id()->get_name();
                        break;
                    }
                    case ASTNode::Type::ASYNC_FUNCTION_EXPRESSION: {
                        const auto* af = static_cast<const AsyncFunctionExpression*>(def);
                        if (af->get_id()) rec.default_local = af->get_id()->get_name();
                        break;
                    }
                    case ASTNode::Type::CLASS_DECLARATION: {
                        auto* cd = const_cast<ClassDeclaration*>(
                            static_cast<const ClassDeclaration*>(def));
                        if (cd->get_id()) rec.default_local = cd->get_id()->get_name();
                        // NamedEvaluation: a static initializer can observe the
                        // name while the class is still being built, so it is
                        // inferred before anything runs.
                        if (rec.default_local.empty()) cd->set_inferred_name("default");
                        break;
                    }
                    default:
                        break;
                }
                // A hoistable default was already built and bound; evaluating
                // it here would replace the function an importer may hold.
                if (!rec.default_is_hoistable && !compile_expression(def)) return false;
            }

            chunk_->ensure_exports().push_back(std::move(rec));
            emit(Op::LinkExports);
            emit_u16(static_cast<uint16_t>(chunk_->ensure_exports().size() - 1));
            return !failed_;
        }

        case ASTNode::Type::EXPRESSION_STATEMENT: {
            const auto* stmt = static_cast<const ExpressionStatement*>(node);
            if (completion_reg_ >= 0) {
                if (!compile_expression(stmt->get_expression())) return false;
                emit_completion_store();
                return !failed_;
            }
            return compile_expression(stmt->get_expression(), /*discard=*/true);
        }

        case ASTNode::Type::VARIABLE_DECLARATION: {
            const auto* decl = static_cast<const VariableDeclaration*>(node);
            bool is_var = decl->get_kind() == VariableDeclarator::Kind::VAR;
            bool is_const = decl->get_kind() == VariableDeclarator::Kind::CONST;
            for (const auto& d : decl->get_declarations()) {
                const std::string& name = d->get_id()->get_name();

                // A `var` inside a `with` resolves its binding before the
                // initializer runs, and the with object can own that name --
                // so the write may belong to the object. A lexical declaration
                // binds in its own scope and no object can stand in front of it.
                if (with_depth_ > 0 && is_var && !name.empty() && d->get_init()) {
                    emit(Op::ResolveWithTarget);
                    emit_u16(add_name(name));
                    int target = alloc_temp();
                    if (failed_) return false;
                    emit(Op::Star);
                    emit_u8(static_cast<uint8_t>(target));
                    if (!compile_expression(d->get_init())) return false;
                    if (is_named_evaluation_rhs(d->get_init())) {
                        emit(Op::SetFunctionNameIfUnnamed);
                        emit_u16(add_name(name));
                    }
                    emit(Op::StaWithResolved);
                    emit_u8(static_cast<uint8_t>(target));
                    emit_u16(add_name(name));
                    free_temp(target);
                    continue;
                }

                if (name.empty() && d->get_init() &&
                    d->get_init()->get_type() == ASTNode::Type::DESTRUCTURING_ASSIGNMENT) {
                    const auto* da = static_cast<const DestructuringAssignment*>(d->get_init());
                    if (!compile_expression(da->get_source())) return false;
                    const ASTNode* lit = da->get_pattern_literal();
                    if (!pattern_is_emittable(lit, !is_var)) return false;
                    if (!emit_pattern_bind(lit, !is_var, is_const)) return false;
                    continue;
                }

                if (!is_local(name)) {
                    // `var arguments` names the implicit object instead of
                    // declaring a binding of its own, so its write goes through
                    // the chain to the one Function::call already made.
                    const bool implicit_arguments =
                        name == "arguments" && allow_arguments_ && is_var;
                    if (!script_mode_ && !implicit_arguments) return false;  // prescan declared everything
                    // Top-level script declaration: the binding pre-exists
                    // (vars on the global object, let/const uninitialized in
                    // the script env).
                    stamp_inferred_class_name(d->get_init(), name);
                    bool is_lex = decl->get_kind() != VariableDeclarator::Kind::VAR;
                    if (!d->get_init()) {
                        if (is_var) continue;
                        emit(Op::LdaUndefined);
                    } else {
                        if (!compile_expression(d->get_init())) return false;
                        // NamedEvaluation: an anonymous function on the RHS
                        // takes the binding's name. The opcode only renames
                        // when the value is still unnamed, so a
                        // `function foo(){}` RHS keeps "foo" (spec 8.6.2).
                        if (is_named_evaluation_rhs(d->get_init())) {
                            emit(Op::SetFunctionNameIfUnnamed);
                            emit_u16(add_name(name));
                        }
                    }
                    if (is_lex) {
                        emit(Op::StaEnvInit);  // initializes the TDZ binding
                        emit_u16(add_name(name));
                    } else {
                        emit(Op::StaLookup);
                        emit_u16(add_name(name));
                    }
                    continue;
                }
                if (!d->get_init()) {
                    // `var x;`: pure hoisting, binding already exists as
                    // undefined (`const x;` is a grammar error).
                    if (is_var) continue;
                    emit(Op::LdaUndefined);
                } else {
                    stamp_inferred_class_name(d->get_init(), name);
                    if (!compile_expression(d->get_init())) return false;
                    // NamedEvaluation for a register-resident binding.
                    if (is_named_evaluation_rhs(d->get_init())) {
                        emit(Op::SetFunctionNameIfUnnamed);
                        emit_u16(add_name(name));
                    }
                }
                // `var` is function-scoped even inside a loop body -- a plain
                // chain-walked write, never StaEnvInit's per-iteration scope.
                emit_write_local(name, /*is_declaration=*/!is_var);
            }
            return true;
        }

        case ASTNode::Type::IF_STATEMENT: {
            emit_completion_reset();
            const auto* stmt = static_cast<const IfStatement*>(node);
            if (!compile_expression(stmt->get_test())) return false;
            size_t else_jump = emit_jump(Op::JumpIfFalse);
            if (!compile_if_branch(stmt->get_consequent())) return false;
            if (stmt->has_alternate()) {
                size_t end_jump = emit_jump(Op::Jump);
                if (!patch_jump(else_jump)) return false;
                if (!compile_if_branch(stmt->get_alternate())) return false;
                if (!patch_jump(end_jump)) return false;
            } else {
                if (!patch_jump(else_jump)) return false;
            }
            return true;
        }

        case ASTNode::Type::WHILE_STATEMENT: {
            emit_completion_reset();
            const auto* stmt = static_cast<const WhileStatement*>(node);
            int loop_env_idx = setup_loop_env({}, stmt->get_body(), false, {stmt->get_test()});
            if (loop_env_idx >= 0) {
                emit(Op::EnterLoopEnv);
                emit_u16(static_cast<uint16_t>(loop_env_idx));
                env_depth_++;
            }
            size_t loop_start = code_.size();
            if (!compile_expression(stmt->get_test())) return false;
            size_t exit_jump = emit_jump(Op::JumpIfFalse);

            // continue must run AdvanceLoopEnv before retrying the test, so
            // it can't be an immediate backward jump when this loop owns
            // per-iteration bindings -- defer it.
            bool deferred_continue = loop_env_idx >= 0;
            loop_stack_.push_back({loop_start, {}, {}, deferred_continue, env_depth_, try_env_depth_,
                                    false, take_pending_labels()});
            if (!compile_statement(stmt->get_body())) return false;
            LoopScope scope = std::move(loop_stack_.back());
            loop_stack_.pop_back();

            if (loop_env_idx >= 0) {
                for (size_t pos : scope.continue_patches) {
                    if (!patch_jump(pos)) return false;  // continue lands on the advance step
                }
                if (loop_env_needs_fresh(loop_env_idx)) {
                    emit(Op::AdvanceLoopEnv);
                    emit_u16(static_cast<uint16_t>(loop_env_idx));
                }
            }
            if (!emit_jump_back(Op::Jump, loop_start)) return false;
            if (!patch_jump(exit_jump)) return false;
            for (size_t pos : scope.break_patches) {
                if (!patch_jump(pos)) return false;
            }
            if (loop_env_idx >= 0) { emit(Op::ExitLoopEnv); env_depth_--; }
            return true;
        }

        case ASTNode::Type::DO_WHILE_STATEMENT: {
            emit_completion_reset();
            const auto* stmt = static_cast<const DoWhileStatement*>(node);
            int loop_env_idx = setup_loop_env({}, stmt->get_body(), false, {stmt->get_test()});
            if (loop_env_idx >= 0) {
                emit(Op::EnterLoopEnv);
                emit_u16(static_cast<uint16_t>(loop_env_idx));
                env_depth_++;
            }
            size_t body_start = code_.size();

            loop_stack_.push_back({0, {}, {}, true, env_depth_, try_env_depth_, false, take_pending_labels()});
            if (!compile_statement(stmt->get_body())) return false;
            LoopScope scope = std::move(loop_stack_.back());
            loop_stack_.pop_back();

            for (size_t pos : scope.continue_patches) {
                if (!patch_jump(pos)) return false;  // continue lands on the advance step / test
            }
            if (loop_env_idx >= 0 && loop_env_needs_fresh(loop_env_idx)) {
                emit(Op::AdvanceLoopEnv);
                emit_u16(static_cast<uint16_t>(loop_env_idx));
            }
            if (!compile_expression(stmt->get_test())) return false;
            if (!emit_jump_back(Op::JumpIfTrue, body_start)) return false;
            for (size_t pos : scope.break_patches) {
                if (!patch_jump(pos)) return false;
            }
            if (loop_env_idx >= 0) { emit(Op::ExitLoopEnv); env_depth_--; }
            return true;
        }

        case ASTNode::Type::FOR_STATEMENT: {
            emit_completion_reset();
            const auto* stmt = static_cast<const ForStatement*>(node);
            // `for (using x = r; ;)` holds the resource for the whole
            // statement, so the scope wraps the loop rather than sitting
            // inside it -- the init is where the resource is registered.
            const bool head_using = stmt->get_init() &&
                stmt->get_init()->get_type() == ASTNode::Type::USING_DECLARATION;
            auto emit_for = [&]() -> bool {
                // Only a let/const header needs a per-iteration copy-forward binding.
                std::vector<BytecodeChunk::LoopEnvVar> header_vars;
                if (stmt->get_init() && stmt->get_init()->get_type() == ASTNode::Type::VARIABLE_DECLARATION) {
                    const auto* vd = static_cast<const VariableDeclaration*>(stmt->get_init());
                    if (vd->get_kind() != VariableDeclarator::Kind::VAR) {
                        bool is_const = vd->get_kind() == VariableDeclarator::Kind::CONST;
                        for (const auto& d : vd->get_declarations()) {
                            if (!d->get_id()) continue;
                            const std::string& hname = d->get_id()->get_name();
                            if (!hname.empty()) {
                                header_vars.push_back({hname, true, is_const, true});
                                continue;
                            }
                            // A destructuring declarator carries no name of its own;
                            // each name the pattern binds needs its own copy-forward
                            // slot, or the body reads a binding nothing declared.
                            if (d->get_init() &&
                                d->get_init()->get_type() == ASTNode::Type::DESTRUCTURING_ASSIGNMENT) {
                                std::vector<std::string> bound;
                                static_cast<const DestructuringAssignment*>(d->get_init())
                                    ->collect_bound_names(bound);
                                for (const auto& bn : bound) {
                                    if (!bn.empty()) header_vars.push_back({bn, true, is_const, true});
                                }
                            }
                        }
                    }
                } else if (head_using) {
                    // `using` binds like a const, and the header is where that
                    // binding lives, so the loop scope has to carry the flag
                    // that makes an assignment to it refuse.
                    for (const auto& b :
                         static_cast<const UsingDeclaration*>(stmt->get_init())->get_bindings()) {
                        if (!b.name.empty()) header_vars.push_back({b.name, true, true, true});
                    }
                }
                lexical_scopes_.emplace_back();
                for (const auto& v : header_vars) {
                    lexical_scopes_.back().push_back(v.name);
                    block_scoped_names_.insert(v.name);
                }
                struct LexicalScopePop {
                    BytecodeCompiler* c;
                    ~LexicalScopePop() { c->lexical_scopes_.pop_back(); }
                } lexical_pop{this};
                int loop_env_idx = setup_loop_env(std::move(header_vars), stmt->get_body(), false,
                                                   {stmt->get_init(), stmt->get_test(), stmt->get_update()});
                if (loop_env_idx >= 0) {
                    emit(Op::EnterLoopEnv);
                    emit_u16(static_cast<uint16_t>(loop_env_idx));
                    env_depth_++;
                }
                if (stmt->get_init()) {
                    const auto init_type = stmt->get_init()->get_type();
                    if (init_type == ASTNode::Type::VARIABLE_DECLARATION ||
                        init_type == ASTNode::Type::USING_DECLARATION) {
                        if (!compile_statement(stmt->get_init())) return false;
                    } else {
                        if (!compile_expression(stmt->get_init())) return false;
                    }
                }
                if (loop_env_idx >= 0 && loop_env_needs_fresh(loop_env_idx)) {
                    // Spec 14.7.4.3 ForBodyEvaluation step 1: CreatePerIterationEnvironment
                    // runs once more right after the init, before the first test --
                    // a closure created during init (e.g. a second declarator's
                    // initializer) must NOT alias the environment the first test/
                    // body/update mutate. Skipped when no closure anywhere in this
                    // for-statement can observe it -- see loop_vars_may_be_captured.
                    emit(Op::AdvanceLoopEnv);
                    emit_u16(static_cast<uint16_t>(loop_env_idx));
                }
                size_t loop_start = code_.size();
                size_t exit_jump = 0;
                bool has_test = stmt->get_test() != nullptr;
                if (has_test) {
                    if (!compile_expression(stmt->get_test())) return false;
                    exit_jump = emit_jump(Op::JumpIfFalse);
                }

                loop_stack_.push_back({0, {}, {}, true, env_depth_, try_env_depth_, false, take_pending_labels()});
                if (!compile_statement(stmt->get_body())) return false;
                LoopScope scope = std::move(loop_stack_.back());
                loop_stack_.pop_back();

                for (size_t pos : scope.continue_patches) {
                    if (!patch_jump(pos)) return false;  // continue lands on the advance step / update
                }
                if (loop_env_idx >= 0 && loop_env_needs_fresh(loop_env_idx)) {
                    emit(Op::AdvanceLoopEnv);
                    emit_u16(static_cast<uint16_t>(loop_env_idx));
                }
                if (stmt->get_update()) {
                    if (!compile_expression(stmt->get_update(), /*discard=*/true)) return false;
                }
                if (!emit_jump_back(Op::Jump, loop_start)) return false;
                if (has_test && !patch_jump(exit_jump)) return false;
                for (size_t pos : scope.break_patches) {
                    if (!patch_jump(pos)) return false;
                }
                if (loop_env_idx >= 0) { emit(Op::ExitLoopEnv); env_depth_--; }
                return true;
            };
            if (!head_using) return emit_for();
            FinallyScope for_escaped;
            if (!emit_dispose_scope_body(stmt, emit_for, for_escaped)) return false;
            return emit_finally_pads(for_escaped);
        }

        case ASTNode::Type::FOR_OF_STATEMENT: {
            const auto* stmt = static_cast<const ForOfStatement*>(node);
            return compile_for_each_loop(stmt->get_left(), stmt->get_right(), stmt->get_body(), false,
                                          stmt->get_left_decl_kind(), stmt->is_await());
        }

        case ASTNode::Type::FOR_IN_STATEMENT: {
            const auto* stmt = static_cast<const ForInStatement*>(node);
            return compile_for_each_loop(stmt->get_left(), stmt->get_right(), stmt->get_body(), true,
                                          stmt->get_left_decl_kind());
        }

        case ASTNode::Type::BREAK_STATEMENT:
            return emit_loop_escape(/*is_continue=*/false,
                                    static_cast<const BreakStatement*>(node)->get_label());

        case ASTNode::Type::CONTINUE_STATEMENT:
            return emit_loop_escape(/*is_continue=*/true,
                                    static_cast<const ContinueStatement*>(node)->get_label());

        case ASTNode::Type::RETURN_STATEMENT: {
            const auto* stmt = static_cast<const ReturnStatement*>(node);
            if (stmt->get_argument()) {
                if (!compile_expression(stmt->get_argument())) return false;
            } else {
                emit(Op::LdaUndefined);
            }
            return emit_return_completion(stmt->get_argument() != nullptr,
                                         /*already_awaited=*/false);
        }

        case ASTNode::Type::THROW_STATEMENT: {
            const auto* stmt = static_cast<const ThrowStatement*>(node);
            if (!compile_expression(stmt->get_expression())) return false;
            emit(Op::Throw);
            return true;
        }

        case ASTNode::Type::TRY_STATEMENT: {
            emit_completion_reset();
            const auto* stmt = static_cast<const TryStatement*>(node);
            const ASTNode* catch_node = stmt->get_catch_clause();
            const ASTNode* finally_node = stmt->get_finally_block();
            if (!catch_node && !finally_node) return false;
            // 14.15.3 step 3: a finally that completes normally hands the
            // result back to the block's own completion, so nothing it
            // evaluates may be recorded as the script's value.
            const int saved_completion_reg = completion_reg_;

            // A throw skips straight to the handler, bypassing any loop
            // Environment pop inside the try -- see VM::run's env_saves side-stack.
            bool save_env = env_mode_;
            if (save_env) {
                if (++try_env_depth_ > 64) return false;  // matches VM::run's fixed env_saves[64]
                emit(Op::SaveEnv);
            }

            if (finally_node) {
                FinallyScope fs;
                fs.finally_node = finally_node;
                fs.save_env = save_env;
                fs.loop_depth = loop_stack_.size();
                finally_stack_.push_back(std::move(fs));
            }
            size_t try_start = code_.size();
            if (!compile_statement(stmt->get_try_block())) return false;
            size_t try_end = code_.size();
            FinallyScope escaped_scope;
            if (finally_node) {
                escaped_scope = std::move(finally_stack_.back());
                finally_stack_.pop_back();
            }
            if (save_env) {
                emit(Op::PopEnvSave);  // no exception: already correctly restored
                try_env_depth_--;
            }
            size_t jump_try_ok = emit_jump(Op::Jump);

            // Patched with genreturn_pc below, once that pc is known.
            size_t handler_idx_try = SIZE_MAX, handler_idx_catch = SIZE_MAX;

            size_t catch_body_start = 0, catch_body_end = 0, jump_catch_ok = 0;
            // A return/break/continue leaving the catch body owes the finally the
            // same pad the try block's escapes get, and its own one: the catch
            // body runs under a separate SaveEnv.
            FinallyScope catch_escaped_scope;
            if (catch_node) {
                const auto* clause = static_cast<const CatchClause*>(catch_node);
                size_t catch_pc = code_.size();
                if (save_env) emit(Op::RestoreEnv);
                // A catch-body throw still reaches finally's own RestoreEnv --
                // give it its own save slot or that underflows env_saves.
                bool save_env_for_catch_body = save_env && finally_node;
                if (save_env_for_catch_body) {
                    if (++try_env_depth_ > 64) return false;
                    emit(Op::SaveEnv);
                }
                int catch_env_idx = -1;
                const ASTNode* catch_pattern = clause->get_destructuring_pattern();
                std::vector<std::string> catch_names;
                if (catch_pattern) {
                    static_cast<const DestructuringAssignment*>(catch_pattern)
                        ->collect_bound_names(catch_names);
                } else if (!clause->get_parameter_name().empty()) {
                    catch_names.push_back(clause->get_parameter_name());
                }
                // A pattern that binds nothing (`catch ([,])`, `catch ({})`) is
                // still evaluated: it steps the iterator and rejects a null
                // value. Only `catch {}` skips the parameter entirely.
                if (catch_pattern || !catch_names.empty()) {
                    for (const auto& cn : catch_names) {
                        if (!is_local(cn)) return false;
                    }
                    // Spec: a fresh Environment per catch, else it would
                    // overwrite an outer same-named binding instead of shadowing it.
                    if (env_mode_ && !catch_names.empty()) {
                        std::vector<BytecodeChunk::LoopEnvVar> vars;
                        for (const auto& cn : catch_names) vars.push_back({cn, false, false, false});
                        record_env_slot_info(vars, env_depth_ + 1);
                        chunk_->ensure_env().loop_envs.push_back(std::move(vars));
                        // A catch clause never emits AdvanceLoopEnv either
                        // (same reasoning as the plain-block case above) --
                        // keep loop_env_needs_fresh_ in lockstep regardless.
                        loop_env_needs_fresh_.push_back(true);
                        catch_env_idx = static_cast<int>(chunk_->ensure_env().loop_envs.size() - 1);
                        emit(Op::EnterLoopEnv);
                        emit_u16(static_cast<uint16_t>(catch_env_idx));
                        env_depth_++;
                    }
                    if (catch_pattern) {
                        const ASTNode* lit = static_cast<const DestructuringAssignment*>(catch_pattern)
                                                 ->get_pattern_literal();
                        if (!pattern_is_emittable(lit, /*is_lexical=*/true)) return false;
                        if (!emit_pattern_bind(lit, /*is_lexical=*/true, /*is_const=*/false)) return false;
                    } else {
                        emit_write_local(catch_names[0], /*is_declaration=*/true);
                    }
                }
                // else: optional catch binding (`catch {}`) -- exception in
                // acc is simply discarded, nothing to store.
                catch_body_start = code_.size();
                if (finally_node) {
                    FinallyScope cfs;
                    cfs.finally_node = finally_node;
                    cfs.save_env = save_env_for_catch_body;
                    cfs.loop_depth = loop_stack_.size();
                    finally_stack_.push_back(std::move(cfs));
                }
                lexical_scopes_.emplace_back();
                for (const auto& cn : catch_names) {
                    lexical_scopes_.back().push_back(cn);
                    block_scoped_names_.insert(cn);
                }
                bool catch_body_ok = compile_statement(clause->get_body());
                lexical_scopes_.pop_back();
                if (finally_node) {
                    catch_escaped_scope = std::move(finally_stack_.back());
                    finally_stack_.pop_back();
                }
                if (!catch_body_ok) return false;
                catch_body_end = code_.size();
                if (catch_env_idx >= 0) { emit(Op::ExitLoopEnv); env_depth_--; }
                if (save_env_for_catch_body) {
                    emit(Op::PopEnvSave);
                    try_env_depth_--;
                }
                jump_catch_ok = emit_jump(Op::Jump);

                handler_idx_try = chunk_->ensure_handlers().size();
                chunk_->ensure_handlers().push_back({static_cast<uint32_t>(try_start),
                                             static_cast<uint32_t>(try_end),
                                             static_cast<uint32_t>(catch_pc)});
            }

            if (finally_node) {
                // Exception path: try (if no catch) or catch (if present)
                // raised past this point -- save it, run finally, re-raise.
                size_t reraise_pc = code_.size();
                if (save_env) emit(Op::RestoreEnv);
                int temp = alloc_temp();
                if (failed_) return false;
                emit(Op::Star);
                emit_u8(static_cast<uint8_t>(temp));
                completion_reg_ = -1;
                if (!compile_statement(finally_node)) return false;
                completion_reg_ = saved_completion_reg;
                emit(Op::Ldar);
                emit_u8(static_cast<uint8_t>(temp));
                free_temp(temp);
                emit(Op::Throw);

                if (catch_node) {
                    handler_idx_catch = chunk_->ensure_handlers().size();
                    chunk_->ensure_handlers().push_back({static_cast<uint32_t>(catch_body_start),
                                                 static_cast<uint32_t>(catch_body_end),
                                                 static_cast<uint32_t>(reraise_pc)});
                } else {
                    handler_idx_try = chunk_->ensure_handlers().size();
                    chunk_->ensure_handlers().push_back({static_cast<uint32_t>(try_start),
                                                 static_cast<uint32_t>(try_end),
                                                 static_cast<uint32_t>(reraise_pc)});
                }

                // A generator .return() mid-suspend unwinds as a C++
                // exception that must skip any catch clause -- always a
                // finally-only pad, whether or not catch_node exists.
                bool try_needs_genreturn = suspendable_ && contains_suspend(stmt->get_try_block());
                bool catch_needs_genreturn = suspendable_ && catch_node &&
                    contains_suspend(static_cast<const CatchClause*>(catch_node)->get_body());
                if (try_needs_genreturn || catch_needs_genreturn) {
                    size_t genreturn_pc = code_.size();
                    if (save_env) emit(Op::RestoreEnv);
                    int gr_temp = alloc_temp();
                    if (failed_) return false;
                    emit(Op::Star);
                    emit_u8(static_cast<uint8_t>(gr_temp));
                    completion_reg_ = -1;
                    if (!compile_statement(finally_node)) return false;
                    completion_reg_ = saved_completion_reg;
                    emit(Op::Ldar);
                    emit_u8(static_cast<uint8_t>(gr_temp));
                    free_temp(gr_temp);
                    emit(Op::ReraiseGeneratorReturn);

                    if (try_needs_genreturn && handler_idx_try != SIZE_MAX) {
                        chunk_->ensure_handlers()[handler_idx_try].genreturn_pc = static_cast<int32_t>(genreturn_pc);
                    }
                    if (catch_needs_genreturn && handler_idx_catch != SIZE_MAX) {
                        chunk_->ensure_handlers()[handler_idx_catch].genreturn_pc = static_cast<int32_t>(genreturn_pc);
                    }
                }
            }

            // Normal-completion path: both "try succeeded" and "catch
            // succeeded" land here and share the same finally emission.
            if (!patch_jump(jump_try_ok)) return false;
            if (catch_node && !patch_jump(jump_catch_ok)) return false;
            if (finally_node) {
                completion_reg_ = -1;
                if (!compile_statement(finally_node)) return false;
                completion_reg_ = saved_completion_reg;
            }

            if (!emit_finally_pads(escaped_scope)) return false;
            if (!emit_finally_pads(catch_escaped_scope)) return false;
            return true;
        }

        case ASTNode::Type::SWITCH_STATEMENT: {
            emit_completion_reset();
            const auto* stmt = static_cast<const SwitchStatement*>(node);
            const auto& cases = stmt->get_cases();

            if (!compile_expression(stmt->get_discriminant())) return false;
            int disc_reg = alloc_temp();
            if (failed_) return false;
            emit(Op::Star);
            emit_u8(static_cast<uint8_t>(disc_reg));

            // Spec: every case shares ONE lexical scope, not one each --
            // entered after the discriminant, before any case test runs.
            // The names it declares have to be resolvable while the clauses
            // compile, and stop being resolvable after: without a scope of
            // its own every `case X: let a = 1;` compiled its reads as
            // by-name lookups of a name nothing had declared.
            int switch_env_idx = -1;
            lexical_scopes_.emplace_back();
            struct SwitchScopePop {
                BytecodeCompiler* c;
                ~SwitchScopePop() { c->lexical_scopes_.pop_back(); }
            } switch_scope_pop{this};
            {
                std::vector<BytecodeChunk::LoopEnvVar> vars;
                bool needs_own_env = false;
                for (const auto& c : cases) {
                    for (const auto& s : static_cast<const CaseClause*>(c.get())->get_consequent()) {
                        // `case X: let a = 1;` declares a in the switch's own
                        // scope; `case X: { let a = 1; }` declares it in that
                        // block's. Handing the braced form to a collector that
                        // reads a block as "the scope whose lexicals I want"
                        // hoisted it into the switch env, where it shadowed
                        // every other reader of that name for the whole switch
                        // body -- with a TDZ, so reading an outer binding of
                        // the same name threw. The block declares its own.
                        if (s->get_type() == ASTNode::Type::BLOCK_STATEMENT) continue;
                        // A function declared in any clause binds in the
                        // switch's own scope, so its presence alone calls for
                        // one (BlockDeclarationInstantiation over the whole
                        // case block).
                        if (s->get_type() == ASTNode::Type::FUNCTION_DECLARATION) {
                            needs_own_env = true;
                            continue;
                        }
                        if (!collect_direct_lexical_decls(s.get(), vars, needs_own_env)) return false;
                    }
                }
                for (const auto& v : vars) {
                    lexical_scopes_.back().push_back(v.name);
                    block_scoped_names_.insert(v.name);
                }
                std::vector<BytecodeChunk::LoopEnvVar> env_vars;
                for (const auto& v : vars) {
                    int reg = env_names_.count(v.name) ? -1 : lookup_local(v.name);
                    if (reg >= 0) {
                        // Register-resident case-block lexical: re-arm its
                        // TDZ instead of living in the switch's own env --
                        // a register isn't block-scoped, so this alone
                        // wouldn't stop it leaking past the switch, but the
                        // TDZ re-arm at least matches re-entry semantics
                        // (switch bodies don't loop, so this runs once).
                        emit(Op::LdaTdz);
                        emit(Op::Star);
                        emit_u8(static_cast<uint8_t>(reg));
                    } else {
                        env_vars.push_back(v);
                    }
                }
                if (env_mode_ && (!env_vars.empty() || needs_own_env)) {
                    record_env_slot_info(env_vars, env_depth_ + 1);
                    chunk_->ensure_env().loop_envs.push_back(std::move(env_vars));
                    // A switch body never emits AdvanceLoopEnv either (runs
                    // once, no per-iteration refresh) -- keep
                    // loop_env_needs_fresh_ in lockstep regardless.
                    loop_env_needs_fresh_.push_back(true);
                    switch_env_idx = static_cast<int>(chunk_->ensure_env().loop_envs.size() - 1);
                    emit(Op::EnterLoopEnv);
                    emit_u16(static_cast<uint16_t>(switch_env_idx));
                    env_depth_++;
                }
            }

            // Spec sec-switch-statement step 5: the whole case block is
            // instantiated before the first selector is evaluated, so a call
            // in one clause finds a function declared in another.
            for (const auto& c : cases) {
                for (const auto& s : static_cast<const CaseClause*>(c.get())->get_consequent()) {
                    if (s->get_type() != ASTNode::Type::FUNCTION_DECLARATION) continue;
                    if (!env_mode_) return false;
                    if (chunk_->ensure_closures().size() >= 0xFFFF) return false;
                    hoisted_fn_decls_.insert(s.get());
                    chunk_->ensure_closures().push_back(closure_template_for(s.get()));
                    emit(Op::DeclareFunction);
                    emit_u16(static_cast<uint16_t>(chunk_->ensure_closures().size() - 1));
                }
            }

            std::vector<size_t> test_jumps(cases.size(), 0);
            int default_index = -1;
            for (size_t i = 0; i < cases.size(); i++) {
                const auto* cc = static_cast<const CaseClause*>(cases[i].get());
                if (cc->is_default()) { default_index = static_cast<int>(i); continue; }
                if (!compile_expression(cc->get_test())) return false;
                emit(Op::TestStrictEq);
                emit_u8(static_cast<uint8_t>(disc_reg));
                test_jumps[i] = emit_jump(Op::JumpIfTrue);
            }
            size_t jump_to_default_or_end = emit_jump(Op::Jump);
            free_temp(disc_reg);

            loop_stack_.push_back({0, {}, {}, true, env_depth_, try_env_depth_,
                                    /*is_switch=*/true, take_pending_labels()});

            // All the cases share one scope but control enters it at whichever
            // case matched, so a declaration in an earlier case says nothing
            // about whether it has run by the time a later one is reached.
            switch_body_depth_++;
            for (size_t i = 0; i < cases.size(); i++) {
                const auto* cc = static_cast<const CaseClause*>(cases[i].get());
                if (cc->is_default()) {
                    if (!patch_jump(jump_to_default_or_end)) { switch_body_depth_--; return false; }
                } else if (!patch_jump(test_jumps[i])) {
                    switch_body_depth_--;
                    return false;
                }
                for (const auto& s : cc->get_consequent()) {
                    if (!compile_statement(s.get())) { switch_body_depth_--; return false; }
                }
            }
            switch_body_depth_--;
            if (default_index < 0 && !patch_jump(jump_to_default_or_end)) return false;

            LoopScope scope = std::move(loop_stack_.back());
            loop_stack_.pop_back();
            for (size_t pos : scope.break_patches) {
                if (!patch_jump(pos)) return false;
            }
            if (switch_env_idx >= 0) { emit(Op::ExitLoopEnv); env_depth_--; }
            return true;
        }

        case ASTNode::Type::LABELED_STATEMENT: {
            const auto* stmt = static_cast<const LabeledStatement*>(node);
            const std::string& label = stmt->get_label();
            for (const auto& l : pending_labels_) if (l == label) return false;
            for (const auto& s : loop_stack_) {
                for (const auto& l : s.labels) if (l == label) return false;
            }

            switch (stmt->get_statement()->get_type()) {
                case ASTNode::Type::FOR_STATEMENT:
                case ASTNode::Type::WHILE_STATEMENT:
                case ASTNode::Type::DO_WHILE_STATEMENT:
                case ASTNode::Type::FOR_OF_STATEMENT:
                case ASTNode::Type::FOR_IN_STATEMENT:
                case ASTNode::Type::SWITCH_STATEMENT:
                case ASTNode::Type::LABELED_STATEMENT:
                    // The loop/switch (or next label) below takes this from pending_labels_.
                    pending_labels_.push_back(label);
                    return compile_statement(stmt->get_statement());
                default: {
                    // Not a loop: break-only wrapper, no continue target.
                    loop_stack_.push_back({0, {}, {}, true, env_depth_, try_env_depth_,
                                            /*is_switch=*/true, {label}});
                    if (!compile_if_branch(stmt->get_statement())) return false;
                    LoopScope scope = std::move(loop_stack_.back());
                    loop_stack_.pop_back();
                    for (size_t pos : scope.break_patches) {
                        if (!patch_jump(pos)) return false;
                    }
                    return true;
                }
            }
        }

        case ASTNode::Type::CLASS_DECLARATION: {
            // ClassDeclaration::evaluate() binds the class's own name via
            // ctx.create_lexical_binding() on the environment directly, so a
            // register-resident name is never written -- force it here.
            if (!env_mode_) return false;
            if (try_compile_plain_class(static_cast<const ClassDeclaration*>(node),
                                        /*bind_name=*/true)) return true;
            if (chunk_->ensure_ast_nodes().size() >= 0xFFFF) return false;
            chunk_->ensure_ast_nodes().push_back(node);
            emit(Op::DefineClass);
            emit_u16(static_cast<uint16_t>(chunk_->ensure_ast_nodes().size() - 1));
            // define_class binds the name itself, so this only
            // has to mirror it into a register when the name has one. A module
            // or script top level has neither a register nor an env slot for
            // it, and asking for one there used to yield register -1.
            const Identifier* class_id = static_cast<const ClassDeclaration*>(node)->get_id();
            if (class_id && !class_id->get_name().empty() &&
                !env_names_.count(class_id->get_name()) &&
                lookup_local(class_id->get_name()) >= 0) {
                emit_write_local(class_id->get_name(), /*is_declaration=*/true);
            }
            return true;
        }

        default:
            return false;
    }
}

// A class the compiler can describe in full: no heritage, no fields, no static
// block, no private names, no computed keys, no accessors and no static
// members, an explicit constructor, and a name no member reads back. What is
// left is a prototype, a constructor and a run of methods -- all of which the
// instructions below build, so nothing about the class has to survive as a
// node. Anything outside that answers false and takes Op::DefineClass instead.
bool BytecodeCompiler::try_compile_plain_class(const ClassDeclaration* cls, bool bind_name) {
    if (!env_mode_) return false;
    const BlockStatement* body = cls->get_body();
    if (!body) return false;
    const Identifier* class_id = cls->get_id();
    // Only a class written with a name of its own has the inner immutable
    // binding; one named from its binding site carries the name and nothing
    // else, so there is no scope for a member to read it out of.
    const std::string inner_name = class_id ? class_id->get_name() : std::string();
    const std::string class_name = inner_name.empty() ? cls->get_inferred_name() : inner_name;
    if (bind_name && class_name.empty()) return false;

    // Whether anything inside the class reads its own name. Only then is the
    // scope holding that name worth building. The heritage expression counts:
    // it runs with that name still uninitialized.
    const ASTNode* superclass = cls->get_superclass();
    bool reads_own_name = !inner_name.empty() && superclass &&
                          references_identifier(superclass, inner_name);
    const MethodDefinition* ctor = nullptr;
    // Every element the definition pass handles, in the order the class writes
    // them -- which is the order their keys are evaluated in, and a computed
    // key is arbitrary code, so the order is observable.
    struct Element {
        bool is_field;
        std::string key;                    // empty when the key is computed
        const ASTNode* key_expr = nullptr;  // the computed key, if any
        const ASTNode* init = nullptr;      // a field's initializer
        const MethodDefinition* m = nullptr;
        uint8_t kind = 0;                   // 0 method, 1 getter, 2 setter
        bool is_static = false;
    };
    std::vector<Element> elements;
    // A static field or a static block: both run once, against the
    // constructor, after the definition pass, in the order the class writes
    // them. An empty key marks a block, which answers nothing.
    struct StaticElement { std::string key; const ASTNode* body; };
    std::vector<StaticElement> static_elements;
    for (const auto& st : body->get_statements()) {
        if (st->get_type() == ASTNode::Type::CLASS_STATIC_BLOCK) {
            const auto* sb = static_cast<const ClassStaticBlock*>(st.get());
            if (!sb->get_body()) return false;
            if (!inner_name.empty() && references_identifier(sb->get_body(), inner_name)) {
                reads_own_name = true;
            }
            static_elements.push_back({std::string(), sb->get_body()});
            continue;
        }
        if (st->get_type() == ASTNode::Type::CLASS_FIELD) {
            const auto* cf = static_cast<const ClassField*>(st.get());
            // A static element's key is resolved in the definition pass but its
            // value only afterwards, so a computed one would have to be carried
            // between the two; that is left to Op::DefineClass.
            if (cf->is_computed() && cf->is_static()) return false;
            std::string key;
            if (!cf->is_computed()) {
                const ASTNode* kn = cf->get_key();
                if (kn->get_type() == ASTNode::Type::IDENTIFIER) {
                    key = static_cast<const Identifier*>(kn)->get_name();
                } else if (kn->get_type() == ASTNode::Type::STRING_LITERAL) {
                    key = static_cast<const StringLiteral*>(kn)->get_value();
                } else if (kn->get_type() == ASTNode::Type::NUMBER_LITERAL) {
                    key = numeric_key_text(static_cast<const NumberLiteral*>(kn)->get_value());
                } else {
                    return false;
                }
                if (key.empty()) return false;
            }
            if (!inner_name.empty() && cf->get_value() &&
                references_identifier(cf->get_value(), inner_name)) {
                reads_own_name = true;
            }
            if (cf->is_static()) {
                static_elements.push_back({std::move(key), cf->get_value()});
            } else {
                Element e;
                e.is_field = true;
                e.key = std::move(key);
                e.key_expr = cf->is_computed() ? cf->get_key() : nullptr;
                e.init = cf->get_value();
                elements.push_back(std::move(e));
            }
            continue;
        }
        if (st->get_type() != ASTNode::Type::METHOD_DEFINITION) return false;
        const auto* m = static_cast<const MethodDefinition*>(st.get());
        const FunctionExpression* fe = m->get_value();
        if (!fe) return false;
        // The class name is an inner immutable binding of its own scope. No
        // member here reads it, so that scope would hold nothing observable
        // and is not built at all; a member that does read it falls back.
        if (!inner_name.empty() && !reads_own_name) {
            // A member whose body was stepped over cannot be asked whether it
            // names the class; the scope holding that name is built anyway.
            if (!fe->get_body() || references_identifier(fe->get_body(), inner_name)) {
                reads_own_name = true;
            } else {
                for (const auto& prm : fe->get_params()) {
                    if ((prm->has_default() &&
                         references_identifier(prm->get_default_value(), inner_name)) ||
                        (prm->has_destructuring() &&
                         references_identifier(prm->get_destructuring_pattern(), inner_name))) {
                        reads_own_name = true;
                        break;
                    }
                }
            }
        }
        std::string key;
        if (!m->is_computed()) {
            const ASTNode* kn = m->get_key();
            if (kn->get_type() == ASTNode::Type::IDENTIFIER) {
                key = static_cast<const Identifier*>(kn)->get_name();
            } else if (kn->get_type() == ASTNode::Type::STRING_LITERAL) {
                key = static_cast<const StringLiteral*>(kn)->get_value();
            } else if (kn->get_type() == ASTNode::Type::NUMBER_LITERAL) {
                key = numeric_key_text(static_cast<const NumberLiteral*>(kn)->get_value());
            } else {
                return false;
            }
            if (key.empty()) return false;
        }
        Element e;
        e.is_field = false;
        e.key = std::move(key);
        e.key_expr = m->is_computed() ? m->get_key() : nullptr;
        e.m = m;
        e.is_static = m->is_static();
        switch (m->get_kind()) {
            case MethodDefinition::CONSTRUCTOR:
                if (ctor || m->is_computed()) return false;
                ctor = m;
                continue;
            case MethodDefinition::METHOD: e.kind = 0; break;
            case MethodDefinition::GETTER: e.kind = 1; break;
            case MethodDefinition::SETTER: e.kind = 2; break;
            default:
                return false;
        }
        elements.push_back(std::move(e));
    }
    if (chunk_->ensure_closures().size() + elements.size() + 1 >= 0xFFFF) return false;

    // From here on instructions go out. Only the heritage expression can still
    // refuse -- it is arbitrary code, and whether the compiler takes it is not
    // answerable without trying. So the emission point is remembered: a
    // refusal winds back to it and the class goes through Op::DefineClass,
    // which reads the heritage off the node instead. Everything else the
    // compiler adds along the way (names, feedback slots, closure templates)
    // is only ever reached from code, so unreferenced leftovers are inert.
    const size_t emit_mark = code_.size();
    const size_t env_mark = chunk_->ensure_env().loop_envs.size();
    const int env_depth_mark = env_depth_;
    const int register_mark = next_register_;

    int proto_reg = alloc_temp();
    int ctor_reg = alloc_temp();
    int super_reg = superclass ? alloc_temp() : -1;
    if (proto_reg < 0 || ctor_reg < 0) return false;

    // classScope (15.7.14 steps 2-4): the class's own name is an inner
    // immutable binding, separate from any outer one, that the members close
    // over. Nothing else goes in it, and it is left before the outer binding
    // is made -- a named class expression binds its name only inside itself.
    int class_env_idx = -1;
    // Shadows env_slot_info_'s entry for inner_name, if it has one, for
    // exactly the scope holding this binding. global_decl_count_ never learns
    // about this binding -- it is not a var/let/const/function declaration
    // prescan_declarations recognizes, only ClassDefinitionEvaluation's own
    // classScope -- so an outer declaration of the same name still looks
    // globally unique to record_env_slot_info's own uniqueness gate. Left
    // alone, a read of inner_name in here would trust that outer slot: right
    // depth by the exact-match rule's own accounting, wrong scope, and (since
    // Shape::intern makes every "x" the same pointer) indistinguishable from
    // the right one by the by-name recheck LdaEnvSlot's guard is built on.
    // Un-erased on every path out, including a mid-class return false.
    struct ClassNameShadow {
        // A reference into the (private) map, taken from inside the member
        // function that already has access to it -- this local class is not
        // itself a member of BytecodeCompiler and so has none of its own.
        std::unordered_map<std::string, EnvSlotInfo>& slots;
        std::string name;
        bool active;
        bool had_entry;
        EnvSlotInfo saved{};
        ClassNameShadow(std::unordered_map<std::string, EnvSlotInfo>& slots,
                        std::string name, bool active)
            : slots(slots), name(std::move(name)), active(active), had_entry(false) {
            if (!active) return;
            auto it = slots.find(this->name);
            had_entry = it != slots.end();
            if (had_entry) { saved = it->second; slots.erase(it); }
        }
        ~ClassNameShadow() {
            if (!active) return;
            if (had_entry) slots[name] = saved;
            else slots.erase(name);
        }
    } class_name_shadow(env_slot_info_, inner_name, reads_own_name);
    if (reads_own_name) {
        std::vector<BytecodeChunk::LoopEnvVar> env_vars;
        env_vars.push_back({inner_name, /*is_lexical=*/true, /*is_const=*/true,
                            /*copy_forward=*/false});
        chunk_->ensure_env().loop_envs.push_back(std::move(env_vars));
        // Entered and left once, never refreshed; the entry keeps
        // loop_env_needs_fresh_ the same length as loop_envs so other indices
        // into it stay valid.
        loop_env_needs_fresh_.push_back(true);
        class_env_idx = static_cast<int>(chunk_->ensure_env().loop_envs.size() - 1);
        emit(Op::EnterLoopEnv);
        emit_u16(static_cast<uint16_t>(class_env_idx));
        env_depth_++;
    }

    // ClassHeritage is class code: strict, and evaluated with the class's own
    // name in scope but not yet initialized.
    if (superclass) {
        const bool failed_before = failed_;
        const size_t closures_before = chunk_->ensure_closures().size();
        if (!compile_expression(superclass)) {
            code_.resize(emit_mark);
            chunk_->ensure_env().loop_envs.resize(env_mark);
            loop_env_needs_fresh_.resize(env_mark);
            env_depth_ = env_depth_mark;
            next_register_ = register_mark;
            failed_ = failed_before;
            return false;
        }
        // ClassHeritage is class code, so a function literal written in it is
        // strict however sloppy the code around the class is.
        auto& closures = chunk_->ensure_closures();
        for (size_t i = closures_before; i < closures.size(); i++) {
            closures[i].body_is_strict = true;
        }
        emit(Op::Star);
        emit_u8(static_cast<uint8_t>(super_reg));
    }

    emit(Op::CreateObject);
    emit_u16(static_cast<uint16_t>(elements.size() + 1));
    emit(Op::Star);
    emit_u8(static_cast<uint8_t>(proto_reg));

    {
        ClosureTemplate tmpl;
        if (ctor) {
            tmpl = closure_template_for(ctor->get_value());
            // What a class reports for itself is its whole text, not its
            // constructor's. The executable belongs to this one class site, so
            // pointing it at the class's range costs nothing and keeps the
            // text uncut until something asks.
            if (cls->has_source_range()) {
                tmpl.executable->set_source_range(cls->source_start(), cls->source_end());
            }
        } else {
            // No constructor written: an empty one, carrying the class's own
            // name and text like any other.
            tmpl.executable = make_default_class_constructor_executable();
            tmpl.form = ClosureTemplate::Form::FunctionExpr;
            tmpl.body_is_strict = true;
            // Nothing to cut from here: this body was made, not parsed.
            tmpl.executable->set_source_text(cls->get_source_text());
        }
        chunk_->ensure_closures().push_back(std::move(tmpl));
    }
    emit(Op::CreateClosure);
    emit_u16(static_cast<uint16_t>(chunk_->ensure_closures().size() - 1));
    // The constructor carries the class's name, not the key it was written
    // under.
    if (!class_name.empty()) {
        emit(Op::SetFunctionNameIfUnnamed);
        emit_u16(add_name(class_name));
    }
    emit(Op::Star);
    emit_u8(static_cast<uint8_t>(ctor_reg));

    emit(Op::BuildClass);
    emit_u8(static_cast<uint8_t>(ctor_reg));
    emit_u8(static_cast<uint8_t>(proto_reg));
    emit_u8(ctor ? 0 : 0x1);

    // Every private name the class writes, recorded before any member is
    // built: a member's body may name one declared further down.
    bool has_private = false;
    for (const auto& se : static_elements) {
        if (se.key.empty() || se.key[0] != '#') continue;
        has_private = true;
        emit(Op::DeclarePrivateName);
        emit_u8(static_cast<uint8_t>(ctor_reg));
        emit_u8(static_cast<uint8_t>(proto_reg));
        emit_u16(add_name(se.key));
        emit_u8(0x1);
    }
    for (const auto& e : elements) {
        if (e.key.empty() || e.key[0] != '#') continue;
        has_private = true;
        emit(Op::DeclarePrivateName);
        emit_u8(static_cast<uint8_t>(ctor_reg));
        emit_u8(static_cast<uint8_t>(proto_reg));
        emit_u16(add_name(e.key));
        emit_u8(static_cast<uint8_t>((e.is_static ? 0x1 : 0) | (e.is_field ? 0 : 0x2)));
    }

    // The inner name is settled before the members are built, so a member's
    // body sees the class the moment it can run.
    if (class_env_idx >= 0) {
        emit(Op::Ldar);
        emit_u8(static_cast<uint8_t>(ctor_reg));
        emit(Op::StaEnvInit);
        emit_u16(add_name(inner_name));
    }

    constexpr uint8_t kClassElementFlag = 0x8;
    constexpr uint8_t kSuperFreeFlag = 0x4;
    for (const auto& e : elements) {
        // A computed key is evaluated here, in the order it was written, and
        // exactly once: what it comes to is what the member is installed under
        // and, for a field, what every instance gets.
        int raw_key_reg = -1, key_reg = -1;
        if (e.key_expr) {
            raw_key_reg = alloc_temp();
            if (failed_) return false;
            if (!compile_expression(e.key_expr)) { failed_ = true; return false; }
            emit(Op::Star);
            emit_u8(static_cast<uint8_t>(raw_key_reg));
            key_reg = alloc_temp();
            if (failed_) return false;
            emit(Op::Ldar);
            emit_u8(static_cast<uint8_t>(raw_key_reg));
            emit(Op::ToPropertyKeyStrict);
            emit(Op::Star);
            emit_u8(static_cast<uint8_t>(key_reg));
        }

        if (e.is_field) {
            // Before the body is taken: what the initializer builds is named
            // from where it is written, and the name has to be on the node the
            // closure is made from.
            bool name_result = false;
            if (e.init && !e.key.empty() && is_named_evaluation_rhs(e.init)) {
                stamp_inferred_class_name(e.init, e.key);
                name_result = !named_evaluation_is_class(e.init);
            }
            if (e.init) {
                // The initializer is a function of its own: it runs with `this`
                // bound to the instance being built, which is why it cannot be
                // emitted here alongside the class.
                ClosureTemplate tmpl;
                auto exe = make_executable_ref();
                // Owned, not borrowed: the tree this was written in can be a
                // copy the class's own evaluation made and then dropped, and a
                // borrowed body would outlive it. The copy keeps pointing at
                // the source, which the executable holds alive for it.
                ExecutableRef<ScriptUnit> unit(cls->source_unit());
                {
                    ast_detail::CloneSourceScope keep_source;
                    exe->adopt_body_from(e.init->clone(), unit);
                }
                tmpl.executable = std::move(exe);
                tmpl.form = ClosureTemplate::Form::FunctionExpr;
                tmpl.body_is_strict = true;
                tmpl.is_method_shorthand = true;  // no .prototype, not a constructor
                if (chunk_->ensure_closures().size() >= 0xFFFF) return false;
                chunk_->ensure_closures().push_back(std::move(tmpl));
                emit(Op::CreateClosure);
                emit_u16(static_cast<uint16_t>(chunk_->ensure_closures().size() - 1));
            } else {
                emit(Op::LdaUndefined);
            }
            if (e.key_expr) {
                emit(Op::AddFieldInitializerKeyed);
                emit_u8(static_cast<uint8_t>(ctor_reg));
                emit_u8(static_cast<uint8_t>(key_reg));
                emit_u8(0);
            } else {
                emit(Op::AddFieldInitializer);
                emit_u8(static_cast<uint8_t>(ctor_reg));
                emit_u16(add_name(e.key));
                emit_u8(static_cast<uint8_t>((name_result ? 0x1 : 0) |
                                             (e.key[0] == '#' ? 0x2 : 0)));
            }
        } else {
            // Asked before the value is compiled: compiling it is what lets
            // go of the body this reads (same ordering as the object-literal
            // compiler's own keeps_super check).
            const bool keeps_super = method_value_references_super(e.m->get_value());
            {
                ClosureTemplate tmpl = closure_template_for(e.m->get_value());
                // A class writes `async m()` as an ordinary function literal
                // carrying the async flag, whereas everywhere else an async
                // function arrives as its own node. instantiate_closure reads
                // the form, so say which one this is, or the method is built
                // as a plain function that returns its body's value instead of
                // a promise.
                if (tmpl.is_async && !tmpl.is_generator) {
                    tmpl.form = ClosureTemplate::Form::AsyncFunctionExpr;
                }
                chunk_->ensure_closures().push_back(std::move(tmpl));
            }
            emit(Op::CreateClosure);
            emit_u16(static_cast<uint16_t>(chunk_->ensure_closures().size() - 1));
            // An instance member belongs to the prototype, a static one to the
            // constructor -- which is also what each homes on for `super`.
            const uint8_t target = static_cast<uint8_t>(e.is_static ? ctor_reg : proto_reg);
            if (e.key_expr) {
                emit(e.kind == 0 ? Op::FinalizeComputedProperty : Op::FinalizeComputedAccessor);
                emit_u8(target);
                emit_u8(static_cast<uint8_t>(key_reg));
                emit_u8(static_cast<uint8_t>(raw_key_reg));
                // The plain-key form spells a method 0; the computed one
                // spells it 2, keeping 0 for a property that is not one.
                emit_u8(static_cast<uint8_t>((e.kind == 0 ? 2 : e.kind) | kClassElementFlag |
                                             (e.is_static ? 0x10 : 0) |
                                             (!keeps_super ? kSuperFreeFlag : 0)));
            } else if (e.key[0] == '#') {
                emit(Op::DefinePrivateMember);
                emit_u8(target);
                emit_u16(add_name(e.key));
                emit_u8(e.kind);
            } else {
                emit(Op::FinalizeStaticProperty);
                emit_u8(target);
                emit_u16(add_name(e.key));
                // An accessor's function is named for how it is reached, not
                // for the key alone.
                emit_u16(add_name(e.kind == 1 ? "get " + e.key
                                : e.kind == 2 ? "set " + e.key
                                              : e.key));
                // FinalizeStaticProperty's super_free bit is only consulted
                // for a method (kind 0); a getter/setter here always writes
                // [[HomeObject]], matching the object-literal compiler's own
                // plain-key form.
                emit_u8(static_cast<uint8_t>(e.kind | kClassElementFlag |
                                             (e.kind == 0 && !keeps_super ? kSuperFreeFlag : 0)));
                emit_u16(alloc_feedback_slot());
            }
        }
        if (raw_key_reg >= 0) free_temp(raw_key_reg);
    }

    // After the members, so each of them is handed the superclass its `super`
    // resolves against.
    if (superclass) {
        emit(Op::Ldar);
        emit_u8(static_cast<uint8_t>(super_reg));
        emit(Op::LinkClassHeritage);
        emit_u8(static_cast<uint8_t>(ctor_reg));
        emit_u8(static_cast<uint8_t>(proto_reg));
    }

    // After the heritage: what a member has to check a brand against depends on
    // whether the class is derived, which the link above is what settles.
    if (has_private) {
        emit(Op::LinkPrivateBrands);
        emit_u8(static_cast<uint8_t>(ctor_reg));
        emit_u8(static_cast<uint8_t>(proto_reg));
    }

    // Last, and in the order they were written: a static element sees the
    // finished class, its members and its heritage.
    for (const auto& se : static_elements) {
        // NamedEvaluation: only a function written anonymously right here takes
        // the field's name. A name that merely holds one keeps its own, which
        // is why the shape is asked at compile time and not the value at run
        // time -- and it is asked before the body is taken.
        bool name_static = false;
        if (se.body && !se.key.empty() && is_named_evaluation_rhs(se.body)) {
            stamp_inferred_class_name(se.body, se.key);
            name_static = !named_evaluation_is_class(se.body);
        }
        if (se.body) {
            ClosureTemplate tmpl;
            auto exe = make_executable_ref();
            ExecutableRef<ScriptUnit> unit(cls->source_unit());
            {
                ast_detail::CloneSourceScope keep_source;
                exe->adopt_body_from(se.body->clone(), unit);
            }
            tmpl.executable = std::move(exe);
            tmpl.form = ClosureTemplate::Form::FunctionExpr;
            tmpl.body_is_strict = true;
            tmpl.is_method_shorthand = true;
            if (chunk_->ensure_closures().size() >= 0xFFFF) return false;
            chunk_->ensure_closures().push_back(std::move(tmpl));
            emit(Op::CreateClosure);
            emit_u16(static_cast<uint16_t>(chunk_->ensure_closures().size() - 1));
            emit(Op::RunStaticElement);
            emit_u8(static_cast<uint8_t>(ctor_reg));
        } else {
            emit(Op::LdaUndefined);
        }
        if (!se.key.empty()) {
            if (name_static) {
                emit(Op::SetFunctionNameIfUnnamed);
                emit_u16(add_name(se.key));
            }
            if (se.key[0] == '#') {
                emit(Op::DefinePrivateStatic);
                emit_u8(static_cast<uint8_t>(ctor_reg));
                emit_u16(add_name(se.key));
            } else {
                emit(Op::DefineOwn);
                emit_u8(static_cast<uint8_t>(ctor_reg));
                emit_u16(add_name(se.key));
                emit_u16(alloc_feedback_slot());
            }
        }
    }

    if (class_env_idx >= 0) {
        emit(Op::ExitLoopEnv);
        env_depth_--;
    }

    emit(Op::Ldar);
    emit_u8(static_cast<uint8_t>(ctor_reg));
    if (bind_name) {
        // Same two steps Op::DefineClass took: the scope-chain binding, which
        // is where a reader of the name looks, and then the register when the
        // name has one.
        emit(Op::BindClassName);
        emit_u16(add_name(class_name));
        if (!env_names_.count(class_name) && lookup_local(class_name) >= 0) {
            emit_write_local(class_name, /*is_declaration=*/true);
        }
    }
    // The class is in the accumulator by now; both temps go back, LIFO, so a
    // call compiling this as one of its arguments keeps its argument run
    // contiguous.
    free_temp(proto_reg);
    return true;
}

// Whether evaluating `node` provably cannot store to any of this frame's
// registers. A whitelist: a literal computes nothing, and reading a name never
// writes one back -- a global read can run a getter, but a getter cannot reach
// a register-resident local, since anything a closure captures is env-resident.
bool BytecodeCompiler::operand_cannot_write_registers(const ASTNode* node) {
    if (!node) return false;
    switch (node->get_type()) {
        case ASTNode::Type::NUMBER_LITERAL:
        case ASTNode::Type::STRING_LITERAL:
        case ASTNode::Type::BOOLEAN_LITERAL:
        case ASTNode::Type::NULL_LITERAL:
        case ASTNode::Type::UNDEFINED_LITERAL:
        case ASTNode::Type::BIGINT_LITERAL:
        case ASTNode::Type::IDENTIFIER:
            return true;
        // Arithmetic over safe operands stays safe: writing to a register-
        // resident local takes an assignment lexically inside this function,
        // and there is none here. An operand's valueOf can run arbitrary code
        // but still cannot reach one, for the same reason a call cannot.
        case ASTNode::Type::BINARY_EXPRESSION: {
            const auto* b = static_cast<const BinaryExpression*>(node);
            return operand_cannot_write_registers(b->get_left()) &&
                   operand_cannot_write_registers(b->get_right());
        }
        default:
            return false;
    }
}

bool BytecodeCompiler::compile_expression(const ASTNode* node, bool discard) {
    if (!node || failed_) return false;

    // Optional chaining: once any link's base is nullish, skip the rest of
    // the chain and produce undefined (detected once per chain, then
    // re-enters here with chain_shortcircuit_jumps_ already set).
    if (!chain_shortcircuit_jumps_ &&
        (node->get_type() == ASTNode::Type::MEMBER_EXPRESSION ||
         node->get_type() == ASTNode::Type::OPTIONAL_CHAINING_EXPRESSION ||
         node->get_type() == ASTNode::Type::CALL_EXPRESSION) &&
        chain_contains_optional(node)) {
        std::vector<size_t> local_jumps;
        chain_shortcircuit_jumps_ = &local_jumps;
        bool ok = compile_expression(node);
        chain_shortcircuit_jumps_ = nullptr;
        if (!ok) return false;
        if (!local_jumps.empty()) {
            size_t skip = emit_jump(Op::Jump);
            for (size_t pos : local_jumps) {
                if (!patch_jump(pos)) return false;
            }
            emit(Op::LdaUndefined);
            if (!patch_jump(skip)) return false;
        }
        return true;
    }

    switch (node->get_type()) {
        case ASTNode::Type::NUMBER_LITERAL: {
            double v = static_cast<const NumberLiteral*>(node)->get_value();
            if (v == 0.0 && !std::signbit(v)) {
                emit(Op::LdaZero);
            } else if (v == std::trunc(v) && v >= INT8_MIN && v <= INT8_MAX &&
                       !(v == 0.0 && std::signbit(v))) {
                emit(Op::LdaSmi);
                emit_u8(static_cast<uint8_t>(static_cast<int8_t>(v)));
            } else {
                emit_load_const(Value(v));
            }
            return !failed_;
        }
        case ASTNode::Type::STRING_LITERAL: {
            emit_load_const(Value(static_cast<const StringLiteral*>(node)->get_value()));
            return !failed_;
        }
        case ASTNode::Type::META_PROPERTY: {
            const auto* n = static_cast<const MetaProperty*>(node);
            if (n->get_meta() == "new" && n->get_property() == "target") {
                emit(Op::LdaNewTarget);
                return !failed_;
            }
            if (n->get_meta() == "import" && n->get_property() == "meta") {
                emit(Op::LdaImportMeta);
                return !failed_;
            }
            return false;
        }
        case ASTNode::Type::BIGINT_LITERAL: {
            // A BigInt has no identity anything can observe, so one cell in
            // the constant pool serves every evaluation, as a string's does.
            const std::string& text = static_cast<const BigIntLiteral*>(node)->get_value();
            BigInt* parsed = nullptr;
            try {
                parsed = new BigInt(text);
            } catch (const std::exception&) {
                return false;
            }
            emit_load_const(Value(parsed));
            return !failed_;
        }
        case ASTNode::Type::TEMPLATE_LITERAL: {
            const auto& elements = static_cast<const TemplateLiteral*>(node)->get_elements();
            using Elem = TemplateLiteral::Element;
            int result_reg = alloc_temp();
            if (failed_) return false;
            // The first piece is the result so far -- it does not need adding
            // to anything. Seeding with an empty string instead cost every
            // template literal one whole addition, and an addition here is
            // the general one, with both operands' types to work out first.
            bool seeded = false;
            for (const auto& el : elements) {
                if (el.type == Elem::Type::TEXT) {
                    if (el.text.empty()) continue;
                    emit_load_const(Value(el.text));
                } else {
                    if (!compile_expression(el.expression.get())) return false;
                    emit(Op::ToTemplateString);
                }
                if (seeded) {
                    emit(Op::Add);
                    emit_u8(static_cast<uint8_t>(result_reg));
                }
                emit(Op::Star);
                emit_u8(static_cast<uint8_t>(result_reg));
                seeded = true;
            }
            // Nothing but empty text: the result is the empty string.
            if (!seeded) {
                emit_load_const(Value(std::string()));
                emit(Op::Star);
                emit_u8(static_cast<uint8_t>(result_reg));
            }
            emit(Op::Ldar);
            emit_u8(static_cast<uint8_t>(result_reg));
            free_temp(result_reg);
            return !failed_;
        }

        case ASTNode::Type::REGEX_LITERAL: {
            const auto* re = static_cast<const RegexLiteral*>(node);
            emit(Op::CreateRegExp);
            emit_u16(add_name(re->get_pattern()));
            emit_u16(add_name(re->get_flags()));
            return !failed_;
        }

        case ASTNode::Type::BOOLEAN_LITERAL:
            emit(static_cast<const BooleanLiteral*>(node)->get_value() ? Op::LdaTrue : Op::LdaFalse);
            return true;
        case ASTNode::Type::NULL_LITERAL:
            emit(Op::LdaNull);
            return true;
        case ASTNode::Type::UNDEFINED_LITERAL:
            emit(Op::LdaUndefined);
            return true;

        case ASTNode::Type::ENGINE_HELPER:
            emit(Op::LdaEngineHelper);
            emit_u8(static_cast<uint8_t>(static_cast<const EngineHelper*>(node)->get_kind()));
            return true;

        case ASTNode::Type::IDENTIFIER: {
            const std::string& name = static_cast<const Identifier*>(node)->get_name();
            if (name == "this") {
                // Op::LdaThis carries the derived-constructor TDZ check itself, so
                // a `this` read before super() throws from there.
                emit(Op::LdaThis);
                return true;
            }
            if (with_depth_ > 0) {
                // The with object is asked first, whatever the name would
                // otherwise resolve to.
                emit(Op::LdaWith);
                emit_u16(add_name(name));
                emit_u8(0);
                return !failed_;
            }
            if (is_local(name) && !lexical_out_of_scope(name)) {
                emit_read_local(name);
                return !failed_;
            }
            // Binding magic beyond a plain lookup stays on the tree-walker.
            // ("this" isn't here -- Function::call already resolves it;
            // "arguments" is a plain lookup too once needs_arguments made
            // Function::call materialize the binding.)
            // Reading `eval` is an ordinary global lookup; only a direct call
            // through that name needs the caller's scope, and the call site
            // refuses that on its own. What comes out of a read can only be
            // called indirectly, which runs in global scope anyway.
            if (name == "arguments" && !allow_arguments_) return false;
            if (name == "super" || name == "new") return false;
            emit(Op::LdaLookup);
            emit_u16(add_name(name));
            return !failed_;
        }

        case ASTNode::Type::MEMBER_EXPRESSION: {
            const auto* mem = static_cast<const MemberExpression*>(node);
            const bool priv = member_is_private(mem);
            if (member_is_super(mem)) {
                if (!super_member_emittable(mem)) return false;
                return emit_super_load(mem);
            }
            if (!priv && !member_is_supported(mem)) return false;
            // A receiver that already sits in a register is its own operand.
            // Loading it into the accumulator only to copy it back out into a
            // temporary was two opcodes per property read, and a chain like
            // `u.a` inside a loop pays it on every iteration.
            int borrowed_reg = -1;
            if (mem->get_object()->get_type() == ASTNode::Type::IDENTIFIER) {
                borrowed_reg = plain_local_register(
                    static_cast<const Identifier*>(mem->get_object())->get_name());
            }
            int obj_reg = borrowed_reg;
            if (obj_reg < 0) {
                if (!compile_expression(mem->get_object())) return false;
                obj_reg = alloc_temp();
                if (failed_) return false;
                emit(Op::Star);
                emit_u8(static_cast<uint8_t>(obj_reg));
            }

            if (!mem->is_computed()) {
                const std::string& name = static_cast<const Identifier*>(mem->get_property())->get_name();
                emit(priv ? Op::GetPrivate : Op::GetNamed);
                emit_u8(static_cast<uint8_t>(obj_reg));
                emit_u16(add_name(name));
                emit_u16(priv ? alloc_private_feedback() : alloc_feedback_slot());
            } else {
                // Evaluating the key leaves it in the accumulator, exactly
                // what GetKeyed expects -- no extra register needed for reads.
                {
                    ChainMaskScope mask(chain_shortcircuit_jumps_);
                    if (!compile_expression(mem->get_property())) return false;
                }
                emit(Op::GetKeyed);
                emit_u8(static_cast<uint8_t>(obj_reg));
                emit_u16(alloc_keyed_feedback());
            }
            if (borrowed_reg < 0) free_temp(obj_reg);
            return !failed_;
        }

        case ASTNode::Type::OPTIONAL_CHAINING_EXPRESSION: {
            const auto* mem = static_cast<const OptionalChainingExpression*>(node);
            if (mem->get_object()->get_type() == ASTNode::Type::IDENTIFIER &&
                static_cast<const Identifier*>(mem->get_object())->get_name() == "super") {
                return false;
            }
            bool priv = false;
            if (!mem->is_computed()) {
                if (mem->get_property()->get_type() != ASTNode::Type::IDENTIFIER) return false;
                const std::string& name = static_cast<const Identifier*>(mem->get_property())->get_name();
                priv = !name.empty() && name[0] == '#';
            }
            if (!compile_expression(mem->get_object())) return false;
            int obj_reg = alloc_temp();
            if (failed_) return false;
            emit(Op::Star);
            emit_u8(static_cast<uint8_t>(obj_reg));

            if (!chain_shortcircuit_jumps_) return false;
            emit(Op::Ldar);
            emit_u8(static_cast<uint8_t>(obj_reg));
            chain_shortcircuit_jumps_->push_back(emit_jump(Op::JumpIfNullish));

            if (!mem->is_computed()) {
                const std::string& name = static_cast<const Identifier*>(mem->get_property())->get_name();
                emit(priv ? Op::GetPrivate : Op::GetNamed);
                emit_u8(static_cast<uint8_t>(obj_reg));
                emit_u16(add_name(name));
                emit_u16(priv ? alloc_private_feedback() : alloc_feedback_slot());
            } else {
                {
                    ChainMaskScope mask(chain_shortcircuit_jumps_);
                    if (!compile_expression(mem->get_property())) return false;
                }
                emit(Op::GetKeyed);
                emit_u8(static_cast<uint8_t>(obj_reg));
                emit_u16(alloc_keyed_feedback());
            }
            free_temp(obj_reg);
            return !failed_;
        }

        case ASTNode::Type::NULLISH_COALESCING_EXPRESSION: {
            const auto* expr = static_cast<const NullishCoalescingExpression*>(node);
            if (!compile_expression(expr->get_left())) return false;
            size_t skip = emit_jump(Op::JumpIfNotNullish);
            if (!compile_expression(expr->get_right())) return false;
            return patch_jump(skip);
        }

        case ASTNode::Type::BINARY_EXPRESSION: {
            const auto* expr = static_cast<const BinaryExpression*>(node);
            using BinOp = BinaryExpression::Operator;
            BinOp op = expr->get_operator();

            if (op == BinOp::LOGICAL_AND || op == BinOp::LOGICAL_OR) {
                if (!compile_expression(expr->get_left())) return false;
                size_t skip = emit_jump(op == BinOp::LOGICAL_AND ? Op::JumpIfFalse
                                                                 : Op::JumpIfTrue);
                if (!compile_expression(expr->get_right())) return false;
                return patch_jump(skip);
            }
            if (op == BinOp::COMMA) {
                return compile_expression(expr->get_left()) &&
                       compile_expression(expr->get_right());
            }
            if (op == BinOp::IN && expr->get_left()->get_type() == ASTNode::Type::IDENTIFIER &&
                !static_cast<const Identifier*>(expr->get_left())->get_name().empty() &&
                static_cast<const Identifier*>(expr->get_left())->get_name()[0] == '#') {
                // `#name in obj`: a brand check, not a property lookup, so it
                // gets its own opcode rather than going through TestIn.
                if (!compile_expression(expr->get_right())) return false;
                emit(Op::HasPrivate);
                emit_u16(add_name(static_cast<const Identifier*>(expr->get_left())->get_name()));
                return !failed_;
            }

            Op vm_op;
            switch (op) {
                case BinOp::ADD:                  vm_op = Op::Add; break;
                case BinOp::SUBTRACT:             vm_op = Op::Sub; break;
                case BinOp::MULTIPLY:             vm_op = Op::Mul; break;
                case BinOp::DIVIDE:               vm_op = Op::Div; break;
                case BinOp::MODULO:               vm_op = Op::Mod; break;
                case BinOp::EXPONENT:             vm_op = Op::Exp; break;
                case BinOp::BITWISE_AND:          vm_op = Op::BitAnd; break;
                case BinOp::BITWISE_OR:           vm_op = Op::BitOr; break;
                case BinOp::BITWISE_XOR:          vm_op = Op::BitXor; break;
                case BinOp::LEFT_SHIFT:           vm_op = Op::Shl; break;
                case BinOp::RIGHT_SHIFT:          vm_op = Op::Sar; break;
                case BinOp::UNSIGNED_RIGHT_SHIFT: vm_op = Op::Shr; break;
                case BinOp::EQUAL:                vm_op = Op::TestEq; break;
                case BinOp::NOT_EQUAL:            vm_op = Op::TestNe; break;
                case BinOp::STRICT_EQUAL:         vm_op = Op::TestStrictEq; break;
                case BinOp::STRICT_NOT_EQUAL:     vm_op = Op::TestStrictNe; break;
                case BinOp::LESS_THAN:            vm_op = Op::TestLt; break;
                case BinOp::GREATER_THAN:         vm_op = Op::TestGt; break;
                case BinOp::LESS_EQUAL:           vm_op = Op::TestLe; break;
                case BinOp::GREATER_EQUAL:        vm_op = Op::TestGe; break;
                case BinOp::INSTANCEOF:           vm_op = Op::TestInstanceOf; break;
                case BinOp::IN:                   vm_op = Op::TestIn; break;
                default:
                    return false;
            }
            // `vm_op reg` reads regs[reg] as the left operand, so a left side
            // already living in a register needs no copy into a temp. It is
            // read after the right side has run, so this holds only while the
            // right side cannot write to it; anything else keeps the copy.
            int left_reg = -1;
            if (expr->get_left()->get_type() == ASTNode::Type::IDENTIFIER) {
                const std::string& lname =
                    static_cast<const Identifier*>(expr->get_left())->get_name();
                int reg = env_names_.count(lname) ? -1 : lookup_local(lname);
                bool tdz_free = !lexical_registers_.count(reg) ||
                                initialized_lexicals_.count(reg) > 0;
                if (reg >= 0 && tdz_free &&
                    operand_cannot_write_registers(expr->get_right())) {
                    left_reg = reg;
                }
            }
            if (left_reg >= 0) {
                if (!compile_expression(expr->get_right())) return false;
                emit(vm_op);
                emit_u8(static_cast<uint8_t>(left_reg));
                return !failed_;
            }
            if (!compile_expression(expr->get_left())) return false;
            int temp = alloc_temp();
            if (failed_) return false;
            emit(Op::Star);
            emit_u8(static_cast<uint8_t>(temp));
            if (!compile_expression(expr->get_right())) return false;
            emit(vm_op);
            emit_u8(static_cast<uint8_t>(temp));
            free_temp(temp);
            return true;
        }

        case ASTNode::Type::UNARY_EXPRESSION: {
            const auto* expr = static_cast<const UnaryExpression*>(node);
            using UnOp = UnaryExpression::Operator;
            switch (expr->get_operator()) {
                case UnOp::PLUS:
                    if (!compile_expression(expr->get_operand())) return false;
                    emit(Op::ToNumber);
                    return true;
                case UnOp::MINUS:
                    if (!compile_expression(expr->get_operand())) return false;
                    emit(Op::Neg);
                    return true;
                case UnOp::LOGICAL_NOT:
                    if (!compile_expression(expr->get_operand())) return false;
                    emit(Op::LogicalNot);
                    return true;
                case UnOp::BITWISE_NOT:
                    if (!compile_expression(expr->get_operand())) return false;
                    emit(Op::BitNot);
                    return true;
                case UnOp::TYPEOF: {
                    // `typeof x` must not throw for an unresolved global
                    // (TDZ still throws) -- LdaLookupTypeof handles that.
                    const ASTNode* operand = expr->get_operand();
                    if (operand->get_type() == ASTNode::Type::IDENTIFIER) {
                        const std::string& name = static_cast<const Identifier*>(operand)->get_name();
                        if (name == "this") {
                            // Register frames no longer bind "this" -- a chain
                            // lookup would find some outer frame's binding.
                            emit(Op::LdaThis);
                        } else if (with_depth_ > 0) {
                            // The with object is asked first, local or not.
                            emit(Op::LdaWith);
                            emit_u16(add_name(name));
                            emit_u8(1);  // a miss is undefined, not a throw
                        } else if (is_local(name) && !lexical_out_of_scope(name)) {
                            emit_read_local(name);
                        } else if ((name == "arguments" && !allow_arguments_) ||
                                   name == "super" || name == "new") {
                            return false;
                        } else {
                            emit(Op::LdaLookupTypeof);
                            emit_u16(add_name(name));
                        }
                    } else if (!compile_expression(operand)) {
                        return false;
                    }
                    emit(Op::TypeOf);
                    return true;
                }
                case UnOp::VOID:
                    if (!compile_expression(expr->get_operand())) return false;
                    emit(Op::LdaUndefined);
                    return true;
                case UnOp::PRE_INCREMENT:
                case UnOp::PRE_DECREMENT:
                case UnOp::POST_INCREMENT:
                case UnOp::POST_DECREMENT: {
                    const ASTNode* operand = expr->get_operand();
                    bool is_inc = expr->get_operator() == UnOp::PRE_INCREMENT ||
                                  expr->get_operator() == UnOp::POST_INCREMENT;
                    // Discarded, a postfix update is a prefix one: both read,
                    // coerce with ToNumeric and store the same way, and differ
                    // only in which of the two values they leave behind.
                    bool is_post = !discard &&
                                   (expr->get_operator() == UnOp::POST_INCREMENT ||
                                    expr->get_operator() == UnOp::POST_DECREMENT);

                    if (operand->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
                        const auto* mem = static_cast<const MemberExpression*>(operand);
                        const bool priv = member_is_private(mem);
                        if (member_is_super(mem)) {
                            if (!super_member_emittable(mem)) return false;
                            int base_reg = alloc_temp();
                            if (failed_) return false;
                            emit(Op::ResolveSuperBase);
                            emit_u8(static_cast<uint8_t>(base_reg));

                            uint16_t sname = 0;
                            int skey_reg = -1;
                            if (mem->is_computed()) {
                                if (!compile_expression(mem->get_property())) return false;
                                skey_reg = alloc_temp();
                                if (failed_) return false;
                                emit(Op::ToPropertyKey);  // once; the read and write reuse the string
                                emit(Op::Star);
                                emit_u8(static_cast<uint8_t>(skey_reg));
                                emit(Op::GetSuperKeyed);
                                emit_u8(static_cast<uint8_t>(base_reg));
                            } else {
                                sname = add_name(
                                    static_cast<const Identifier*>(mem->get_property())->get_name());
                                emit(Op::GetSuper);
                                emit_u16(sname);
                            }

                            int sold = -1;
                            if (is_post) {
                                emit(Op::ToNumeric);
                                sold = alloc_temp();
                                if (failed_) return false;
                                emit(Op::Star);
                                emit_u8(static_cast<uint8_t>(sold));
                            }
                            emit(is_inc ? Op::Inc : Op::Dec);
                            if (skey_reg >= 0) {
                                emit(Op::SetSuperKeyed);
                                emit_u8(static_cast<uint8_t>(base_reg));
                                emit_u8(static_cast<uint8_t>(skey_reg));
                            } else {
                                emit(Op::SetSuper);
                                emit_u8(static_cast<uint8_t>(base_reg));
                                emit_u16(sname);
                            }
                            if (is_post) {
                                emit(Op::Ldar);
                                emit_u8(static_cast<uint8_t>(sold));
                                free_temp(sold);
                            }
                            if (skey_reg >= 0) free_temp(skey_reg);
                            free_temp(base_reg);
                            return !failed_;
                        }
                        if (!priv && !member_is_supported(mem)) return false;
                        if (chain_contains_optional(mem)) return false;

                        if (!compile_expression(mem->get_object())) return false;
                        int obj_reg = alloc_temp();
                        if (failed_) return false;
                        emit(Op::Star);
                        emit_u8(static_cast<uint8_t>(obj_reg));

                        uint16_t name_idx = 0;
                        int key_reg = -1;
                        if (!mem->is_computed()) {
                            name_idx = add_name(
                                static_cast<const Identifier*>(mem->get_property())->get_name());
                            emit(priv ? Op::GetPrivate : Op::GetNamed);
                            emit_u8(static_cast<uint8_t>(obj_reg));
                            emit_u16(name_idx);
                            emit_u16(priv ? alloc_private_feedback() : alloc_feedback_slot());
                        } else {
                            if (!compile_expression(mem->get_property())) return false;
                            key_reg = alloc_temp();
                            if (failed_) return false;
                            emit(Op::Star);
                            emit_u8(static_cast<uint8_t>(key_reg));
                            // Spec: CheckObjectCoercible(base) before
                            // ToPropertyKey(key) for ++/--'s GetValue step.
                            emit(Op::Ldar);
                            emit_u8(static_cast<uint8_t>(obj_reg));
                            emit(Op::CheckObjectCoercible);
                            emit(Op::Ldar);
                            emit_u8(static_cast<uint8_t>(key_reg));
                            emit(Op::ToPropertyKey);  // once; GetKeyed/SetKeyed reuse the string
                            emit(Op::Star);
                            emit_u8(static_cast<uint8_t>(key_reg));
                            emit(Op::Ldar);
                            emit_u8(static_cast<uint8_t>(key_reg));
                            emit(Op::GetKeyed);  // key still in the accumulator
                            emit_u8(static_cast<uint8_t>(obj_reg));
                            emit_u16(alloc_keyed_feedback());
                        }

                        int old_temp = -1;
                        if (is_post) {
                            emit(Op::ToNumeric);
                            old_temp = alloc_temp();
                            if (failed_) return false;
                            emit(Op::Star);
                            emit_u8(static_cast<uint8_t>(old_temp));
                        }
                        emit(is_inc ? Op::Inc : Op::Dec);
                        if (!mem->is_computed()) {
                            emit(priv ? Op::SetPrivate : Op::SetNamed);
                            emit_u8(static_cast<uint8_t>(obj_reg));
                            emit_u16(name_idx);
                            emit_u16(priv ? alloc_private_feedback() : alloc_feedback_slot());
                        } else {
                            emit(Op::SetKeyed);
                            emit_u8(static_cast<uint8_t>(obj_reg));
                            emit_u8(static_cast<uint8_t>(key_reg));
                            emit_u16(alloc_keyed_feedback());
                        }
                        if (is_post) {
                            emit(Op::Ldar);
                            emit_u8(static_cast<uint8_t>(old_temp));
                            free_temp(old_temp);
                        }
                        if (key_reg >= 0) free_temp(key_reg);
                        free_temp(obj_reg);
                        return !failed_;
                    }

                    if (operand->get_type() != ASTNode::Type::IDENTIFIER) return false;
                    const std::string& name = static_cast<const Identifier*>(operand)->get_name();
                    if (with_depth_ > 0 ||
                        (outer_with_ && (!is_local(name) || lexical_out_of_scope(name)))) {
                        // One resolution serves both the read and the write,
                        // which is what the spec's single Reference means.
                        emit(Op::ResolveWithTarget);
                        emit_u16(add_name(name));
                        int target = alloc_temp();
                        if (failed_) return false;
                        emit(Op::Star);
                        emit_u8(static_cast<uint8_t>(target));
                        emit(Op::LdaWithResolved);
                        emit_u8(static_cast<uint8_t>(target));
                        emit_u16(add_name(name));
                        int old_reg = -1;
                        if (is_post) {
                            emit(Op::ToNumeric);
                            old_reg = alloc_temp();
                            if (failed_) return false;
                            emit(Op::Star);
                            emit_u8(static_cast<uint8_t>(old_reg));
                        }
                        emit(is_inc ? Op::Inc : Op::Dec);
                        emit(Op::StaWithResolved);
                        emit_u8(static_cast<uint8_t>(target));
                        emit_u16(add_name(name));
                        if (is_post) {
                            emit(Op::Ldar);
                            emit_u8(static_cast<uint8_t>(old_reg));
                            free_temp(old_reg);
                        }
                        free_temp(target);
                        return !failed_;
                    }
                    if (!is_local(name) || lexical_out_of_scope(name)) {
                        emit(Op::LdaLookup);
                        emit_u16(add_name(name));
                        if (is_post) {
                            emit(Op::ToNumeric);
                            int temp = alloc_temp();
                            if (failed_) return false;
                            emit(Op::Star);
                            emit_u8(static_cast<uint8_t>(temp));
                            emit(is_inc ? Op::Inc : Op::Dec);
                            emit(Op::StaLookup);
                            emit_u16(add_name(name));
                            emit(Op::Ldar);
                            emit_u8(static_cast<uint8_t>(temp));
                            free_temp(temp);
                        } else {
                            emit(is_inc ? Op::Inc : Op::Dec);
                            emit(Op::StaLookup);
                            emit_u16(add_name(name));
                        }
                        return !failed_;
                    }
                    emit_read_local(name);
                    if (is_post) {
                        // Postfix result is the OLD value (as a numeric):
                        // acc = ToNumeric(old), reg = acc +/- 1, acc stays old.
                        emit(Op::ToNumeric);
                        int temp = alloc_temp();
                        if (failed_) return false;
                        emit(Op::Star);
                        emit_u8(static_cast<uint8_t>(temp));
                        emit(is_inc ? Op::Inc : Op::Dec);
                        emit_write_local(name, /*is_declaration=*/false);
                        emit(Op::Ldar);
                        emit_u8(static_cast<uint8_t>(temp));
                        free_temp(temp);
                    } else {
                        emit(is_inc ? Op::Inc : Op::Dec);
                        emit_write_local(name, /*is_declaration=*/false);
                    }
                    return !failed_;
                }
                case UnOp::DELETE: {
                    const ASTNode* operand = expr->get_operand();
                    if (operand->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
                        const auto* mem = static_cast<const MemberExpression*>(operand);
                        if (member_is_super(mem)) {
                            // The reference is still evaluated -- the this
                            // binding, the super base and the key's own
                            // ToPropertyKey all run -- and only then does
                            // delete on a super reference throw.
                            if (!super_member_emittable(mem)) return false;
                            if (chain_contains_optional(mem)) return false;
                            int base_reg = alloc_temp();
                            if (failed_) return false;
                            emit(Op::ResolveSuperBase);
                            emit_u8(static_cast<uint8_t>(base_reg));
                            // The key expression runs, but MakeSuperPropertyReference
                            // keeps it unconverted -- ToPropertyKey belongs to the
                            // GetValue/PutValue that never happens here.
                            if (mem->is_computed() &&
                                !compile_expression(mem->get_property(), /*discard=*/true)) {
                                return false;
                            }
                            free_temp(base_reg);
                            emit(Op::ThrowSuperDelete);
                            return !failed_;
                        }
                        // The private form still needs the brand ceremony.
                        if (!member_is_supported(mem)) return false;
                        if (chain_contains_optional(mem)) return false;
                        if (!compile_expression(mem->get_object())) return false;
                        int obj_reg = alloc_temp();
                        if (failed_) return false;
                        emit(Op::Star);
                        emit_u8(static_cast<uint8_t>(obj_reg));
                        if (!mem->is_computed()) {
                            emit(Op::DeleteNamed);
                            emit_u8(static_cast<uint8_t>(obj_reg));
                            emit_u16(add_name(
                                static_cast<const Identifier*>(mem->get_property())->get_name()));
                        } else {
                            if (!compile_expression(mem->get_property())) return false;
                            emit(Op::DeleteKeyed);
                            emit_u8(static_cast<uint8_t>(obj_reg));
                        }
                        free_temp(obj_reg);
                        return !failed_;
                    }
                    // Anything that is not a reference is `true` without
                    // being evaluated, same as the tree-walker.
                    if (operand->get_type() == ASTNode::Type::IDENTIFIER) {
                        const std::string& name = static_cast<const Identifier*>(operand)->get_name();
                        emit(Op::DeleteLookup);
                        emit_u16(add_name(name));
                        // A register local answers false without a lookup, but an
                        // open `with` can carry the same name on its object, and
                        // then the delete belongs to that object.
                        const bool reg_local = is_local(name) && !env_names_.count(name) &&
                                               !lexical_out_of_scope(name);
                        emit_u8(reg_local ? (with_depth_ > 0 ? 2 : 1) : 0);
                        return !failed_;
                    }
                    // Not a reference: the answer is true, but the operand is
                    // still evaluated for its side effects.
                    if (!compile_expression(operand, /*discard=*/true)) return false;
                    emit(Op::LdaTrue);
                    return true;
                }
                default:
                    return false;
            }
        }

        case ASTNode::Type::ASSIGNMENT_EXPRESSION: {
            const auto* expr = static_cast<const AssignmentExpression*>(node);
            using AsOp = AssignmentExpression::Operator;

            switch (expr->get_operator()) {
                case AsOp::LOGICAL_AND_ASSIGN:
                case AsOp::LOGICAL_OR_ASSIGN:
                case AsOp::NULLISH_ASSIGN:
                    return compile_logical_assignment(expr);
                default:
                    break;
            }

            Op vm_op;
            bool compound = true;
            switch (expr->get_operator()) {
                case AsOp::ASSIGN:            compound = false; vm_op = Op::Add; break;
                case AsOp::PLUS_ASSIGN:       vm_op = Op::Add; break;
                case AsOp::MINUS_ASSIGN:      vm_op = Op::Sub; break;
                case AsOp::MUL_ASSIGN:        vm_op = Op::Mul; break;
                case AsOp::DIV_ASSIGN:        vm_op = Op::Div; break;
                case AsOp::MOD_ASSIGN:        vm_op = Op::Mod; break;
                case AsOp::BITWISE_AND_ASSIGN: vm_op = Op::BitAnd; break;
                case AsOp::BITWISE_OR_ASSIGN:  vm_op = Op::BitOr; break;
                case AsOp::BITWISE_XOR_ASSIGN: vm_op = Op::BitXor; break;
                case AsOp::LEFT_SHIFT_ASSIGN:  vm_op = Op::Shl; break;
                case AsOp::RIGHT_SHIFT_ASSIGN: vm_op = Op::Sar; break;
                case AsOp::UNSIGNED_RIGHT_SHIFT_ASSIGN: vm_op = Op::Shr; break;
                default:
                    return false;  // destructuring assignment operators, etc.
            }

            if (expr->get_left()->get_type() == ASTNode::Type::IDENTIFIER) {
                const std::string& name = static_cast<const Identifier*>(expr->get_left())->get_name();
                if (!is_local(name) || lexical_out_of_scope(name)) {
                    // Outer/global write via chain lookup.
                    // A parenthesised target is not an IdentifierRef, so no
                    // NamedEvaluation happens (spec 13.15.2 step 1.d).
                    if (!compound && !expr->is_lhs_paren()) {
                        stamp_inferred_class_name(expr->get_right(), name);
                    }
                    if (!compound && (with_depth_ > 0 || outer_with_)) {
                        emit(Op::ResolveWithTarget);
                        emit_u16(add_name(name));
                        int target = alloc_temp();
                        if (failed_) return false;
                        emit(Op::Star);
                        emit_u8(static_cast<uint8_t>(target));
                        if (!compile_expression(expr->get_right())) return false;
                        if (!expr->is_lhs_paren() && is_named_evaluation_rhs(expr->get_right())) {
                            emit(Op::SetFunctionNameIfUnnamed);
                            emit_u16(add_name(name));
                        }
                        emit(Op::StaWithResolved);
                        emit_u8(static_cast<uint8_t>(target));
                        emit_u16(add_name(name));
                        free_temp(target);
                        return !failed_;
                    }
                    if (!compound && eval_in_body_) {
                        // A direct eval in the right side can declare a nearer
                        // binding, so the resolved one is kept rather than the
                        // verdict alone (spec 13.15.2: the reference is made
                        // before the right side runs, and the store is to it).
                        if (resolved_env_slots_ >= 8) return false;
                        const uint8_t slot = resolved_env_slots_++;
                        emit(Op::ResolveBindingEnv);
                        emit_u16(add_name(name));
                        emit_u8(slot);
                        if (!compile_expression(expr->get_right())) return false;
                        if (!expr->is_lhs_paren() && is_named_evaluation_rhs(expr->get_right())) {
                            emit(Op::SetFunctionNameIfUnnamed);
                            emit_u16(add_name(name));
                        }
                        emit(Op::StaResolvedEnv);
                        emit_u8(slot);
                        emit_u16(add_name(name));
                        resolved_env_slots_--;
                        return !failed_;
                    }
                    if (!compound) {
                        // Spec 13.15.2: ResolveBinding happens before the RHS
                        // evaluates, so an unresolvable reference throws (in
                        // strict mode) even if the RHS itself creates the
                        // binding, e.g. `x = (this.x = 1)`.
                        emit(Op::CheckLookupResolvable);
                        emit_u16(add_name(name));
                        int resolved = alloc_temp();
                        if (failed_) return false;
                        emit(Op::Star);
                        emit_u8(static_cast<uint8_t>(resolved));
                        if (!compile_expression(expr->get_right())) return false;
                        // NamedEvaluation: anonymous RHS takes the target's
                        // name (the opcode no-ops on an already-named value).
                        // Only for a bare IdentifierRef -- `(fn) = function(){}`
                        // covers the target in parentheses, which makes
                        // IsIdentifierRef false and leaves the name empty.
                        if (!expr->is_lhs_paren() && is_named_evaluation_rhs(expr->get_right())) {
                            emit(Op::SetFunctionNameIfUnnamed);
                            emit_u16(add_name(name));
                        }
                        emit(Op::StaLookupChecked);
                        emit_u8(static_cast<uint8_t>(resolved));
                        emit_u16(add_name(name));
                        free_temp(resolved);
                        return !failed_;
                    }
                    if (with_depth_ > 0 || outer_with_) {
                        emit(Op::ResolveWithTarget);
                        emit_u16(add_name(name));
                        int target = alloc_temp();
                        if (failed_) return false;
                        emit(Op::Star);
                        emit_u8(static_cast<uint8_t>(target));
                        emit(Op::LdaWithResolved);
                        emit_u8(static_cast<uint8_t>(target));
                        emit_u16(add_name(name));
                        int old_reg = alloc_temp();
                        if (failed_) return false;
                        emit(Op::Star);
                        emit_u8(static_cast<uint8_t>(old_reg));
                        if (!compile_expression(expr->get_right())) return false;
                        emit(vm_op);
                        emit_u8(static_cast<uint8_t>(old_reg));
                        free_temp(old_reg);
                        emit(Op::StaWithResolved);
                        emit_u8(static_cast<uint8_t>(target));
                        emit_u16(add_name(name));
                        free_temp(target);
                        return !failed_;
                    }
                    // Spec order: the old value is read (and an unresolvable
                    // reference throws) BEFORE the rhs runs.
                    uint8_t eval_slot = 0;
                    const bool park = eval_in_body_;
                    if (park) {
                        // One resolution serves the read and the write, so a
                        // direct eval in the rhs cannot move the target between
                        // them.
                        if (resolved_env_slots_ >= 8) return false;
                        eval_slot = resolved_env_slots_++;
                        emit(Op::ResolveBindingEnv);
                        emit_u16(add_name(name));
                        emit_u8(eval_slot);
                        emit(Op::LdaResolvedEnv);
                        emit_u8(eval_slot);
                        emit_u16(add_name(name));
                    } else {
                        emit(Op::LdaLookup);
                        emit_u16(add_name(name));
                    }
                    int temp = alloc_temp();
                    if (failed_) return false;
                    emit(Op::Star);
                    emit_u8(static_cast<uint8_t>(temp));
                    if (!compile_expression(expr->get_right())) return false;
                    emit(vm_op);
                    emit_u8(static_cast<uint8_t>(temp));
                    free_temp(temp);
                    if (park) {
                        emit(Op::StaResolvedEnv);
                        emit_u8(eval_slot);
                        emit_u16(add_name(name));
                        resolved_env_slots_--;
                    } else {
                        emit(Op::StaLookup);
                        emit_u16(add_name(name));
                    }
                    return !failed_;
                }

                if (!compound && with_depth_ > 0) {
                    // A with object can hold this name too, and the reference
                    // is bound before the right side runs.
                    if (!expr->is_lhs_paren()) stamp_inferred_class_name(expr->get_right(), name);
                    emit(Op::ResolveWithTarget);
                    emit_u16(add_name(name));
                    int target = alloc_temp();
                    if (failed_) return false;
                    emit(Op::Star);
                    emit_u8(static_cast<uint8_t>(target));
                    if (!compile_expression(expr->get_right())) return false;
                    if (!expr->is_lhs_paren() && is_named_evaluation_rhs(expr->get_right())) {
                        emit(Op::SetFunctionNameIfUnnamed);
                        emit_u16(add_name(name));
                    }
                    emit(Op::StaWithResolved);
                    emit_u8(static_cast<uint8_t>(target));
                    emit_u16(add_name(name));
                    free_temp(target);
                    return !failed_;
                }

                if (!compound) {
                    // Same NamedEvaluation the outer/global branch above does:
                    // a local target names its anonymous RHS too, and a class
                    // has to be named before its static initializers run.
                    if (!expr->is_lhs_paren()) stamp_inferred_class_name(expr->get_right(), name);
                    if (!compile_expression(expr->get_right())) return false;
                    if (!expr->is_lhs_paren() && is_named_evaluation_rhs(expr->get_right())) {
                        emit(Op::SetFunctionNameIfUnnamed);
                        emit_u16(add_name(name));
                    }
                    emit_write_local(name, /*is_declaration=*/false);
                    return !failed_;
                }
                // The target's own register can stand in for the copy when the
                // right side cannot disturb it. The copy exists only because
                // evaluating the right side lands in the accumulator, which is
                // where the old left value would otherwise be sitting.
                if (with_depth_ > 0) {
                    // One resolution serves both the read and the write, and it
                    // happens before the right side can move the target.
                    emit(Op::ResolveWithTarget);
                    emit_u16(add_name(name));
                    int wtarget = alloc_temp();
                    if (failed_) return false;
                    emit(Op::Star);
                    emit_u8(static_cast<uint8_t>(wtarget));
                    emit(Op::LdaWithResolved);
                    emit_u8(static_cast<uint8_t>(wtarget));
                    emit_u16(add_name(name));
                    int old_reg = alloc_temp();
                    if (failed_) return false;
                    emit(Op::Star);
                    emit_u8(static_cast<uint8_t>(old_reg));
                    if (!compile_expression(expr->get_right())) return false;
                    emit(vm_op);
                    emit_u8(static_cast<uint8_t>(old_reg));
                    free_temp(old_reg);
                    emit(Op::StaWithResolved);
                    emit_u8(static_cast<uint8_t>(wtarget));
                    emit_u16(add_name(name));
                    free_temp(wtarget);
                    return !failed_;
                }
                int target_reg = plain_local_register(name);
                if (target_reg >= 0 && leaves_locals_untouched(expr->get_right())) {
                    if (!compile_expression(expr->get_right())) return false;
                    emit(vm_op);
                    emit_u8(static_cast<uint8_t>(target_reg));
                    emit_write_local(name, /*is_declaration=*/false);
                    return !failed_;
                }
                // Spec order: the old lhs value is read BEFORE the rhs runs, so
                // `x += (x = 5)` sees the original x on the left. Evaluating the
                // right side first is only sound above, where it can neither
                // observe the left nor throw ahead of the left's own read.
                int temp = alloc_temp();
                if (failed_) return false;
                emit_read_local(name);
                emit(Op::Star);
                emit_u8(static_cast<uint8_t>(temp));
                if (!compile_expression(expr->get_right())) return false;
                emit(vm_op);
                emit_u8(static_cast<uint8_t>(temp));
                free_temp(temp);
                emit_write_local(name, /*is_declaration=*/false);
                return !failed_;
            }

            // Bare destructuring (`[a,b]=[b,a];`): array/object-literal LHS,
            // plain `=` only. A DestructuringAssignment node here is NOT the
            // same thing: `let [x] = [1]` parses as a declarator whose init is
            // an assignment carrying one, so treating it as an assignment would
            // write the names into the enclosing scope instead of declaring
            // them, and nothing on the node itself tells the two apart.
            if (!compound && (expr->get_left()->get_type() == ASTNode::Type::ARRAY_LITERAL ||
                              expr->get_left()->get_type() == ASTNode::Type::OBJECT_LITERAL)) {
                if (false ||
                    !pattern_is_emittable(expr->get_left(), /*is_lexical=*/false,
                                          /*is_assignment=*/true)) {
                    return false;
                }
                return emit_pattern_assign(expr->get_left(), expr->get_right());
            }

            if (expr->get_left()->get_type() != ASTNode::Type::MEMBER_EXPRESSION) return false;
            const auto* mem = static_cast<const MemberExpression*>(expr->get_left());
            const bool priv = member_is_private(mem);
            if (member_is_super(mem)) {
                if (!super_member_emittable(mem)) return false;
                // PutValue resolves the base before the RHS runs, so it is parked
                // in a register rather than resolved by the store opcode.
                int base_reg = alloc_temp();
                if (failed_) return false;
                emit(Op::ResolveSuperBase);
                emit_u8(static_cast<uint8_t>(base_reg));

                int super_key_reg = -1;
                uint16_t super_name = 0;
                if (mem->is_computed()) {
                    if (!compile_expression(mem->get_property())) return false;
                    super_key_reg = alloc_temp();
                    if (failed_) return false;
                    if (compound) {
                        // The read needs a property key now; a plain assign leaves
                        // the conversion to the store, after the RHS (spec PutValue).
                        emit(Op::ToPropertyKey);
                    }
                    emit(Op::Star);
                    emit_u8(static_cast<uint8_t>(super_key_reg));
                } else {
                    super_name = add_name(static_cast<const Identifier*>(mem->get_property())->get_name());
                }

                if (compound) {
                    if (super_key_reg >= 0) {
                        emit(Op::Ldar);
                        emit_u8(static_cast<uint8_t>(super_key_reg));
                        emit(Op::GetSuperKeyed);
                        emit_u8(static_cast<uint8_t>(base_reg));
                    } else {
                        emit(Op::GetSuper);
                        emit_u16(super_name);
                    }
                    int old_val = alloc_temp();
                    if (failed_) return false;
                    emit(Op::Star);
                    emit_u8(static_cast<uint8_t>(old_val));
                    if (!compile_expression(expr->get_right())) return false;
                    emit(vm_op);
                    emit_u8(static_cast<uint8_t>(old_val));
                    free_temp(old_val);
                } else if (!compile_expression(expr->get_right())) {
                    return false;
                }

                if (super_key_reg >= 0) {
                    emit(Op::SetSuperKeyed);
                    emit_u8(static_cast<uint8_t>(base_reg));
                    emit_u8(static_cast<uint8_t>(super_key_reg));
                    free_temp(super_key_reg);
                } else {
                    emit(Op::SetSuper);
                    emit_u8(static_cast<uint8_t>(base_reg));
                    emit_u16(super_name);
                }
                free_temp(base_reg);
                return !failed_;
            }
            if (!priv && !member_is_supported(mem)) return false;

            if (!compile_expression(mem->get_object())) return false;
            int obj_reg = alloc_temp();
            if (failed_) return false;
            emit(Op::Star);
            emit_u8(static_cast<uint8_t>(obj_reg));

            uint16_t name_idx = 0;
            int key_reg = -1;
            if (!mem->is_computed()) {
                name_idx = add_name(static_cast<const Identifier*>(mem->get_property())->get_name());
            } else {
                if (!compile_expression(mem->get_property())) return false;
                key_reg = alloc_temp();
                if (failed_) return false;
                emit(Op::Star);
                emit_u8(static_cast<uint8_t>(key_reg));
            }

            if (compound) {
                if (!mem->is_computed()) {
                    emit(priv ? Op::GetPrivate : Op::GetNamed);
                    emit_u8(static_cast<uint8_t>(obj_reg));
                    emit_u16(name_idx);
                    emit_u16(priv ? alloc_private_feedback() : alloc_feedback_slot());
                } else {
                    // Spec: CheckObjectCoercible(base) before ToPropertyKey(key)
                    // for a compound assignment's GetValue step.
                    emit(Op::Ldar);
                    emit_u8(static_cast<uint8_t>(obj_reg));
                    emit(Op::CheckObjectCoercible);
                    emit(Op::Ldar);
                    emit_u8(static_cast<uint8_t>(key_reg));
                    emit(Op::ToPropertyKey);  // once; GetKeyed/SetKeyed below reuse the string
                    emit(Op::Star);
                    emit_u8(static_cast<uint8_t>(key_reg));
                    emit(Op::Ldar);
                    emit_u8(static_cast<uint8_t>(key_reg));
                    emit(Op::GetKeyed);
                    emit_u8(static_cast<uint8_t>(obj_reg));
                    emit_u16(alloc_keyed_feedback());
                }
                int old_val = alloc_temp();
                if (failed_) return false;
                emit(Op::Star);
                emit_u8(static_cast<uint8_t>(old_val));
                if (!compile_expression(expr->get_right())) return false;
                emit(vm_op);
                emit_u8(static_cast<uint8_t>(old_val));
                free_temp(old_val);
            } else {
                // Plain assign: per spec, the key is NOT converted to a
                // property key until PutValue -- after the RHS evaluates.
                // (base[k] = v where base is null must run k's toString
                // AFTER v, not before -- see AssignmentExpression's own
                // deferred-ToPropertyKey comment.)
                if (!compile_expression(expr->get_right())) return false;
            }

            if (!mem->is_computed()) {
                emit(priv ? Op::SetPrivate : Op::SetNamed);
                emit_u8(static_cast<uint8_t>(obj_reg));
                emit_u16(name_idx);
                emit_u16(priv ? alloc_private_feedback() : alloc_feedback_slot());
            } else {
                emit(Op::SetKeyed);
                emit_u8(static_cast<uint8_t>(obj_reg));
                emit_u8(static_cast<uint8_t>(key_reg));
                emit_u16(alloc_keyed_feedback());
                free_temp(key_reg);
            }
            free_temp(obj_reg);
            return !failed_;
        }

        case ASTNode::Type::CALL_EXPRESSION: {
            const auto* call = static_cast<const CallExpression*>(node);
            // Optional forms need the chain wrapper's short-circuit collector;
            // without it (non-expression contexts) bail like before.
            if ((call->is_optional() || chain_contains_optional(call)) &&
                !chain_shortcircuit_jumps_) {
                return false;
            }
            const ASTNode* callee = call->get_callee();
            const auto& call_args = call->get_arguments();
            if (call_args.size() > 200) return false;
            const bool spread_args = has_spread(call_args);
            if (spread_args) {
                // `super(...spread)`: the ceremony itself lives in
                // perform_super_call, so this only has to gather the arguments
                // the same way any other spread call does and hand the array
                // over. The super bindings it reads live on the context chain.
                if (callee->get_type() == ASTNode::Type::IDENTIFIER &&
                    static_cast<const Identifier*>(callee)->get_name() == "super") {
                    if (!env_mode_) return false;
                    int super_arr = emit_spread_array(call_args);
                    if (super_arr < 0) return false;
                    emit(Op::SuperCallSpread);
                    emit_u8(static_cast<uint8_t>(super_arr));
                    free_temp(super_arr);
                    return !failed_;
                }

            }
            if (spread_args &&
                callee->get_type() != ASTNode::Type::MEMBER_EXPRESSION &&
                callee->get_type() != ASTNode::Type::OPTIONAL_CHAINING_EXPRESSION) {
                if (!compile_expression(callee)) return false;
                int func_reg = alloc_temp();
                if (failed_) return false;
                emit(Op::Star);
                emit_u8(static_cast<uint8_t>(func_reg));
                // Spec: the callee is evaluated before the arguments.
                int args_reg;
                if (call_args.size() == 1) {
                    // `f(...xs)`: the whole argument list IS the spread, so
                    // hand CallSpread the original iterable and skip
                    // materializing an argument array entirely.
                    if (!compile_expression(static_cast<const SpreadElement*>(call_args[0].get())->get_argument())) return false;
                    args_reg = alloc_temp();
                    if (failed_) return false;
                    emit(Op::Star);
                    emit_u8(static_cast<uint8_t>(args_reg));
                } else {
                    args_reg = emit_spread_array(call_args);
                }
                if (args_reg < 0) { free_temp(func_reg); return false; }
                int this_reg = alloc_temp();
                if (failed_) return false;
                emit(Op::LdaUndefined);
                emit(Op::Star);
                emit_u8(static_cast<uint8_t>(this_reg));
                // A spread does not stop `eval(...)` being a direct eval; the
                // flag has to be up for the call and down again after.
                const bool spread_eval = callee->get_type() == ASTNode::Type::IDENTIFIER &&
                                         static_cast<const Identifier*>(callee)->get_name() == "eval" &&
                                         !call->is_optional();
                if (spread_eval) { emit(Op::SetDirectEval); emit_u8(1); }
                emit(Op::CallSpread);
                emit_u8(static_cast<uint8_t>(func_reg));
                emit_u8(static_cast<uint8_t>(this_reg));
                emit_u8(static_cast<uint8_t>(args_reg));
                emit_u16(add_name(callee->get_type() == ASTNode::Type::IDENTIFIER
                                      ? static_cast<const Identifier*>(callee)->get_name()
                                      : std::string("expression")));
                if (spread_eval) { emit(Op::SetDirectEval); emit_u8(0); }
                free_temp(this_reg);
                free_temp(args_reg);
                free_temp(func_reg);
                return !failed_;
            }

            // obj.method(...): the receiver must be `obj`, not undefined --
            // needs CallResolved, not a plain Call of the loaded function value.
            if (callee->get_type() == ASTNode::Type::MEMBER_EXPRESSION ||
                callee->get_type() == ASTNode::Type::OPTIONAL_CHAINING_EXPRESSION) {
                const ASTNode* mem_obj;
                const ASTNode* mem_prop;
                bool mem_computed;
                bool mem_optional = callee->get_type() == ASTNode::Type::OPTIONAL_CHAINING_EXPRESSION;
                bool mem_private = false;
                const MemberExpression* super_mem = nullptr;
                if (!mem_optional) {
                    const auto* mem = static_cast<const MemberExpression*>(callee);
                    mem_private = member_is_private(mem);
                    if (member_is_super(mem)) {
                        if (!super_member_emittable(mem)) return false;
                        super_mem = mem;
                    } else if (!mem_private && !member_is_supported(mem)) {
                        // Forms the register compiler doesn't implement: delegate the
                        // whole call to the tree-walker instead of bailing the function.
                        return false;
                    }
                    mem_obj = mem->get_object();
                    mem_prop = mem->get_property();
                    mem_computed = mem->is_computed();
                } else {
                    const auto* opt = static_cast<const OptionalChainingExpression*>(callee);
                    if (opt->get_object()->get_type() == ASTNode::Type::IDENTIFIER &&
                        static_cast<const Identifier*>(opt->get_object())->get_name() == "super") {
                        return false;
                    }
                    if (!opt->is_computed()) {
                        if (opt->get_property()->get_type() != ASTNode::Type::IDENTIFIER) return false;
                        const std::string& pname =
                            static_cast<const Identifier*>(opt->get_property())->get_name();
                        if (!pname.empty() && pname[0] == '#') return false;
                    }
                    mem_obj = opt->get_object();
                    mem_prop = opt->get_property();
                    mem_computed = opt->is_computed();
                }

                // super.m(...) calls the method found on the super base, but with
                // the current `this` as receiver, so the two differ here where
                // every other form uses one object for both.
                if (super_mem) {
                    emit(Op::LdaThis);
                } else if (!compile_expression(mem_obj)) {
                    return false;
                }
                int obj_reg = alloc_temp();
                if (failed_) return false;
                emit(Op::Star);
                emit_u8(static_cast<uint8_t>(obj_reg));
                if (mem_optional) {
                    // a?.b(...): receiver still in the accumulator after Star.
                    chain_shortcircuit_jumps_->push_back(emit_jump(Op::JumpIfNullish));
                }

                // Resolve the method before compiling arguments (spec order):
                // this throws on a null/undefined receiver, args must not run first.
                std::string method_name;
                if (super_mem) {
                    method_name = mem_computed
                        ? "<computed>"
                        : static_cast<const Identifier*>(mem_prop)->get_name();
                    if (!emit_super_load(super_mem)) return false;
                } else if (!mem_computed) {
                    method_name = static_cast<const Identifier*>(mem_prop)->get_name();
                    emit(mem_private ? Op::GetPrivate : Op::GetNamed);
                    emit_u8(static_cast<uint8_t>(obj_reg));
                    emit_u16(add_name(method_name));
                    emit_u16(mem_private ? alloc_private_feedback() : alloc_feedback_slot());
                } else {
                    method_name = "<computed>";  // CallResolved diagnostics only
                    ChainMaskScope key_mask(chain_shortcircuit_jumps_);
                    if (!compile_expression(mem_prop)) return false;
                    emit(Op::GetKeyed);
                    emit_u8(static_cast<uint8_t>(obj_reg));
                    emit_u16(alloc_keyed_feedback());
                }
                int func_reg = alloc_temp();
                if (failed_) return false;
                emit(Op::Star);
                emit_u8(static_cast<uint8_t>(func_reg));
                if (call->is_optional()) {
                    // a.b?.(...): the resolved method is still in the accumulator.
                    chain_shortcircuit_jumps_->push_back(emit_jump(Op::JumpIfNullish));
                }

                if (spread_args) {
                    // Receiver and method are already resolved; only the
                    // argument marshalling differs from the fixed-arity case.
                    int args_reg;
                    {
                        ChainMaskScope mask(chain_shortcircuit_jumps_);
                        if (call_args.size() == 1) {
                            if (!compile_expression(static_cast<const SpreadElement*>(call_args[0].get())->get_argument())) return false;
                            args_reg = alloc_temp();
                            if (failed_) return false;
                            emit(Op::Star);
                            emit_u8(static_cast<uint8_t>(args_reg));
                        } else {
                            args_reg = emit_spread_array(call_args);
                        }
                    }
                    if (args_reg < 0) { free_temp(obj_reg); return false; }
                    emit(Op::CallSpread);
                    emit_u8(static_cast<uint8_t>(func_reg));
                    emit_u8(static_cast<uint8_t>(obj_reg));
                    emit_u8(static_cast<uint8_t>(args_reg));
                    emit_u16(add_name(method_name));
                    free_temp(args_reg);
                    free_temp(obj_reg);
                    return !failed_;
                }

                int args_start = next_register_;
                uint8_t argc = static_cast<uint8_t>(call_args.size());
                if (call->is_tagged_template()) {
                    ChainMaskScope mask(chain_shortcircuit_jumps_);
                    if (!emit_tagged_template_args(call, args_start, argc)) return false;
                } else {
                    ChainMaskScope mask(chain_shortcircuit_jumps_);
                    for (const auto& arg : call_args) {
                        int arg_reg = alloc_temp();
                        if (failed_) return false;
                        if (!compile_expression(arg.get())) return false;
                        emit(Op::Star);
                        emit_u8(static_cast<uint8_t>(arg_reg));
                    }
                }

                emit(Op::CallResolved);
                emit_u8(static_cast<uint8_t>(func_reg));
                emit_u8(static_cast<uint8_t>(obj_reg));
                emit_u8(static_cast<uint8_t>(args_start));
                emit_u8(argc);
                emit_u16(add_name(method_name));
                free_temp(obj_reg);
                return !failed_;
            }

            // An engine-synthesised callee names an operation, not a binding,
            // so none of the identifier cases below can apply to it; the
            // generic tail loads it from its slot and calls it normally.
            const bool engine_callee = callee->get_type() == ASTNode::Type::ENGINE_HELPER;
            // Anything else reaching here is an expression whose value is
            // called with an undefined `this`: every receiver-carrying callee
            // form -- member, optional chain, super property -- returns from
            // the branch above rather than falling through. `f()()`,
            // `(a || b)()` and `(0, o.m)()` are all that shape, and refusing
            // them handed the whole function to the tree-walker. Only the
            // diagnostic name the call site records needs the identifier.
            const bool named_callee = callee->get_type() == ASTNode::Type::IDENTIFIER;
            static const std::string kUnnamedCallee = "";
            const std::string& callee_name = named_callee
                ? static_cast<const Identifier*>(callee)->get_name() : kUnnamedCallee;
            // super(), direct eval and import() are each spelled as a bare
            // identifier, so only a named callee can be one of them.
            if (named_callee && callee_name == "super") {
                // The whole derived-constructor ceremony lives in
                // perform_super_call; this only marshals the arguments. It needs
                // the super bindings on the context chain, like the property forms.
                if (!env_mode_) return false;
                int super_args_start = next_register_;
                {
                    ChainMaskScope mask(chain_shortcircuit_jumps_);
                    for (const auto& arg : call_args) {
                        int arg_reg = alloc_temp();
                        if (failed_) return false;
                        if (!compile_expression(arg.get())) return false;
                        emit(Op::Star);
                        emit_u8(static_cast<uint8_t>(arg_reg));
                    }
                }
                emit(Op::SuperCall);
                emit_u8(static_cast<uint8_t>(super_args_start));
                emit_u8(static_cast<uint8_t>(call_args.size()));
                return !failed_;
            }
            // A direct eval reads and writes the caller's scope by name, which
            // only a real Environment holds. (`import(...)` used to sit here
            // with it, but its callee is an ordinary global function and a
            // reserved word no one can shadow, so the plain call form fits.)
            // `eval?.(x)` is an optional call, and an optional call is never a
            // direct eval -- the chain evaluates the callee as a value first.
            const bool direct_eval =
                named_callee && callee_name == "eval" && !call->is_optional();
            // At the top level there are no frame locals to hide: every name is
            // already an outer binding, so no full environment is needed.
            if (direct_eval && !full_env_ && !script_mode_) return false;

            // Class field initialisers are synthesised as a DefineField helper
            // call -- a slot read and a native call per field, on every
            // construction. The accumulator opcode for exactly that semantic
            // (CreateDataProperty, never a prototype setter) already exists, so
            // emit it directly.
            if (engine_callee &&
                static_cast<const EngineHelper*>(callee)->get_kind() == EngineHelper::Kind::DefineField &&
                call_args.size() == 3 &&
                call_args[0]->get_type() == ASTNode::Type::IDENTIFIER &&
                static_cast<const Identifier*>(call_args[0].get())->get_name() == "this" &&
                call_args[1]->get_type() == ASTNode::Type::STRING_LITERAL) {
                const std::string& field_key =
                    static_cast<const StringLiteral*>(call_args[1].get())->get_value();
                emit(Op::LdaThis);
                int this_reg = alloc_temp();
                if (failed_) return false;
                emit(Op::Star);
                emit_u8(static_cast<uint8_t>(this_reg));
                if (!compile_expression(call_args[2].get())) return false;
                emit(Op::DefineOwn);
                emit_u8(static_cast<uint8_t>(this_reg));
                emit_u16(add_name(field_key));
                emit_u16(alloc_feedback_slot());
                free_temp(this_reg);
                // The call's own value is undefined; the statement discards it.
                emit(Op::LdaUndefined);
                return !failed_;
            }

            if (!compile_expression(callee)) return false;
            int callee_reg = alloc_temp();
            if (failed_) return false;
            emit(Op::Star);
            emit_u8(static_cast<uint8_t>(callee_reg));
            if (call->is_optional()) {
                // f?.(...): callee still in the accumulator after Star.
                chain_shortcircuit_jumps_->push_back(emit_jump(Op::JumpIfNullish));
            }

            // Arguments occupy consecutive temps: each argument expression
            // balances its own temps, so alloc_temp stays contiguous here.
            int args_start = next_register_;
            uint8_t argc = static_cast<uint8_t>(call_args.size());
            if (call->is_tagged_template()) {
                ChainMaskScope mask(chain_shortcircuit_jumps_);
                if (!emit_tagged_template_args(call, args_start, argc)) return false;
            } else {
                ChainMaskScope mask(chain_shortcircuit_jumps_);
                for (const auto& arg : call_args) {
                    int arg_reg = alloc_temp();
                    if (failed_) return false;
                    if (!compile_expression(arg.get())) return false;
                    emit(Op::Star);
                    emit_u8(static_cast<uint8_t>(arg_reg));
                }
            }

            emit(direct_eval ? Op::CallDirectEval : Op::Call);
            emit_u8(static_cast<uint8_t>(callee_reg));
            emit_u8(static_cast<uint8_t>(args_start));
            emit_u8(argc);
            emit_u16(add_name(callee_name));
            free_temp(callee_reg);
            return !failed_;
        }

        case ASTNode::Type::NEW_EXPRESSION: {
            const auto* expr = static_cast<const NewExpression*>(node);
            const auto& new_args = expr->get_arguments();
            if (new_args.size() > 200) return false;
            if (has_spread(new_args)) {
                if (!compile_expression(expr->get_constructor())) return false;
                int ctor_reg = alloc_temp();
                if (failed_) return false;
                emit(Op::Star);
                emit_u8(static_cast<uint8_t>(ctor_reg));
                int args_reg;
                if (new_args.size() == 1) {
                    // `new C(...xs)`: same single-spread shortcut as the call form.
                    if (!compile_expression(static_cast<const SpreadElement*>(new_args[0].get())->get_argument())) return false;
                    args_reg = alloc_temp();
                    if (failed_) return false;
                    emit(Op::Star);
                    emit_u8(static_cast<uint8_t>(args_reg));
                } else {
                    args_reg = emit_spread_array(new_args);
                }
                if (args_reg < 0) { free_temp(ctor_reg); return false; }
                emit(Op::ConstructSpread);
                emit_u8(static_cast<uint8_t>(ctor_reg));
                emit_u8(static_cast<uint8_t>(args_reg));
                emit_u16(add_name(expr->get_constructor()->to_string()));
                free_temp(args_reg);
                free_temp(ctor_reg);
                return !failed_;
            }

            if (!compile_expression(expr->get_constructor())) return false;
            int callee_reg = alloc_temp();
            if (failed_) return false;
            emit(Op::Star);
            emit_u8(static_cast<uint8_t>(callee_reg));

            int args_start = next_register_;
            for (const auto& arg : new_args) {
                int arg_reg = alloc_temp();
                if (failed_) return false;
                if (!compile_expression(arg.get())) return false;
                emit(Op::Star);
                emit_u8(static_cast<uint8_t>(arg_reg));
            }

            emit(Op::Construct);
            emit_u8(static_cast<uint8_t>(callee_reg));
            emit_u8(static_cast<uint8_t>(args_start));
            emit_u8(static_cast<uint8_t>(new_args.size()));
            emit_u16(add_name(expr->get_constructor()->to_string()));
            free_temp(callee_reg);
            return !failed_;
        }

        case ASTNode::Type::CONDITIONAL_EXPRESSION: {
            const auto* expr = static_cast<const ConditionalExpression*>(node);
            if (!compile_expression(expr->get_test())) return false;
            size_t else_jump = emit_jump(Op::JumpIfFalse);
            if (!compile_expression(expr->get_consequent())) return false;
            size_t end_jump = emit_jump(Op::Jump);
            if (!patch_jump(else_jump)) return false;
            if (!compile_expression(expr->get_alternate())) return false;
            return patch_jump(end_jump);
        }

        // yield/await delegate to the tree-walker, which suspends the current
        // FIBER (stackful coroutine) -- the VM's own C++ frame sleeps through
        // the suspension and the sent/resolved value lands in the accumulator.
        case ASTNode::Type::YIELD_EXPRESSION: {
            const auto* n = static_cast<const YieldExpression*>(node);
            // Valid code cannot yield outside a suspendable body, but such a
            // node must still not compile to a suspend.
            if (!suspendable_) return false;
            if (n->get_argument()) {
                if (!compile_expression(n->get_argument())) return false;
            } else {
                emit(Op::LdaUndefined);
            }
            emit(n->is_delegate() ? Op::YieldStar : Op::Yield);
            return !failed_;
        }

        case ASTNode::Type::AWAIT_EXPRESSION: {
            const auto* n = static_cast<const AwaitExpression*>(node);
            // No gate on suspendable_: perform_await asks the running fiber
            // itself, and answers a module top level -- which has none -- by
            // draining microtasks, exactly as the tree-walker does.
            if (n->get_argument()) {
                if (!compile_expression(n->get_argument())) return false;
            } else {
                emit(Op::LdaUndefined);
            }
            emit(Op::Await);
            emit_u8(n->get_argument() ? 1 : 0);
            return !failed_;
        }

        // Delegates to the tree-walker's own evaluate() (Op::CreateClosure) --
        // correct since env_mode guarantees every local this closure could
        // reference lives in ctx.get_lexical_environment(). CLASS_DECLARATION
        // here is the expression form (`const C = class {}`): evaluate()
        // returns the class without binding a name. Generator forms ride the
        // FUNCTION_EXPRESSION node; async fns/arrows have their own type.
        case ASTNode::Type::FUNCTION_EXPRESSION:
        case ASTNode::Type::ARROW_FUNCTION_EXPRESSION:
        case ASTNode::Type::ASYNC_FUNCTION_EXPRESSION: {
            if (!env_mode_) return false;
            if (chunk_->ensure_closures().size() >= 0xFFFF) return false;
            chunk_->ensure_closures().push_back(closure_template_for(node));
            emit(Op::CreateClosure);
            emit_u16(static_cast<uint16_t>(chunk_->ensure_closures().size() - 1));
            return true;
        }

        // A class expression is not a function literal: evaluate() builds the
        // whole class (heritage, methods, fields, private brands), none of
        // which the compiler can emit yet -- so it goes through the generic
        // tree-walk escape, not CreateClosure.
        case ASTNode::Type::CLASS_DECLARATION: {
            if (!env_mode_) return false;
            if (try_compile_plain_class(static_cast<const ClassDeclaration*>(node),
                                        /*bind_name=*/false)) return true;
            if (chunk_->ensure_ast_nodes().size() >= 0xFFFF) return false;
            chunk_->ensure_ast_nodes().push_back(node);
            emit(Op::DefineClass);
            emit_u16(static_cast<uint16_t>(chunk_->ensure_ast_nodes().size() - 1));
            return true;
        }

        case ASTNode::Type::OBJECT_LITERAL: {
            const auto* lit = static_cast<const ObjectLiteral*>(node);
            // Spread, non-computed __proto__, computed-key Getter/Setter,
            // oversized: ObjectLiteral::evaluate handles every form, and method
            // closures capture through the ctx chain delegation provides.
            // Everything else (static/computed Value and Method, static-key
            // Getter/Setter) has native codegen below.
            if (object_literal_is_complex(lit)) return false;

            emit(Op::CreateObject);
            emit_u16(static_cast<uint16_t>(lit->get_properties().size()));
            int obj_reg = alloc_temp();
            if (failed_) return false;
            emit(Op::Star);
            emit_u8(static_cast<uint8_t>(obj_reg));

            // Spec 12.2.6.9 NamedEvaluation step 6: Getters/Setters are always
            // named; other properties only if IsAnonymousFunctionDefinition
            // (checked by AST node shape, not name emptiness) -- mirrors
            // literals.cpp's own is_anon_fn_def exactly.
            auto is_anon_fn_def = [](const ASTNode* n) {
                if (!n) return false;
                auto t = n->get_type();
                return t == ASTNode::Type::FUNCTION_EXPRESSION ||
                       t == ASTNode::Type::ARROW_FUNCTION_EXPRESSION ||
                       t == ASTNode::Type::ASYNC_FUNCTION_EXPRESSION ||
                       t == ASTNode::Type::CLASS_DECLARATION;
            };

            // Op::FinalizeStaticProperty/FinalizeComputedProperty's method
            // branch unconditionally writes the [[HomeObject]] marker
            // property, needed only so a `super.x`/`super()` inside the
            // method body can resolve -- but that write pays a full
            // Object::set_property() [[Set]] (Proxy check + a two-pass walk
            // up Function.prototype/Object.prototype, both already dictionary-
            // mode) on every single method creation, even for the overwhelming
            // majority of shorthand methods that never reference `super` at
            // all. method_value_references_super proves "no super anywhere in
            // here" at compile time (see its own doc comment), and skip the
            // write entirely when proven.
            // kind's low 2 bits are the existing 0/1/2 (Method/Getter/Setter);
            // bit 0x4 is new: "method body proved super-free, the
            // [[HomeObject]] write was skipped" (Interpreter.cpp masks it off
            // before comparing against 0/1/2).
            constexpr uint8_t kSuperFreeFlag = 0x4;

            for (const auto& prop : lit->get_properties()) {
                if (!prop->key) {
                    // Spread: no key of its own, and its properties land in
                    // source order relative to the ones around it, which is
                    // why it is emitted here rather than hoisted.
                    const auto* spread = static_cast<const SpreadElement*>(prop->value.get());
                    if (!compile_expression(spread->get_argument())) return false;
                    emit(Op::ObjectSpreadInto);
                    emit_u8(static_cast<uint8_t>(obj_reg));
                    continue;
                }
                bool is_method = prop->type == ObjectLiteral::PropertyType::Method;
                bool is_getter = prop->type == ObjectLiteral::PropertyType::Getter;
                bool is_setter = prop->type == ObjectLiteral::PropertyType::Setter;

                if (!prop->computed) {
                    // Static key: known at compile time. Matches
                    // ObjectLiteral::evaluate's own NUMBER_LITERAL-key
                    // formatting exactly.
                    std::string key;
                    auto kt = prop->key->get_type();
                    if (kt == ASTNode::Type::IDENTIFIER) {
                        key = static_cast<const Identifier*>(prop->key.get())->get_name();
                    } else if (kt == ASTNode::Type::STRING_LITERAL) {
                        key = static_cast<const StringLiteral*>(prop->key.get())->get_value();
                    } else {
                        key = numeric_key_text(
                            static_cast<const NumberLiteral*>(prop->key.get())->get_value());
                    }

                    // Asked before the value is compiled: compiling it is
                    // what lets go of the body this reads.
                    const bool keeps_super =
                        (is_method || is_getter || is_setter) &&
                        method_value_references_super(prop->value.get());
                    if (!compile_expression(prop->value.get())) return false;

                    // Only this spelling sets [[Prototype]]: the shorthand,
                    // `__proto__(){}` and a computed key are all properties.
                    if (key == "__proto__" && !prop->shorthand &&
                        prop->type == ObjectLiteral::PropertyType::Value) {
                        emit(Op::SetLiteralProto);
                        emit_u8(static_cast<uint8_t>(obj_reg));
                        continue;
                    }

                    if (is_method || is_getter || is_setter) {
                        uint16_t key_name_idx = add_name(key);
                        std::string display_name = is_getter ? ("get " + key)
                                                  : is_setter ? ("set " + key)
                                                  : key;
                        uint16_t display_name_idx = add_name(display_name);
                        emit(Op::FinalizeStaticProperty);
                        emit_u8(static_cast<uint8_t>(obj_reg));
                        emit_u16(key_name_idx);
                        emit_u16(display_name_idx);
                        uint8_t kind_byte = is_method ? 0 : (is_getter ? 1 : 2);
                        if (is_method && !keeps_super) kind_byte |= kSuperFreeFlag;
                        emit_u8(kind_byte);
                        emit_u16(alloc_feedback_slot());
                    } else {
                        // NamedEvaluation step 6: unlike Method/Getter/Setter
                        // (always named), a plain Value property is only named
                        // if its value is anonymous-function-shaped. A literal
                        // mixing this with newly-native forms (computed keys,
                        // methods) used to get this via whole-literal tree-
                        // walker delegation; must replicate it natively now.
                        if (is_anon_fn_def(prop->value.get())) {
                            emit(Op::SetFunctionNameIfUnnamed);
                            emit_u16(add_name(key));
                        }
                        // CreateDataProperty (a poisoned Object.prototype
                        // accessor must not fire) -- DefineOwn.
                        emit(Op::DefineOwn);
                        emit_u8(static_cast<uint8_t>(obj_reg));
                        emit_u16(add_name(key));
                        emit_u16(alloc_feedback_slot());
                    }
                } else {
                    // Computed key (Value or Method only -- computed-key
                    // Getter/Setter is excluded by object_literal_is_complex).
                    // ToPropertyKey must run before the value/method body
                    // evaluates (spec order) -- raw_key_reg keeps the
                    // pre-conversion Value alive for Symbol-aware naming,
                    // key_reg holds the converted form for install.
                    int raw_key_reg = alloc_temp();
                    if (failed_) return false;
                    if (!compile_expression(prop->key.get())) return false;
                    emit(Op::Star);
                    emit_u8(static_cast<uint8_t>(raw_key_reg));

                    int key_reg = alloc_temp();
                    if (failed_) return false;
                    emit(Op::Ldar);
                    emit_u8(static_cast<uint8_t>(raw_key_reg));
                    emit(Op::ToPropertyKeyStrict);
                    emit(Op::Star);
                    emit_u8(static_cast<uint8_t>(key_reg));

                    const bool computed_keeps_super =
                        (is_method || is_getter || is_setter) &&
                        method_value_references_super(prop->value.get());
                    if (!compile_expression(prop->value.get())) return false;

                    // An accessor names itself "get k"/"set k" and merges
                    // with the other half of its pair, so it takes its own
                    // instruction rather than a fourth kind of this one.
                    const bool is_accessor = is_getter || is_setter;
                    uint8_t kind;
                    if (is_accessor) {
                        kind = is_getter ? 1 : 2;
                    } else {
                        kind = is_method ? 2 : (is_anon_fn_def(prop->value.get()) ? 1 : 0);
                    }
                    if ((is_accessor || is_method) && !computed_keeps_super) {
                        kind |= kSuperFreeFlag;
                    }
                    emit(is_accessor ? Op::FinalizeComputedAccessor : Op::FinalizeComputedProperty);
                    emit_u8(static_cast<uint8_t>(obj_reg));
                    emit_u8(static_cast<uint8_t>(key_reg));
                    emit_u8(static_cast<uint8_t>(raw_key_reg));
                    emit_u8(kind);
                    free_temp(key_reg);
                    free_temp(raw_key_reg);
                }
            }
            emit(Op::Ldar);
            emit_u8(static_cast<uint8_t>(obj_reg));
            free_temp(obj_reg);
            return !failed_;
        }

        case ASTNode::Type::ARRAY_LITERAL: {
            const auto* lit = static_cast<const ArrayLiteral*>(node);
            for (const auto& el : lit->get_elements()) {
                if (!el) return false;
            }
            if (has_spread(lit->get_elements())) {
                int arr_reg = emit_spread_array(lit->get_elements());
                if (arr_reg < 0) return false;
                emit(Op::Ldar);
                emit_u8(static_cast<uint8_t>(arr_reg));
                free_temp(arr_reg);
                return !failed_;
            }

            // CreateArray's count is a u16. A longer literal starts empty and
            // takes its length from a store after its elements instead; an
            // element's temp is freed within its own iteration, so length is
            // the only thing that scales with the literal.
            const size_t elem_count = lit->get_elements().size();
            const bool long_literal = elem_count > 0xFFFF;
            emit(Op::CreateArray);
            emit_u16(static_cast<uint16_t>(long_literal ? 0 : elem_count));
            int obj_reg = alloc_temp();
            if (failed_) return false;
            emit(Op::Star);
            emit_u8(static_cast<uint8_t>(obj_reg));

            const auto& elements = lit->get_elements();
            for (size_t i = 0; i < elements.size(); i++) {
                // Holes ride the UNDEFINED_LITERAL node and are skipped --
                // no own element, same as ArrayLiteral::evaluate. CreateArray
                // already fixed the length.
                if (elements[i]->get_type() == ASTNode::Type::UNDEFINED_LITERAL) continue;
                int key_reg = alloc_temp();
                if (failed_) return false;
                if (i <= static_cast<size_t>(INT8_MAX)) {
                    emit(Op::LdaSmi);
                    emit_u8(static_cast<uint8_t>(static_cast<int8_t>(i)));
                } else {
                    emit_load_const(Value(static_cast<double>(i)));
                }
                emit(Op::Star);
                emit_u8(static_cast<uint8_t>(key_reg));
                if (!compile_expression(elements[i].get())) return false;
                // CreateDataProperty: a poisoned Array.prototype index must
                // not block or intercept the literal's own element.
                emit(Op::DefineElement);
                emit_u8(static_cast<uint8_t>(obj_reg));
                emit_u8(static_cast<uint8_t>(key_reg));
                free_temp(key_reg);
            }
            if (long_literal) {
                emit_load_const(Value(static_cast<double>(elem_count)));
                emit(Op::SetNamed);
                emit_u8(static_cast<uint8_t>(obj_reg));
                emit_u16(add_name("length"));
                emit_u16(alloc_feedback_slot());
            }
            emit(Op::Ldar);
            emit_u8(static_cast<uint8_t>(obj_reg));
            free_temp(obj_reg);
            return !failed_;
        }

        case ASTNode::Type::DESTRUCTURING_ASSIGNMENT: {
            const auto* n = static_cast<const DestructuringAssignment*>(node);
            const ASTNode* lit = n->get_pattern_literal();
            // An arbitrary AssignmentTarget (a member expression) still goes to
            // the tree-walker whole; plain names the pattern emitter can write.
            if (!lit || !n->get_source() ||
                !pattern_is_emittable(lit, /*is_lexical=*/false, /*is_assignment=*/true)) {
                return false;
            }
            return emit_pattern_assign(lit, n->get_source());
        }

        default:
            return false;
    }
}

}
