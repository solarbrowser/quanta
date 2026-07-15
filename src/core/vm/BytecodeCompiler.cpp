/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/vm/BytecodeCompiler.h"
#include <algorithm>
#include "quanta/parser/AST.h"
#include <climits>
#include <cmath>
#include <sstream>

namespace Quanta {

namespace {

constexpr int kMaxRegisters = 255;

struct DeclInfo {
    std::string name;
    bool is_lexical;  // let/const: needs a TDZ-seeded register
    bool is_const;
    bool is_catch_param = false;  // gets its own per-catch env; never a function-level local
};

// Parser-encoded placeholder, not a real binding name (nested patterns
// collapse into "__nested_vars:a,b" / "b:c,d" style strings).
bool is_encoded_pattern_name(const std::string& n) {
    return n.rfind("__nested", 0) == 0 || n.rfind("__computed", 0) == 0 ||
           n.find_first_of(":,>\x01") != std::string::npos;
}

// Appends every leaf binding name of a "flat" pattern (plain identifiers +
// `...rest`). Encoded/nested patterns return false: whole function tree-walks.
bool collect_flat_pattern_names(const ASTNode* pattern, bool is_lexical, bool is_const,
                                 std::vector<DeclInfo>& out) {
    if (!pattern) return true;
    if (pattern->get_type() == ASTNode::Type::IDENTIFIER) {
        const std::string& n = static_cast<const Identifier*>(pattern)->get_name();
        if (n.empty()) return true;  // elision
        if (n.size() >= 3 && n.substr(0, 3) == "...") {
            std::string rest = n.substr(3);
            if (rest == "__nested_rest__") return true;  // real pattern is nested_rest_pattern_, below
            if (rest.empty() || is_encoded_pattern_name(rest)) return false;
            out.push_back({rest, is_lexical, is_const});
            return true;
        }
        if (is_encoded_pattern_name(n)) return false;
        out.push_back({n, is_lexical, is_const});
        return true;
    }
    if (pattern->get_type() != ASTNode::Type::DESTRUCTURING_ASSIGNMENT) return false;
    const auto* n = static_cast<const DestructuringAssignment*>(pattern);
    for (const auto& t : n->get_targets()) {
        if (!collect_flat_pattern_names(t.get(), is_lexical, is_const, out)) return false;
    }
    for (const auto& pm : n->get_property_mappings()) {
        if (pm.variable_name.empty() || is_encoded_pattern_name(pm.variable_name)) return false;
        out.push_back({pm.variable_name, is_lexical, is_const});
    }
    if (n->get_nested_rest_pattern()) {
        if (!collect_flat_pattern_names(n->get_nested_rest_pattern(), is_lexical, is_const, out)) return false;
    }
    return true;
}

// Collects every declared name up front (var hoisting), including repeats --
// see contains_nested_lexical_decl for why duplicates are fine here.
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
        case ASTNode::Type::VARIABLE_DECLARATION: {
            const auto* decl = static_cast<const VariableDeclaration*>(node);
            bool is_lexical = decl->get_kind() != VariableDeclarator::Kind::VAR;
            bool is_const = decl->get_kind() == VariableDeclarator::Kind::CONST;
            for (const auto& d : decl->get_declarations()) {
                if (!d->get_id()) return false;
                const std::string& name = d->get_id()->get_name();
                if (name.empty()) continue;  // destructuring: no named slot here
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
            return prescan_declarations(n->get_consequent(), out) &&
                   prescan_declarations(n->get_alternate(), out);
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
                if (clause->get_destructuring_pattern()) return false;
                const std::string& pname = clause->get_parameter_name();
                if (!pname.empty()) {
                    out.push_back({pname, false, false, true});  // its own per-catch env
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
        case ASTNode::Type::LABELED_STATEMENT:
            return prescan_declarations(static_cast<const LabeledStatement*>(node)->get_statement(), out);
        default:
            return true;
    }
}

// True if `name` is ever an assignment or ++/-- target anywhere in `node`.
// Runtime const-immutability isn't implemented, so this is the compile-time check.
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
            return assigns_to_identifier(n->get_right(), name) ||
                   assigns_to_identifier(n->get_body(), name);
        }
        case ASTNode::Type::FOR_IN_STATEMENT: {
            const auto* n = static_cast<const ForInStatement*>(node);
            return assigns_to_identifier(n->get_right(), name) ||
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
            if (n->get_left()->get_type() == ASTNode::Type::IDENTIFIER &&
                static_cast<const Identifier*>(n->get_left())->get_name() == name) {
                return true;
            }
            return assigns_to_identifier(n->get_left(), name) ||
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
        default:
            return false;
    }
}

// True if `node` contains a nested function/arrow. Forces env_mode (locals in
// a real Environment instead of registers, which die with the call frame).
bool contains_closure(const ASTNode* node) {
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
                if (contains_closure(stmt.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::IF_STATEMENT: {
            const auto* n = static_cast<const IfStatement*>(node);
            return contains_closure(n->get_test()) || contains_closure(n->get_consequent()) ||
                   contains_closure(n->get_alternate());
        }
        case ASTNode::Type::WHILE_STATEMENT: {
            const auto* n = static_cast<const WhileStatement*>(node);
            return contains_closure(n->get_test()) || contains_closure(n->get_body());
        }
        case ASTNode::Type::DO_WHILE_STATEMENT: {
            const auto* n = static_cast<const DoWhileStatement*>(node);
            return contains_closure(n->get_body()) || contains_closure(n->get_test());
        }
        case ASTNode::Type::FOR_STATEMENT: {
            const auto* n = static_cast<const ForStatement*>(node);
            return contains_closure(n->get_init()) || contains_closure(n->get_test()) ||
                   contains_closure(n->get_update()) || contains_closure(n->get_body());
        }
        case ASTNode::Type::FOR_OF_STATEMENT: {
            const auto* n = static_cast<const ForOfStatement*>(node);
            return contains_closure(n->get_right()) || contains_closure(n->get_body());
        }
        case ASTNode::Type::FOR_IN_STATEMENT: {
            const auto* n = static_cast<const ForInStatement*>(node);
            return contains_closure(n->get_right()) || contains_closure(n->get_body());
        }
        case ASTNode::Type::TRY_STATEMENT: {
            const auto* n = static_cast<const TryStatement*>(node);
            if (contains_closure(n->get_try_block())) return true;
            if (const ASTNode* cc = n->get_catch_clause()) {
                if (contains_closure(static_cast<const CatchClause*>(cc)->get_body())) return true;
            }
            return contains_closure(n->get_finally_block());
        }
        case ASTNode::Type::SWITCH_STATEMENT: {
            const auto* n = static_cast<const SwitchStatement*>(node);
            if (contains_closure(n->get_discriminant())) return true;
            for (const auto& c : n->get_cases()) {
                const auto* cc = static_cast<const CaseClause*>(c.get());
                if (cc->get_test() && contains_closure(cc->get_test())) return true;
                for (const auto& s : cc->get_consequent()) {
                    if (contains_closure(s.get())) return true;
                }
            }
            return false;
        }
        case ASTNode::Type::LABELED_STATEMENT:
            return contains_closure(static_cast<const LabeledStatement*>(node)->get_statement());
        case ASTNode::Type::EXPRESSION_STATEMENT:
            return contains_closure(static_cast<const ExpressionStatement*>(node)->get_expression());
        case ASTNode::Type::RETURN_STATEMENT: {
            const auto* n = static_cast<const ReturnStatement*>(node);
            return n->get_argument() && contains_closure(n->get_argument());
        }
        case ASTNode::Type::VARIABLE_DECLARATION: {
            const auto* n = static_cast<const VariableDeclaration*>(node);
            for (const auto& d : n->get_declarations()) {
                if (d->get_init() && contains_closure(d->get_init())) return true;
            }
            return false;
        }
        case ASTNode::Type::ASSIGNMENT_EXPRESSION: {
            const auto* n = static_cast<const AssignmentExpression*>(node);
            return contains_closure(n->get_left()) || contains_closure(n->get_right());
        }
        case ASTNode::Type::UNARY_EXPRESSION:
            return contains_closure(static_cast<const UnaryExpression*>(node)->get_operand());
        case ASTNode::Type::BINARY_EXPRESSION: {
            const auto* n = static_cast<const BinaryExpression*>(node);
            return contains_closure(n->get_left()) || contains_closure(n->get_right());
        }
        case ASTNode::Type::CONDITIONAL_EXPRESSION: {
            const auto* n = static_cast<const ConditionalExpression*>(node);
            return contains_closure(n->get_test()) || contains_closure(n->get_consequent()) ||
                   contains_closure(n->get_alternate());
        }
        case ASTNode::Type::CALL_EXPRESSION: {
            const auto* n = static_cast<const CallExpression*>(node);
            if (contains_closure(n->get_callee())) return true;
            for (const auto& arg : n->get_arguments()) {
                if (contains_closure(arg.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::MEMBER_EXPRESSION: {
            const auto* n = static_cast<const MemberExpression*>(node);
            return contains_closure(n->get_object()) ||
                   (n->is_computed() && contains_closure(n->get_property()));
        }
        case ASTNode::Type::OBJECT_LITERAL: {
            const auto* n = static_cast<const ObjectLiteral*>(node);
            for (const auto& prop : n->get_properties()) {
                if (prop->value && contains_closure(prop->value.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::ARRAY_LITERAL: {
            const auto* n = static_cast<const ArrayLiteral*>(node);
            for (const auto& el : n->get_elements()) {
                if (el && contains_closure(el.get())) return true;
            }
            return false;
        }
        default:
            return false;
    }
}

bool has_spread(const std::vector<std::unique_ptr<ASTNode>>& nodes) {
    for (const auto& n : nodes) {
        if (n && n->get_type() == ASTNode::Type::SPREAD_ELEMENT) return true;
    }
    return false;
}

// Anything the native CreateObject/DefineOwn path can't emit (it only does
// plain data properties with a static string/number/identifier key):
// methods/accessors, computed keys, spread, __proto__, oversized literals.
bool object_literal_is_complex(const ObjectLiteral* lit) {
    for (const auto& prop : lit->get_properties()) {
        if (!prop->key) return true;  // spread: null key
        if (prop->type != ObjectLiteral::PropertyType::Value || prop->computed) return true;
        auto kt = prop->key->get_type();
        if (kt == ASTNode::Type::IDENTIFIER) {
            if (static_cast<const Identifier*>(prop->key.get())->get_name() == "__proto__") return true;
        } else if (kt == ASTNode::Type::STRING_LITERAL) {
            if (static_cast<const StringLiteral*>(prop->key.get())->get_value() == "__proto__") return true;
        } else if (kt != ASTNode::Type::NUMBER_LITERAL) {
            return true;
        }
    }
    return lit->get_properties().size() > 200;
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
                    for (const auto& dv : da->get_default_values())
                        if (contains_delegated_expr(dv.expr.get())) return true;
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
            if (has_spread(n->get_arguments())) return true;
            if (contains_delegated_expr(n->get_callee())) return true;
            for (const auto& arg : n->get_arguments()) {
                if (contains_delegated_expr(arg.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::NEW_EXPRESSION: {
            const auto* n = static_cast<const NewExpression*>(node);
            if (has_spread(n->get_arguments())) return true;
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
                if (prop->value && contains_delegated_expr(prop->value.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::ARRAY_LITERAL: {
            const auto* n = static_cast<const ArrayLiteral*>(node);
            if (has_spread(n->get_elements())) return true;
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
bool uses_arguments(const ASTNode* node) {
    if (!node) return false;
    switch (node->get_type()) {
        case ASTNode::Type::IDENTIFIER:
            return static_cast<const Identifier*>(node)->get_name() == "arguments";
        case ASTNode::Type::FUNCTION_EXPRESSION:
        case ASTNode::Type::FUNCTION_DECLARATION:
            return false;
        case ASTNode::Type::ARROW_FUNCTION_EXPRESSION:
            return uses_arguments(static_cast<const ArrowFunctionExpression*>(node)->get_body());
        case ASTNode::Type::ASYNC_FUNCTION_EXPRESSION: {
            // Async arrows share the enclosing arguments; async functions own theirs.
            const auto* n = static_cast<const AsyncFunctionExpression*>(node);
            return n->is_arrow() && uses_arguments(n->get_body());
        }
        case ASTNode::Type::CLASS_DECLARATION: {
            // Superclass expressions and computed keys evaluate in the
            // enclosing scope; method bodies get their own arguments.
            const auto* n = static_cast<const ClassDeclaration*>(node);
            return uses_arguments(n->get_superclass()) || uses_arguments(n->get_body());
        }
        case ASTNode::Type::METHOD_DEFINITION: {
            const auto* n = static_cast<const MethodDefinition*>(node);
            return n->is_computed() && uses_arguments(n->get_key());
        }
        case ASTNode::Type::CLASS_FIELD: {
            const auto* n = static_cast<const ClassField*>(node);
            return n->is_computed() && uses_arguments(n->get_key());
        }
        case ASTNode::Type::BLOCK_STATEMENT: {
            const auto* n = static_cast<const BlockStatement*>(node);
            for (const auto& stmt : n->get_statements()) {
                if (uses_arguments(stmt.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::IF_STATEMENT: {
            const auto* n = static_cast<const IfStatement*>(node);
            return uses_arguments(n->get_test()) || uses_arguments(n->get_consequent()) ||
                   uses_arguments(n->get_alternate());
        }
        case ASTNode::Type::WHILE_STATEMENT: {
            const auto* n = static_cast<const WhileStatement*>(node);
            return uses_arguments(n->get_test()) || uses_arguments(n->get_body());
        }
        case ASTNode::Type::DO_WHILE_STATEMENT: {
            const auto* n = static_cast<const DoWhileStatement*>(node);
            return uses_arguments(n->get_body()) || uses_arguments(n->get_test());
        }
        case ASTNode::Type::FOR_STATEMENT: {
            const auto* n = static_cast<const ForStatement*>(node);
            return uses_arguments(n->get_init()) || uses_arguments(n->get_test()) ||
                   uses_arguments(n->get_update()) || uses_arguments(n->get_body());
        }
        case ASTNode::Type::FOR_OF_STATEMENT: {
            const auto* n = static_cast<const ForOfStatement*>(node);
            return uses_arguments(n->get_right()) || uses_arguments(n->get_body());
        }
        case ASTNode::Type::FOR_IN_STATEMENT: {
            const auto* n = static_cast<const ForInStatement*>(node);
            return uses_arguments(n->get_right()) || uses_arguments(n->get_body());
        }
        case ASTNode::Type::TRY_STATEMENT: {
            const auto* n = static_cast<const TryStatement*>(node);
            if (uses_arguments(n->get_try_block())) return true;
            if (const ASTNode* cc = n->get_catch_clause()) {
                if (uses_arguments(static_cast<const CatchClause*>(cc)->get_body())) return true;
            }
            return uses_arguments(n->get_finally_block());
        }
        case ASTNode::Type::SWITCH_STATEMENT: {
            const auto* n = static_cast<const SwitchStatement*>(node);
            if (uses_arguments(n->get_discriminant())) return true;
            for (const auto& c : n->get_cases()) {
                const auto* cc = static_cast<const CaseClause*>(c.get());
                if (cc->get_test() && uses_arguments(cc->get_test())) return true;
                for (const auto& s : cc->get_consequent()) {
                    if (uses_arguments(s.get())) return true;
                }
            }
            return false;
        }
        case ASTNode::Type::LABELED_STATEMENT:
            return uses_arguments(static_cast<const LabeledStatement*>(node)->get_statement());
        case ASTNode::Type::EXPRESSION_STATEMENT:
            return uses_arguments(static_cast<const ExpressionStatement*>(node)->get_expression());
        case ASTNode::Type::RETURN_STATEMENT: {
            const auto* n = static_cast<const ReturnStatement*>(node);
            return n->get_argument() && uses_arguments(n->get_argument());
        }
        case ASTNode::Type::THROW_STATEMENT:
            return uses_arguments(static_cast<const ThrowStatement*>(node)->get_expression());
        case ASTNode::Type::VARIABLE_DECLARATION: {
            const auto* n = static_cast<const VariableDeclaration*>(node);
            for (const auto& d : n->get_declarations()) {
                if (d->get_init() && uses_arguments(d->get_init())) return true;
            }
            return false;
        }
        case ASTNode::Type::DESTRUCTURING_ASSIGNMENT: {
            const auto* n = static_cast<const DestructuringAssignment*>(node);
            if (n->get_source() && uses_arguments(n->get_source())) return true;
            for (const auto& dv : n->get_default_values()) {
                if (uses_arguments(dv.expr.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::ASSIGNMENT_EXPRESSION: {
            const auto* n = static_cast<const AssignmentExpression*>(node);
            return uses_arguments(n->get_left()) || uses_arguments(n->get_right());
        }
        case ASTNode::Type::UNARY_EXPRESSION:
            return uses_arguments(static_cast<const UnaryExpression*>(node)->get_operand());
        case ASTNode::Type::BINARY_EXPRESSION: {
            const auto* n = static_cast<const BinaryExpression*>(node);
            return uses_arguments(n->get_left()) || uses_arguments(n->get_right());
        }
        case ASTNode::Type::NULLISH_COALESCING_EXPRESSION: {
            const auto* n = static_cast<const NullishCoalescingExpression*>(node);
            return uses_arguments(n->get_left()) || uses_arguments(n->get_right());
        }
        case ASTNode::Type::CONDITIONAL_EXPRESSION: {
            const auto* n = static_cast<const ConditionalExpression*>(node);
            return uses_arguments(n->get_test()) || uses_arguments(n->get_consequent()) ||
                   uses_arguments(n->get_alternate());
        }
        case ASTNode::Type::CALL_EXPRESSION: {
            const auto* n = static_cast<const CallExpression*>(node);
            if (uses_arguments(n->get_callee())) return true;
            for (const auto& arg : n->get_arguments()) {
                if (uses_arguments(arg.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::NEW_EXPRESSION: {
            const auto* n = static_cast<const NewExpression*>(node);
            if (uses_arguments(n->get_constructor())) return true;
            for (const auto& arg : n->get_arguments()) {
                if (uses_arguments(arg.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::MEMBER_EXPRESSION: {
            // `x.arguments` is a property NAME, not a use -- only the object
            // side (and a computed key) counts.
            const auto* n = static_cast<const MemberExpression*>(node);
            return uses_arguments(n->get_object()) ||
                   (n->is_computed() && uses_arguments(n->get_property()));
        }
        case ASTNode::Type::OPTIONAL_CHAINING_EXPRESSION: {
            const auto* n = static_cast<const OptionalChainingExpression*>(node);
            return uses_arguments(n->get_object()) ||
                   (n->is_computed() && uses_arguments(n->get_property()));
        }
        case ASTNode::Type::SPREAD_ELEMENT:
            return uses_arguments(static_cast<const SpreadElement*>(node)->get_argument());
        case ASTNode::Type::TEMPLATE_LITERAL: {
            const auto* n = static_cast<const TemplateLiteral*>(node);
            for (const auto& el : n->get_elements()) {
                if (el.type == TemplateLiteral::Element::Type::EXPRESSION &&
                    uses_arguments(el.expression.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::OBJECT_LITERAL: {
            const auto* n = static_cast<const ObjectLiteral*>(node);
            for (const auto& prop : n->get_properties()) {
                if (prop->value && uses_arguments(prop->value.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::ARRAY_LITERAL: {
            const auto* n = static_cast<const ArrayLiteral*>(node);
            for (const auto& el : n->get_elements()) {
                if (el && uses_arguments(el.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::YIELD_EXPRESSION: {
            const auto* n = static_cast<const YieldExpression*>(node);
            return n->get_argument() && uses_arguments(n->get_argument());
        }
        case ASTNode::Type::AWAIT_EXPRESSION: {
            const auto* n = static_cast<const AwaitExpression*>(node);
            return n->get_argument() && uses_arguments(n->get_argument());
        }
        default:
            return false;
    }
}

}

bool BytecodeCompiler::references_arguments(const ASTNode* node) {
    return uses_arguments(node);
}

namespace {

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
                           bool& saw_eval, bool& saw_class, bool& unknown,
                           bool suspendable = false) {
    if (!node) return;
    auto walk_params = [&](const std::vector<std::unique_ptr<Parameter>>& ps) {
        for (const auto& p : ps) {
            if (p->has_default())
                collect_closure_names(p->get_default_value(), true, out, saw_eval, saw_class, unknown, suspendable);
            if (p->has_destructuring())
                collect_closure_names(p->get_destructuring_pattern(), true, out, saw_eval, saw_class, unknown, suspendable);
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
        case ASTNode::Type::EMPTY_STATEMENT:
        case ASTNode::Type::BREAK_STATEMENT:
        case ASTNode::Type::CONTINUE_STATEMENT:
            return;
        case ASTNode::Type::IDENTIFIER: {
            if (!inside_closure) return;
            const std::string& n = static_cast<const Identifier*>(node)->get_name();
            if (n == "eval") saw_eval = true;
            out.insert(n);
            return;
        }
        case ASTNode::Type::FUNCTION_EXPRESSION: {
            const auto* n = static_cast<const FunctionExpression*>(node);
            walk_params(n->get_params());
            collect_closure_names(n->get_body(), true, out, saw_eval, saw_class, unknown, suspendable);
            return;
        }
        case ASTNode::Type::FUNCTION_DECLARATION: {
            const auto* n = static_cast<const FunctionDeclaration*>(node);
            walk_params(n->get_params());
            collect_closure_names(n->get_body(), true, out, saw_eval, saw_class, unknown, suspendable);
            return;
        }
        case ASTNode::Type::ARROW_FUNCTION_EXPRESSION: {
            const auto* n = static_cast<const ArrowFunctionExpression*>(node);
            walk_params(n->get_params());
            collect_closure_names(n->get_body(), true, out, saw_eval, saw_class, unknown, suspendable);
            return;
        }
        case ASTNode::Type::ASYNC_FUNCTION_EXPRESSION: {
            const auto* n = static_cast<const AsyncFunctionExpression*>(node);
            walk_params(n->get_params());
            collect_closure_names(n->get_body(), true, out, saw_eval, saw_class, unknown, suspendable);
            return;
        }
        case ASTNode::Type::CLASS_DECLARATION:
            saw_class = true;
            return;
        case ASTNode::Type::BLOCK_STATEMENT: {
            const auto* n = static_cast<const BlockStatement*>(node);
            for (const auto& stmt : n->get_statements())
                collect_closure_names(stmt.get(), inside_closure, out, saw_eval, saw_class, unknown, suspendable);
            return;
        }
        case ASTNode::Type::IF_STATEMENT: {
            const auto* n = static_cast<const IfStatement*>(node);
            collect_closure_names(n->get_test(), inside_closure, out, saw_eval, saw_class, unknown, suspendable);
            collect_closure_names(n->get_consequent(), inside_closure, out, saw_eval, saw_class, unknown, suspendable);
            collect_closure_names(n->get_alternate(), inside_closure, out, saw_eval, saw_class, unknown, suspendable);
            return;
        }
        case ASTNode::Type::WHILE_STATEMENT: {
            const auto* n = static_cast<const WhileStatement*>(node);
            collect_closure_names(n->get_test(), inside_closure, out, saw_eval, saw_class, unknown, suspendable);
            collect_closure_names(n->get_body(), inside_closure, out, saw_eval, saw_class, unknown, suspendable);
            return;
        }
        case ASTNode::Type::DO_WHILE_STATEMENT: {
            const auto* n = static_cast<const DoWhileStatement*>(node);
            collect_closure_names(n->get_body(), inside_closure, out, saw_eval, saw_class, unknown, suspendable);
            collect_closure_names(n->get_test(), inside_closure, out, saw_eval, saw_class, unknown, suspendable);
            return;
        }
        case ASTNode::Type::FOR_STATEMENT: {
            const auto* n = static_cast<const ForStatement*>(node);
            collect_closure_names(n->get_init(), inside_closure, out, saw_eval, saw_class, unknown, suspendable);
            collect_closure_names(n->get_test(), inside_closure, out, saw_eval, saw_class, unknown, suspendable);
            collect_closure_names(n->get_update(), inside_closure, out, saw_eval, saw_class, unknown, suspendable);
            collect_closure_names(n->get_body(), inside_closure, out, saw_eval, saw_class, unknown, suspendable);
            return;
        }
        case ASTNode::Type::FOR_OF_STATEMENT: {
            const auto* n = static_cast<const ForOfStatement*>(node);
            collect_closure_names(n->get_left(), inside_closure, out, saw_eval, saw_class, unknown, suspendable);
            collect_closure_names(n->get_right(), inside_closure, out, saw_eval, saw_class, unknown, suspendable);
            collect_closure_names(n->get_body(), inside_closure, out, saw_eval, saw_class, unknown, suspendable);
            return;
        }
        case ASTNode::Type::FOR_IN_STATEMENT: {
            const auto* n = static_cast<const ForInStatement*>(node);
            collect_closure_names(n->get_left(), inside_closure, out, saw_eval, saw_class, unknown, suspendable);
            collect_closure_names(n->get_right(), inside_closure, out, saw_eval, saw_class, unknown, suspendable);
            collect_closure_names(n->get_body(), inside_closure, out, saw_eval, saw_class, unknown, suspendable);
            return;
        }
        case ASTNode::Type::TRY_STATEMENT: {
            const auto* n = static_cast<const TryStatement*>(node);
            collect_closure_names(n->get_try_block(), inside_closure, out, saw_eval, saw_class, unknown, suspendable);
            if (const ASTNode* cc = n->get_catch_clause())
                collect_closure_names(static_cast<const CatchClause*>(cc)->get_body(), inside_closure, out, saw_eval, saw_class, unknown, suspendable);
            collect_closure_names(n->get_finally_block(), inside_closure, out, saw_eval, saw_class, unknown, suspendable);
            return;
        }
        case ASTNode::Type::SWITCH_STATEMENT: {
            const auto* n = static_cast<const SwitchStatement*>(node);
            collect_closure_names(n->get_discriminant(), inside_closure, out, saw_eval, saw_class, unknown, suspendable);
            for (const auto& c : n->get_cases()) {
                const auto* cc = static_cast<const CaseClause*>(c.get());
                collect_closure_names(cc->get_test(), inside_closure, out, saw_eval, saw_class, unknown, suspendable);
                for (const auto& st : cc->get_consequent())
                    collect_closure_names(st.get(), inside_closure, out, saw_eval, saw_class, unknown, suspendable);
            }
            return;
        }
        case ASTNode::Type::LABELED_STATEMENT:
            collect_closure_names(static_cast<const LabeledStatement*>(node)->get_statement(), inside_closure, out, saw_eval, saw_class, unknown, suspendable);
            return;
        case ASTNode::Type::EXPRESSION_STATEMENT:
            collect_closure_names(static_cast<const ExpressionStatement*>(node)->get_expression(), inside_closure, out, saw_eval, saw_class, unknown, suspendable);
            return;
        case ASTNode::Type::RETURN_STATEMENT: {
            // Suspendable: return's argument also delegates to the tree-walker.
            const auto* n = static_cast<const ReturnStatement*>(node);
            collect_closure_names(n->get_argument(), suspendable ? true : inside_closure,
                                  out, saw_eval, saw_class, unknown, suspendable);
            return;
        }
        case ASTNode::Type::THROW_STATEMENT:
            collect_closure_names(static_cast<const ThrowStatement*>(node)->get_expression(), inside_closure, out, saw_eval, saw_class, unknown, suspendable);
            return;
        case ASTNode::Type::VARIABLE_DECLARATION: {
            const auto* n = static_cast<const VariableDeclaration*>(node);
            for (const auto& d : n->get_declarations())
                collect_closure_names(d->get_init(), inside_closure, out, saw_eval, saw_class, unknown, suspendable);
            return;
        }
        case ASTNode::Type::DESTRUCTURING_ASSIGNMENT: {
            // Delegates whole to the tree-walker when used bare, so every
            // name it touches must be env-resident regardless of the ambient
            // inside_closure (same as YIELD/AWAIT below).
            const auto* n = static_cast<const DestructuringAssignment*>(node);
            collect_closure_names(n->get_source(), true, out, saw_eval, saw_class, unknown, suspendable);
            for (const auto& t : n->get_targets())
                collect_closure_names(t.get(), true, out, saw_eval, saw_class, unknown, suspendable);
            for (const auto& pm : n->get_property_mappings()) {
                out.insert(pm.variable_name);
                if (pm.computed_key)
                    collect_closure_names(pm.computed_key.get(), true, out, saw_eval, saw_class, unknown, suspendable);
            }
            for (const auto& dv : n->get_default_values())
                collect_closure_names(dv.expr.get(), true, out, saw_eval, saw_class, unknown, suspendable);
            if (n->get_nested_rest_pattern())
                collect_closure_names(n->get_nested_rest_pattern(), true, out, saw_eval, saw_class, unknown, suspendable);
            return;
        }
        case ASTNode::Type::ASSIGNMENT_EXPRESSION: {
            // An array/object-literal LHS delegates whole to the tree-walker,
            // same env-residency forcing as DESTRUCTURING_ASSIGNMENT above.
            const auto* n = static_cast<const AssignmentExpression*>(node);
            bool is_pattern = n->get_left()->get_type() == ASTNode::Type::ARRAY_LITERAL ||
                              n->get_left()->get_type() == ASTNode::Type::OBJECT_LITERAL;
            bool forced = is_pattern ? true : inside_closure;
            collect_closure_names(n->get_left(), forced, out, saw_eval, saw_class, unknown, suspendable);
            collect_closure_names(n->get_right(), forced, out, saw_eval, saw_class, unknown, suspendable);
            return;
        }
        case ASTNode::Type::UNARY_EXPRESSION:
            collect_closure_names(static_cast<const UnaryExpression*>(node)->get_operand(), inside_closure, out, saw_eval, saw_class, unknown, suspendable);
            return;
        case ASTNode::Type::BINARY_EXPRESSION: {
            const auto* n = static_cast<const BinaryExpression*>(node);
            collect_closure_names(n->get_left(), inside_closure, out, saw_eval, saw_class, unknown, suspendable);
            collect_closure_names(n->get_right(), inside_closure, out, saw_eval, saw_class, unknown, suspendable);
            return;
        }
        case ASTNode::Type::NULLISH_COALESCING_EXPRESSION: {
            const auto* n = static_cast<const NullishCoalescingExpression*>(node);
            collect_closure_names(n->get_left(), inside_closure, out, saw_eval, saw_class, unknown, suspendable);
            collect_closure_names(n->get_right(), inside_closure, out, saw_eval, saw_class, unknown, suspendable);
            return;
        }
        case ASTNode::Type::CONDITIONAL_EXPRESSION: {
            const auto* n = static_cast<const ConditionalExpression*>(node);
            collect_closure_names(n->get_test(), inside_closure, out, saw_eval, saw_class, unknown, suspendable);
            collect_closure_names(n->get_consequent(), inside_closure, out, saw_eval, saw_class, unknown, suspendable);
            collect_closure_names(n->get_alternate(), inside_closure, out, saw_eval, saw_class, unknown, suspendable);
            return;
        }
        case ASTNode::Type::CALL_EXPRESSION: {
            // Spread args delegate whole (see compile_expression) -- force
            // residency same as a complex object literal above.
            const auto* n = static_cast<const CallExpression*>(node);
            bool forced = inside_closure || has_spread(n->get_arguments());
            collect_closure_names(n->get_callee(), forced, out, saw_eval, saw_class, unknown, suspendable);
            for (const auto& arg : n->get_arguments())
                collect_closure_names(arg.get(), forced, out, saw_eval, saw_class, unknown, suspendable);
            return;
        }
        case ASTNode::Type::NEW_EXPRESSION: {
            const auto* n = static_cast<const NewExpression*>(node);
            bool forced = inside_closure || has_spread(n->get_arguments());
            collect_closure_names(n->get_constructor(), forced, out, saw_eval, saw_class, unknown, suspendable);
            for (const auto& arg : n->get_arguments())
                collect_closure_names(arg.get(), forced, out, saw_eval, saw_class, unknown, suspendable);
            return;
        }
        case ASTNode::Type::MEMBER_EXPRESSION: {
            // `x.name` references only `x` -- a non-computed property is a name.
            const auto* n = static_cast<const MemberExpression*>(node);
            collect_closure_names(n->get_object(), inside_closure, out, saw_eval, saw_class, unknown, suspendable);
            if (n->is_computed())
                collect_closure_names(n->get_property(), inside_closure, out, saw_eval, saw_class, unknown, suspendable);
            return;
        }
        case ASTNode::Type::OPTIONAL_CHAINING_EXPRESSION: {
            const auto* n = static_cast<const OptionalChainingExpression*>(node);
            collect_closure_names(n->get_object(), inside_closure, out, saw_eval, saw_class, unknown, suspendable);
            if (n->is_computed())
                collect_closure_names(n->get_property(), inside_closure, out, saw_eval, saw_class, unknown, suspendable);
            return;
        }
        case ASTNode::Type::SPREAD_ELEMENT:
            collect_closure_names(static_cast<const SpreadElement*>(node)->get_argument(), inside_closure, out, saw_eval, saw_class, unknown, suspendable);
            return;
        case ASTNode::Type::TEMPLATE_LITERAL: {
            const auto* n = static_cast<const TemplateLiteral*>(node);
            for (const auto& el : n->get_elements())
                if (el.type == TemplateLiteral::Element::Type::EXPRESSION)
                    collect_closure_names(el.expression.get(), inside_closure, out, saw_eval, saw_class, unknown, suspendable);
            return;
        }
        case ASTNode::Type::OBJECT_LITERAL: {
            // A complex literal delegates whole (see compile_expression) --
            // same env-residency forcing as DESTRUCTURING_ASSIGNMENT above.
            const auto* n = static_cast<const ObjectLiteral*>(node);
            bool forced = inside_closure || object_literal_is_complex(n);
            for (const auto& prop : n->get_properties()) {
                if (prop->computed && prop->key)
                    collect_closure_names(prop->key.get(), forced, out, saw_eval, saw_class, unknown, suspendable);
                if (prop->value)
                    collect_closure_names(prop->value.get(), forced, out, saw_eval, saw_class, unknown, suspendable);
            }
            return;
        }
        case ASTNode::Type::ARRAY_LITERAL: {
            const auto* n = static_cast<const ArrayLiteral*>(node);
            bool forced = inside_closure || has_spread(n->get_elements());
            for (const auto& el : n->get_elements())
                collect_closure_names(el.get(), forced, out, saw_eval, saw_class, unknown, suspendable);
            return;
        }
        // Delegated to the tree-walker, like a closure (see emit_treewalker_delegate).
        case ASTNode::Type::YIELD_EXPRESSION: {
            const auto* n = static_cast<const YieldExpression*>(node);
            if (n->get_argument())
                collect_closure_names(n->get_argument(), true, out, saw_eval, saw_class, unknown, suspendable);
            return;
        }
        case ASTNode::Type::AWAIT_EXPRESSION: {
            const auto* n = static_cast<const AwaitExpression*>(node);
            if (n->get_argument())
                collect_closure_names(n->get_argument(), true, out, saw_eval, saw_class, unknown, suspendable);
            return;
        }
        default:
            unknown = true;
            return;
    }
}

}

bool BytecodeCompiler::references_identifier(const ASTNode* node, const std::string& name) {
    std::unordered_set<std::string> names;
    bool saw_eval = false, saw_class = false, unknown = false;
    collect_closure_names(node, /*inside_closure=*/true, names, saw_eval, saw_class, unknown);
    return saw_eval || saw_class || unknown || names.count(name) > 0;
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
            for (const auto& dv : n->get_default_values()) {
                if (uses_super_or_private(dv.expr.get())) return true;
            }
            return false;
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
bool contains_suspend(const ASTNode* node) {
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
            return n->is_arrow() && contains_suspend(n->get_body());
        }
        case ASTNode::Type::ARROW_FUNCTION_EXPRESSION:
            return contains_suspend(static_cast<const ArrowFunctionExpression*>(node)->get_body());
        case ASTNode::Type::BLOCK_STATEMENT: {
            const auto* n = static_cast<const BlockStatement*>(node);
            for (const auto& s : n->get_statements()) if (contains_suspend(s.get())) return true;
            return false;
        }
        case ASTNode::Type::IF_STATEMENT: {
            const auto* n = static_cast<const IfStatement*>(node);
            return contains_suspend(n->get_test()) || contains_suspend(n->get_consequent()) ||
                   contains_suspend(n->get_alternate());
        }
        case ASTNode::Type::WHILE_STATEMENT: {
            const auto* n = static_cast<const WhileStatement*>(node);
            return contains_suspend(n->get_test()) || contains_suspend(n->get_body());
        }
        case ASTNode::Type::DO_WHILE_STATEMENT: {
            const auto* n = static_cast<const DoWhileStatement*>(node);
            return contains_suspend(n->get_body()) || contains_suspend(n->get_test());
        }
        case ASTNode::Type::FOR_STATEMENT: {
            const auto* n = static_cast<const ForStatement*>(node);
            return contains_suspend(n->get_init()) || contains_suspend(n->get_test()) ||
                   contains_suspend(n->get_update()) || contains_suspend(n->get_body());
        }
        case ASTNode::Type::FOR_OF_STATEMENT: {
            const auto* n = static_cast<const ForOfStatement*>(node);
            return contains_suspend(n->get_right()) || contains_suspend(n->get_body());
        }
        case ASTNode::Type::FOR_IN_STATEMENT: {
            const auto* n = static_cast<const ForInStatement*>(node);
            return contains_suspend(n->get_right()) || contains_suspend(n->get_body());
        }
        case ASTNode::Type::TRY_STATEMENT: {
            const auto* n = static_cast<const TryStatement*>(node);
            if (contains_suspend(n->get_try_block())) return true;
            if (const ASTNode* cc = n->get_catch_clause()) {
                if (contains_suspend(static_cast<const CatchClause*>(cc)->get_body())) return true;
            }
            return contains_suspend(n->get_finally_block());
        }
        case ASTNode::Type::SWITCH_STATEMENT: {
            const auto* n = static_cast<const SwitchStatement*>(node);
            if (contains_suspend(n->get_discriminant())) return true;
            for (const auto& c : n->get_cases()) {
                const auto* cc = static_cast<const CaseClause*>(c.get());
                if (cc->get_test() && contains_suspend(cc->get_test())) return true;
                for (const auto& s : cc->get_consequent()) if (contains_suspend(s.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::LABELED_STATEMENT:
            return contains_suspend(static_cast<const LabeledStatement*>(node)->get_statement());
        case ASTNode::Type::EXPRESSION_STATEMENT:
            return contains_suspend(static_cast<const ExpressionStatement*>(node)->get_expression());
        case ASTNode::Type::RETURN_STATEMENT: {
            const auto* n = static_cast<const ReturnStatement*>(node);
            return n->get_argument() && contains_suspend(n->get_argument());
        }
        case ASTNode::Type::THROW_STATEMENT:
            return contains_suspend(static_cast<const ThrowStatement*>(node)->get_expression());
        case ASTNode::Type::VARIABLE_DECLARATION: {
            const auto* n = static_cast<const VariableDeclaration*>(node);
            for (const auto& d : n->get_declarations()) {
                if (d->get_init() && contains_suspend(d->get_init())) return true;
            }
            return false;
        }
        case ASTNode::Type::DESTRUCTURING_ASSIGNMENT: {
            const auto* n = static_cast<const DestructuringAssignment*>(node);
            if (n->get_source() && contains_suspend(n->get_source())) return true;
            for (const auto& dv : n->get_default_values()) {
                if (contains_suspend(dv.expr.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::ASSIGNMENT_EXPRESSION: {
            const auto* n = static_cast<const AssignmentExpression*>(node);
            return contains_suspend(n->get_left()) || contains_suspend(n->get_right());
        }
        case ASTNode::Type::UNARY_EXPRESSION:
            return contains_suspend(static_cast<const UnaryExpression*>(node)->get_operand());
        case ASTNode::Type::BINARY_EXPRESSION: {
            const auto* n = static_cast<const BinaryExpression*>(node);
            return contains_suspend(n->get_left()) || contains_suspend(n->get_right());
        }
        case ASTNode::Type::NULLISH_COALESCING_EXPRESSION: {
            const auto* n = static_cast<const NullishCoalescingExpression*>(node);
            return contains_suspend(n->get_left()) || contains_suspend(n->get_right());
        }
        case ASTNode::Type::CONDITIONAL_EXPRESSION: {
            const auto* n = static_cast<const ConditionalExpression*>(node);
            return contains_suspend(n->get_test()) || contains_suspend(n->get_consequent()) ||
                   contains_suspend(n->get_alternate());
        }
        case ASTNode::Type::CALL_EXPRESSION: {
            const auto* n = static_cast<const CallExpression*>(node);
            if (contains_suspend(n->get_callee())) return true;
            for (const auto& a : n->get_arguments()) if (contains_suspend(a.get())) return true;
            return false;
        }
        case ASTNode::Type::NEW_EXPRESSION: {
            const auto* n = static_cast<const NewExpression*>(node);
            if (contains_suspend(n->get_constructor())) return true;
            for (const auto& a : n->get_arguments()) if (contains_suspend(a.get())) return true;
            return false;
        }
        case ASTNode::Type::MEMBER_EXPRESSION: {
            const auto* n = static_cast<const MemberExpression*>(node);
            return contains_suspend(n->get_object()) ||
                   (n->is_computed() && contains_suspend(n->get_property()));
        }
        case ASTNode::Type::OPTIONAL_CHAINING_EXPRESSION: {
            const auto* n = static_cast<const OptionalChainingExpression*>(node);
            return contains_suspend(n->get_object()) ||
                   (n->is_computed() && contains_suspend(n->get_property()));
        }
        case ASTNode::Type::SPREAD_ELEMENT:
            return contains_suspend(static_cast<const SpreadElement*>(node)->get_argument());
        case ASTNode::Type::TEMPLATE_LITERAL: {
            const auto* n = static_cast<const TemplateLiteral*>(node);
            for (const auto& el : n->get_elements()) {
                if (el.type == TemplateLiteral::Element::Type::EXPRESSION &&
                    contains_suspend(el.expression.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::OBJECT_LITERAL: {
            const auto* n = static_cast<const ObjectLiteral*>(node);
            for (const auto& p : n->get_properties()) {
                if (p->value && contains_suspend(p->value.get())) return true;
                if (p->computed && p->key && contains_suspend(p->key.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::ARRAY_LITERAL: {
            const auto* n = static_cast<const ArrayLiteral*>(node);
            for (const auto& el : n->get_elements()) {
                if (el && contains_suspend(el.get())) return true;
            }
            return false;
        }
        default:
            return false;
    }
}


// True if `node` contains a `let/const/var [a,b]=...` declaration anywhere.
// Forces env_mode: Op::DestructureBind binds through a real Environment.
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
bool contains_nested_lexical_decl(const BlockStatement* top_level_body) {
    for (const auto& stmt : top_level_body->get_statements()) {
        if (stmt->get_type() == ASTNode::Type::VARIABLE_DECLARATION) continue;
        if (contains_lexical_decl(stmt.get())) return true;
    }
    return false;
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
bool contains_control_escape(const ASTNode* node) {
    if (!node) return false;
    switch (node->get_type()) {
        case ASTNode::Type::RETURN_STATEMENT:
        case ASTNode::Type::BREAK_STATEMENT:
        case ASTNode::Type::CONTINUE_STATEMENT:
            return true;
        case ASTNode::Type::BLOCK_STATEMENT: {
            const auto* n = static_cast<const BlockStatement*>(node);
            for (const auto& stmt : n->get_statements()) {
                if (contains_control_escape(stmt.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::IF_STATEMENT: {
            const auto* n = static_cast<const IfStatement*>(node);
            return contains_control_escape(n->get_consequent()) ||
                   contains_control_escape(n->get_alternate());
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
            if (contains_control_escape(n->get_try_block())) return true;
            if (const ASTNode* cc = n->get_catch_clause()) {
                if (contains_control_escape(static_cast<const CatchClause*>(cc)->get_body())) return true;
            }
            return contains_control_escape(n->get_finally_block());
        }
        case ASTNode::Type::SWITCH_STATEMENT: {
            const auto* n = static_cast<const SwitchStatement*>(node);
            for (const auto& c : n->get_cases()) {
                for (const auto& s : static_cast<const CaseClause*>(c.get())->get_consequent()) {
                    if (contains_control_escape(s.get())) return true;
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

// Recursively collects for-header lexical names (`for (let i = ...; ...)`,
// C-style only -- for-of/for-in headers use a different, already-safe
// mechanism). These are the only nested lexicals whose register inclusion
// selective env_mode allows: their per-iteration scope means an outer
// reference to the same name is a different binding, not a leak, and this
// is where the register refinement's real performance win comes from
// (arith_loop/properties). Every OTHER nested lexical (block/switch/catch)
// stays env-resident unconditionally, since a register isn't block-scoped
// and would otherwise leak past the owning block (see scope-lex-const.js).
void collect_for_header_names(const ASTNode* node, std::unordered_set<std::string>& out) {
    if (!node) return;
    switch (node->get_type()) {
        case ASTNode::Type::FOR_STATEMENT: {
            const auto* n = static_cast<const ForStatement*>(node);
            if (n->get_init() && n->get_init()->get_type() == ASTNode::Type::VARIABLE_DECLARATION) {
                const auto* vd = static_cast<const VariableDeclaration*>(n->get_init());
                if (vd->get_kind() != VariableDeclarator::Kind::VAR) {
                    for (const auto& d : vd->get_declarations()) {
                        if (d->get_id()) out.insert(d->get_id()->get_name());
                    }
                }
            }
            collect_for_header_names(n->get_body(), out);
            return;
        }
        case ASTNode::Type::BLOCK_STATEMENT: {
            const auto* n = static_cast<const BlockStatement*>(node);
            for (const auto& stmt : n->get_statements()) collect_for_header_names(stmt.get(), out);
            return;
        }
        case ASTNode::Type::IF_STATEMENT: {
            const auto* n = static_cast<const IfStatement*>(node);
            collect_for_header_names(n->get_consequent(), out);
            collect_for_header_names(n->get_alternate(), out);
            return;
        }
        case ASTNode::Type::WHILE_STATEMENT:
            collect_for_header_names(static_cast<const WhileStatement*>(node)->get_body(), out);
            return;
        case ASTNode::Type::DO_WHILE_STATEMENT:
            collect_for_header_names(static_cast<const DoWhileStatement*>(node)->get_body(), out);
            return;
        case ASTNode::Type::FOR_OF_STATEMENT:
            collect_for_header_names(static_cast<const ForOfStatement*>(node)->get_body(), out);
            return;
        case ASTNode::Type::FOR_IN_STATEMENT:
            collect_for_header_names(static_cast<const ForInStatement*>(node)->get_body(), out);
            return;
        case ASTNode::Type::TRY_STATEMENT: {
            const auto* n = static_cast<const TryStatement*>(node);
            collect_for_header_names(n->get_try_block(), out);
            if (const ASTNode* cc = n->get_catch_clause())
                collect_for_header_names(static_cast<const CatchClause*>(cc)->get_body(), out);
            collect_for_header_names(n->get_finally_block(), out);
            return;
        }
        case ASTNode::Type::SWITCH_STATEMENT: {
            const auto* n = static_cast<const SwitchStatement*>(node);
            for (const auto& c : n->get_cases()) {
                for (const auto& s : static_cast<const CaseClause*>(c.get())->get_consequent())
                    collect_for_header_names(s.get(), out);
            }
            return;
        }
        case ASTNode::Type::LABELED_STATEMENT:
            collect_for_header_names(static_cast<const LabeledStatement*>(node)->get_statement(), out);
            return;
        default:
            return;
    }
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

std::unique_ptr<BytecodeChunk> BytecodeCompiler::compile(
    const ASTNode* body, const std::vector<std::unique_ptr<Parameter>>& params,
    bool suspendable) {
    if (!body || body->get_type() != ASTNode::Type::BLOCK_STATEMENT) return nullptr;
    if (params.size() > 64) return nullptr;

    // Default/destructured/rest parameters force env_mode: rest needs a
    // fresh (not run()-auto-bound) slot, and destructuring only knows how
    // to bind through a real Environment (see Op::DestructureBind).
    bool has_complex_params = false;
    for (const auto& p : params) {
        if (p->has_default() || p->has_destructuring() || p->is_rest()) {
            has_complex_params = true;
            break;
        }
    }
    // Rest is always the last parameter (grammar); everything before it
    // occupies a fixed position that run() auto-binds by index.
    bool has_rest = !params.empty() && params.back()->is_rest();
    size_t fixed_param_count = has_rest ? params.size() - 1 : params.size();

    std::vector<std::string> param_names;  // excludes rest -- see CreateRestArray below
    std::string rest_name;
    for (const auto& p : params) {
        const std::string& pname = p->get_name()->get_name();
        if (pname == "arguments") return nullptr;
        if (p->is_rest()) {
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

    // `arguments` forces env_mode too: its mapped accessors (sloppy mode,
    // simple params) read/write the parameter bindings through the context,
    // which register-mode parameters don't have. A default/destructuring
    // expression can reference it too (spec: it exists during parameter
    // evaluation already).
    bool needs_arguments = uses_arguments(body);
    for (const auto& p : params) {
        if (p->has_default() && uses_arguments(p->get_default_value())) needs_arguments = true;
        if (p->has_destructuring() && uses_arguments(p->get_destructuring_pattern())) needs_arguments = true;
    }

    // Shadowing is covered too: a true duplicate name always has a nested
    // occurrence (a top-level dup is already a parser SyntaxError).
    // super/private-name access also forces env_mode: those forms delegate
    // to the tree-walker's own evaluate() and need `this`/`__super__`/brand
    // bindings and any captured locals resolvable through a real Environment.
    // Suspendable bodies always use env_mode: locals must survive across the
    // fiber suspension that delegated yield/await expressions perform.
    bool has_closures = contains_closure(body);
    bool has_nested_lex = contains_nested_lexical_decl(static_cast<const BlockStatement*>(body));
    // Bare destructuring assignments and complex object literals delegate to
    // the tree-walker, so they need env_mode like a suspendable's yield/await.
    bool has_delegated_expr = contains_delegated_expr(body);
    // Private access no longer forces env_mode: GetPrivate/SetPrivate resolve
    // brands through the CallStack, not the env chain. `super` still needs the
    // env (__super__/__home_object__ bindings); detect it from a whole-body
    // name sweep, whose opacity flags force full env_mode just like the
    // selective scan below. Delegated private forms that do need an env
    // (#x in obj, delete this.#x) hit emit_treewalker_delegate's guards and
    // fall back to the tree-walker.
    std::unordered_set<std::string> all_names;
    bool an_eval = false, an_class = false, an_unknown = false;
    collect_closure_names(body, /*inside_closure=*/true, all_names, an_eval, an_class, an_unknown, suspendable);
    bool full_env = has_complex_params || needs_arguments ||
                    contains_destructuring(body) || an_class || an_unknown ||
                    all_names.count("super") > 0;

    // Selective env_mode: only names a closure (or a suspendable body's own
    // yield/await/return delegate) can observe, or that need a runtime
    // scope (nested lexicals, catch params, hoisted function names), stay
    // Environment-resident; every other local gets a register. Any
    // opacity -- eval inside a closure, class bodies, an AST form the
    // scanner doesn't know -- falls back to full env_mode.
    std::unordered_set<std::string> env_resident;
    bool selective = false;
    if (!full_env && (has_closures || has_nested_lex || suspendable || has_delegated_expr)) {
        bool saw_eval = false, saw_class = false, unknown = false;
        collect_closure_names(body, /*inside_closure=*/false, env_resident,
                              saw_eval, saw_class, unknown, suspendable);
        if (saw_eval || saw_class || unknown) {
            full_env = true;
        } else {
            for (const auto& stmt : static_cast<const BlockStatement*>(body)->get_statements()) {
                if (stmt->get_type() != ASTNode::Type::FUNCTION_DECLARATION) continue;
                const auto* fd = static_cast<const FunctionDeclaration*>(stmt.get());
                if (fd->get_id()) env_resident.insert(fd->get_id()->get_name());
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
        std::unordered_set<std::string> for_header_names;
        collect_for_header_names(body, for_header_names);
        for (const auto& info : declared_pre) {
            if (info.is_catch_param) {
                env_resident.insert(info.name);
            } else if (info.is_lexical && !direct_pre.count(info.name) &&
                       (decl_count[info.name] > 1 || !for_header_names.count(info.name))) {
                env_resident.insert(info.name);
            }
        }
        // Nothing ended up captured: the whole function is register-pure
        // (closures still pin the env for CreateClosure delegation; a
        // suspendable body or a delegated expr -- e.g. `[]=x;`, no targets --
        // keeps env_mode on regardless, see env_mode2).
        if (env_resident.empty() && !has_closures && !suspendable && !has_delegated_expr) {
            selective = false;
        }
    }
    // Suspendable bodies and delegated expressions always keep env_mode on:
    // emit_treewalker_delegate requires it to delegate at all.
    bool env_mode2 = full_env || has_closures || !env_resident.empty() || suspendable || has_delegated_expr;

    const bool env_mode = env_mode2;
    BytecodeCompiler compiler(param_names, env_mode, selective ? &env_resident : nullptr);
    if (compiler.failed_) return nullptr;
    compiler.allow_arguments_ = needs_arguments;
    compiler.suspendable_ = suspendable;
    if (has_rest) {
        if (!env_mode || !compiler.env_names_.insert(rest_name).second) return nullptr;
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
        // A local named "arguments" needs the implicit arguments-object
        // hoisting semantics neither storage mode replicates.
        if (info.name == "arguments") return nullptr;
        for (const auto& p : param_names) {
            if (p == info.name) return nullptr;  // param/local aliasing: stay on tree-walker
        }
        if (has_rest && rest_name == info.name) return nullptr;
        // Runtime const-immutability isn't implemented for the register path;
        // refuse rather than compile an incorrectly-mutable const.
        if (info.is_const && assigns_to_identifier(body, info.name)) return nullptr;

        bool resident = env_mode && (!selective || env_resident.count(info.name) > 0);
        if (resident) {
            // A repeat declare_local (shadowed name) is fine -- the Environment
            // chain resolves each occurrence to its own scope at runtime.
            compiler.declare_local(info.name);
            if (!info.is_catch_param &&
                (!info.is_lexical || direct_lexical_names.count(info.name))) {
                compiler.chunk_->env_locals.push_back({info.name, info.is_lexical, info.is_const});
            }
        } else {
            if (!compiler.declare_local(info.name)) return nullptr;
            if (info.is_lexical) {
                compiler.lexical_registers_.insert(compiler.lookup_local(info.name));
            }
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
            compiler.emit(Op::StaEnvInit);
            compiler.emit_u16(compiler.add_name(param_names[i]));
        }
    }
    // Non-rest parameters' function-entry bindings are data-driven from the
    // chunk (env_params), set up once by VM::run -- no bytecode needed for
    // those. Rest gets its own immediately-initialized slot here instead,
    // since CreateRestArray/DestructureBind below fills it, not run().
    if (has_rest) {
        compiler.chunk_->env_locals.push_back({rest_name, false, false});
    }

    // Parameter lists with initializers follow spec FDI ordering (see
    // BytecodeChunk::env_params_tdz): params seed uninitialized, and each
    // one initializes left to right from its register-held raw argument.
    // A default whose value closes over the environment would capture the
    // body's (deferred) bindings through the flat function env -- the spec
    // gives parameter expressions their own scope, so those stay on the
    // tree-walker.
    bool params_tdz = false;
    for (const auto& p : params) {
        if (p->is_rest()) continue;
        if (p->has_default() || p->has_destructuring()) params_tdz = true;
    }
    if (params_tdz) {
        for (const auto& p : params) {
            if ((p->has_default() && contains_closure(p->get_default_value())) ||
                (p->has_destructuring() && contains_closure(p->get_destructuring_pattern()))) {
                return nullptr;
            }
        }
    }

    // Defaults and destructuring resolve once at entry, before the body --
    // in TDZ mode the raw values come from registers (the env binding is
    // still uninitialized); otherwise run() already bound plain parameters.
    {
        uint8_t param_index = 0;
        for (const auto& p : params) {
            if (p->is_rest()) continue;  // handled below, after BindEnvLocals
            if (!params_tdz && !p->has_default() && !p->has_destructuring()) {
                param_index++;
                continue;
            }
            const std::string& pname = p->get_name()->get_name();
            if (params_tdz) {
                compiler.emit(Op::Ldar);
                compiler.emit_u8(param_index);
            } else {
                compiler.emit_read_local(pname);
            }
            if (p->has_default()) {
                size_t skip = compiler.emit_jump(Op::JumpIfNotUndefined);
                if (!compiler.compile_expression(p->get_default_value())) return nullptr;
                if (!compiler.patch_jump(skip)) return nullptr;
            }
            if (p->has_destructuring()) {
                auto* destr = static_cast<DestructuringAssignment*>(p->get_destructuring_pattern());
                compiler.chunk_->destructuring_patterns.push_back({destr, true, false});
                compiler.emit(Op::DestructureBind);
                compiler.emit_u16(static_cast<uint16_t>(compiler.chunk_->destructuring_patterns.size() - 1));
            } else if (params_tdz) {
                compiler.emit(Op::StaEnvInit);
                compiler.emit_u16(compiler.add_name(pname));
            } else {
                compiler.emit_write_local(pname, false);
            }
            param_index++;
        }
    }
    if (params_tdz) compiler.emit(Op::BindEnvLocals);
    if (has_rest) {
        const auto& p = params.back();
        compiler.emit(Op::CreateRestArray);
        compiler.emit_u8(static_cast<uint8_t>(fixed_param_count));
        if (p->has_destructuring()) {
            auto* destr = static_cast<DestructuringAssignment*>(p->get_destructuring_pattern());
            compiler.chunk_->destructuring_patterns.push_back({destr, true, false});
            compiler.emit(Op::DestructureBind);
            compiler.emit_u16(static_cast<uint16_t>(compiler.chunk_->destructuring_patterns.size() - 1));
        } else {
            compiler.emit_write_local(rest_name, false);
        }
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
        if (compiler.chunk_->closures.size() >= 0xFFFF) return nullptr;
        compiler.hoisted_fn_decls_.insert(stmt.get());
        compiler.chunk_->closures.push_back(stmt.get());
        compiler.emit(Op::CreateClosure);
        compiler.emit_u16(static_cast<uint16_t>(compiler.chunk_->closures.size() - 1));
    }

    for (const auto& stmt : block->get_statements()) {
        if (!compiler.compile_statement(stmt.get())) return nullptr;
    }

    // Falling off the end returns undefined.
    compiler.emit(Op::LdaUndefined);
    compiler.emit(Op::Return);

    compiler.chunk_->register_count = static_cast<uint16_t>(compiler.temp_watermark_);
    compiler.chunk_->parameter_count = static_cast<uint8_t>(param_names.size());
    compiler.chunk_->env_mode = env_mode;
    compiler.chunk_->env_params_tdz = params_tdz;
    compiler.chunk_->needs_arguments = needs_arguments;
    if (env_mode && !selective) compiler.chunk_->env_params = param_names;
    compiler.chunk_->lookup_cache.assign(compiler.chunk_->names.size(),
                                         BytecodeChunk::LookupCacheEntry{});
    return std::move(compiler.chunk_);
}

std::unique_ptr<BytecodeChunk> BytecodeCompiler::compile_script(
    const std::vector<std::unique_ptr<ASTNode>>& statements) {
    // Scan the whole program once. Modules and anything opaque to the
    // scanners tree-walk; class definitions and closure-side eval only
    // disable the nested-register refinement (top-level names are outer
    // bindings either way, so correctness doesn't depend on the scan).
    std::unordered_set<std::string> closure_names;
    bool saw_eval = false, saw_class = false, unknown = false;
    for (const auto& st : statements) {
        auto t = st->get_type();
        if (t == ASTNode::Type::EXPORT_STATEMENT || t == ASTNode::Type::IMPORT_STATEMENT) {
            return nullptr;
        }
        if (contains_destructuring(st.get())) return nullptr;
        collect_closure_names(st.get(), /*inside_closure=*/false, closure_names,
                              saw_eval, saw_class, unknown);
    }
    if (saw_eval) return nullptr;  // a closure's eval text can name anything
    bool refine = !saw_class && !unknown;

    // Top-level names are pre-hoisted outer bindings (vars on the global,
    // let/const/class in the script env, function declarations already
    // evaluated) -- the compiler must treat them as non-locals.
    std::unordered_set<std::string> top_names;
    for (const auto& st : statements) {
        const ASTNode* eff = st.get();
        if (eff->get_type() == ASTNode::Type::VARIABLE_DECLARATION) {
            const auto* vd = static_cast<const VariableDeclaration*>(eff);
            for (const auto& d : vd->get_declarations()) {
                if (d->get_id()) top_names.insert(d->get_id()->get_name());
            }
        } else if (eff->get_type() == ASTNode::Type::CLASS_DECLARATION) {
            const auto* cd = static_cast<const ClassDeclaration*>(eff);
            if (cd->get_id()) top_names.insert(cd->get_id()->get_name());
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
    std::unordered_map<std::string, int> decl_count;
    for (const auto& info : declared) decl_count[info.name]++;
    std::unordered_set<std::string> for_header_names;
    for (const auto& st : statements) collect_for_header_names(st.get(), for_header_names);
    std::unordered_set<std::string> env_resident;
    for (const auto& info : declared) {
        if (!info.is_lexical && !info.is_catch_param) continue;  // nested var: global
        if (!for_header_names.count(info.name) || info.is_catch_param || !refine ||
            closure_names.count(info.name) || decl_count[info.name] > 1 ||
            top_names.count(info.name)) {
            env_resident.insert(info.name);
        }
    }

    BytecodeCompiler compiler({}, /*env_mode=*/true, &env_resident);
    if (compiler.failed_) return nullptr;
    compiler.script_mode_ = true;
    for (const auto& info : declared) {
        if (!info.is_lexical && !info.is_catch_param) continue;
        if (env_resident.count(info.name)) {
            compiler.declare_local(info.name);
        } else {
            if (!compiler.declare_local(info.name)) return nullptr;
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

    for (const auto& st : statements) {
        if (st->get_type() == ASTNode::Type::FUNCTION_DECLARATION) continue;  // pre-evaluated
        if (!compiler.compile_statement(st.get())) return nullptr;
    }
    compiler.emit(Op::LdaUndefined);
    compiler.emit(Op::Return);

    compiler.chunk_->register_count = static_cast<uint16_t>(compiler.temp_watermark_);
    compiler.chunk_->parameter_count = 0;
    compiler.chunk_->env_mode = true;
    compiler.chunk_->script_mode = true;
    compiler.chunk_->lookup_cache.assign(compiler.chunk_->names.size(),
                                         BytecodeChunk::LookupCacheEntry{});
    return std::move(compiler.chunk_);
}

BytecodeCompiler::BytecodeCompiler(const std::vector<std::string>& param_names, bool env_mode,
                                   const std::unordered_set<std::string>* env_resident)
    : chunk_(std::make_unique<BytecodeChunk>()), env_mode_(env_mode) {
    if (env_resident) {
        full_env_ = false;
        env_resident_ = *env_resident;
    }
    if (env_mode_ && full_env_) {
        for (const auto& p : param_names) {
            if (!env_names_.insert(p).second) { failed_ = true; return; }
        }
        return;
    }
    // Register (or selective) mode: every param owns register i. A selective
    // env-resident param keeps its register as the entry seed source; all
    // reads/writes go through the env binding (see emit_read_local).
    for (const auto& p : param_names) {
        if (locals_.count(p)) { failed_ = true; return; }
        locals_[p] = next_register_++;
        if (env_mode_ && env_resident_.count(p)) env_names_.insert(p);
    }
}

int BytecodeCompiler::setup_loop_env(std::vector<BytecodeChunk::LoopEnvVar> extra_vars, const ASTNode* body,
                                      bool force_own_env) {
    if (!env_mode_) return -1;
    std::vector<BytecodeChunk::LoopEnvVar> vars = std::move(extra_vars);
    bool needs_own_env = force_own_env;
    if (!collect_direct_lexical_decls(body, vars, needs_own_env)) return -1;
    // Register-resident lexicals get their TDZ re-armed at block entry
    // instead of living in the per-iteration env.
    vars.erase(std::remove_if(vars.begin(), vars.end(),
                              [&](const BytecodeChunk::LoopEnvVar& v) {
                                  return !env_names_.count(v.name) && lookup_local(v.name) >= 0;
                              }),
               vars.end());
    if (vars.empty() && !needs_own_env) return -1;
    chunk_->loop_envs.push_back(std::move(vars));
    return static_cast<int>(chunk_->loop_envs.size() - 1);
}

std::vector<std::string> BytecodeCompiler::take_pending_labels() {
    std::vector<std::string> labels = std::move(pending_labels_);
    pending_labels_.clear();
    return labels;
}

bool BytecodeCompiler::compile_for_each_loop(const ASTNode* left, const ASTNode* right,
                                              const ASTNode* body, bool is_for_in,
                                              int left_decl_kind) {
    // Supported targets: a simple identifier, or a destructuring pattern WITH
    // a declaration keyword (keywordless `for ({a} of arr)` reports kind -1
    // and needs arbitrary-AssignmentTarget writeback this path doesn't have).
    std::string var_name;
    bool declare_fresh = false;
    bool is_const = false;
    bool is_lexical = false;
    const DestructuringAssignment* destr = nullptr;
    if (left->get_type() == ASTNode::Type::VARIABLE_DECLARATION) {
        const auto* vd = static_cast<const VariableDeclaration*>(left);
        if (vd->declaration_count() != 1) return false;
        const auto& d = vd->get_declarations()[0];
        if (!d->get_id()) return false;
        var_name = d->get_id()->get_name();
        declare_fresh = true;
        is_const = vd->get_kind() == VariableDeclarator::Kind::CONST;
    } else if (left->get_type() == ASTNode::Type::IDENTIFIER) {
        var_name = static_cast<const Identifier*>(left)->get_name();
    } else if (left->get_type() == ASTNode::Type::DESTRUCTURING_ASSIGNMENT && left_decl_kind >= 0) {
        destr = static_cast<const DestructuringAssignment*>(left);
        declare_fresh = true;
        is_const = left_decl_kind == 2;
        is_lexical = left_decl_kind != 0;  // 0 = var
    } else {
        return false;  // bare destructuring (no declaration keyword) / member-expression LHS
    }
    if (!destr && !is_local(var_name)) return false;

    // Entered before compiling `right`: a lexical ForDeclaration's bound name
    // is in TDZ even during the head's own iterable/object expression (spec).
    //
    // Destructuring targets must NOT be pre-listed in extra_vars: bind_or_set
    // always create_lexical_binding's fresh (no TDZ-slot-initialize mode), so
    // a pre-declared name would silently no-op after iteration 1.
    // force_own_env still gives lexicals a fresh per-iteration env.
    std::vector<BytecodeChunk::LoopEnvVar> extra_vars;
    if (destr) {
        // Nothing to push -- see the comment above.
    } else if (declare_fresh && env_mode_ && env_names_.count(var_name)) {
        extra_vars.push_back({var_name, true, is_const, false});
    }
    int loop_env_idx = setup_loop_env(std::move(extra_vars), body, /*force_own_env=*/destr && is_lexical);
    if (loop_env_idx >= 0) {
        emit(Op::EnterLoopEnv);
        emit_u16(static_cast<uint16_t>(loop_env_idx));
        env_depth_++;
    }

    if (!compile_expression(right)) return false;  // acc = iterable/object
    if (is_for_in) emit(Op::CreateForInKeys);  // acc = Array of enumerable key strings

    int next_fn_reg = alloc_temp();
    if (failed_) return false;
    emit(Op::GetIterator);
    emit_u8(static_cast<uint8_t>(next_fn_reg));
    int iterator_reg = alloc_temp();
    if (failed_) return false;
    emit(Op::Star);
    emit_u8(static_cast<uint8_t>(iterator_reg));

    if (loop_env_idx >= 0) {
        // Spec 14.7.5.6 ForIn/OfBodyEvaluation step 1: CreatePerIterationEnvironment
        // runs once more right after head evaluation (the object/iterable
        // expression, where a closure may capture the still-TDZ binding),
        // before the first per-iteration binding write -- same reasoning as
        // the C-style FOR_STATEMENT fix above.
        emit(Op::AdvanceLoopEnv);
        emit_u16(static_cast<uint16_t>(loop_env_idx));
    }

    size_t loop_start = chunk_->code.size();
    emit(Op::IteratorNextOrJump);
    emit_u8(static_cast<uint8_t>(iterator_reg));
    emit_u8(static_cast<uint8_t>(next_fn_reg));
    size_t next_jump = chunk_->code.size();
    emit_u16(0);  // patched below to pre_exit (done, or the iterator threw)

    if (destr) {
        if (chunk_->destructuring_patterns.size() >= 0xFFFF) return false;
        chunk_->destructuring_patterns.push_back({destr, is_lexical, is_const});
        emit(Op::DestructureBind);
        emit_u16(static_cast<uint16_t>(chunk_->destructuring_patterns.size() - 1));
    } else {
        emit_write_local(var_name, /*is_declaration=*/declare_fresh);
    }

    size_t body_start = chunk_->code.size();
    loop_stack_.push_back({0, {}, {}, true, env_depth_, try_env_depth_, false, take_pending_labels(), iterator_reg});
    if (!compile_statement(body)) return false;
    LoopScope scope = std::move(loop_stack_.back());
    loop_stack_.pop_back();
    size_t body_end = chunk_->code.size();

    for (size_t pos : scope.continue_patches) {
        if (!patch_jump(pos)) return false;  // continue lands on the advance step
    }
    if (loop_env_idx >= 0) {
        emit(Op::AdvanceLoopEnv);
        emit_u16(static_cast<uint16_t>(loop_env_idx));
    }
    if (!emit_jump_back(Op::Jump, loop_start)) return false;

    // break: close the iterator (validating return()'s result, per spec)
    // before falling into the same exit path as "done".
    for (size_t pos : scope.break_patches) {
        if (!patch_jump(pos)) return false;
    }
    emit(Op::IteratorClose);
    emit_u8(static_cast<uint8_t>(iterator_reg));
    emit_u8(0);  // mode 0: validate, no pending exception

    if (!patch_jump(next_jump)) return false;
    if (loop_env_idx >= 0) { emit(Op::ExitLoopEnv); env_depth_--; }

    // Normal control flow ends here; skip over the exception-cleanup
    // handler below (reached only via CHECK_EXC's handler-table jump).
    size_t skip_cleanup = emit_jump(Op::Jump);
    size_t cleanup_pc = chunk_->code.size();
    emit(Op::IteratorClose);
    emit_u8(static_cast<uint8_t>(iterator_reg));
    emit_u8(1);  // mode 1: acc holds the pending exception -- restore + re-raise
    chunk_->handlers.push_back({static_cast<uint32_t>(body_start),
                                 static_cast<uint32_t>(body_end),
                                 static_cast<uint32_t>(cleanup_pc)});
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

void BytecodeCompiler::emit_read_local(const std::string& name) {
    if (env_names_.count(name)) {
        emit(Op::LdaEnv);
        emit_u16(add_name(name));
        return;
    }
    int reg = lookup_local(name);
    if (lexical_registers_.count(reg)) {
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
        emit(is_declaration ? Op::StaEnvInit : Op::StaEnv);
        emit_u16(add_name(name));
        return;
    }
    int reg = lookup_local(name);
    if (!is_declaration && lexical_registers_.count(reg)) {
        emit(Op::StarChecked);
        emit_u8(static_cast<uint8_t>(reg));
        emit_u16(add_name(name));
    } else {
        emit(Op::Star);
        emit_u8(static_cast<uint8_t>(reg));
    }
}

void BytecodeCompiler::emit(Op op) { chunk_->code.push_back(static_cast<uint8_t>(op)); }
void BytecodeCompiler::emit_u8(uint8_t v) { chunk_->code.push_back(v); }
void BytecodeCompiler::emit_u16(uint16_t v) {
    chunk_->code.push_back(static_cast<uint8_t>(v & 0xFF));
    chunk_->code.push_back(static_cast<uint8_t>(v >> 8));
}

uint16_t BytecodeCompiler::add_constant(const Value& v) {
    if (chunk_->constants.size() >= 0xFFFF) { failed_ = true; return 0; }
    chunk_->constants.push_back(v);
    return static_cast<uint16_t>(chunk_->constants.size() - 1);
}

uint16_t BytecodeCompiler::add_name(const std::string& name) {
    for (size_t i = 0; i < chunk_->names.size(); i++) {
        if (chunk_->names[i] == name) return static_cast<uint16_t>(i);
    }
    if (chunk_->names.size() >= 0xFFFF) { failed_ = true; return 0; }
    chunk_->names.push_back(name);
    return static_cast<uint16_t>(chunk_->names.size() - 1);
}

uint16_t BytecodeCompiler::alloc_feedback_slot() {
    if (chunk_->feedback.size() >= 0xFFFF) { failed_ = true; return 0; }
    chunk_->feedback.push_back(FeedbackSlot{});
    return static_cast<uint16_t>(chunk_->feedback.size() - 1);
}

uint16_t BytecodeCompiler::alloc_private_feedback() {
    if (chunk_->private_feedback.size() >= 0xFFFF) { failed_ = true; return 0; }
    chunk_->private_feedback.push_back(PrivateFeedback{});
    return static_cast<uint16_t>(chunk_->private_feedback.size() - 1);
}

bool BytecodeCompiler::member_is_supported(const MemberExpression* mem) const {
    if (mem->get_object()->get_type() == ASTNode::Type::IDENTIFIER &&
        static_cast<const Identifier*>(mem->get_object())->get_name() == "super") {
        return false;
    }
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
bool BytecodeCompiler::emit_treewalker_delegate(const ASTNode* node) {
    if (!env_mode_) return false;
    if (!full_env_) {
        // Selective storage: the delegated subtree resolves names through the
        // ctx chain, so a reference to a register-resident local would miss.
        // Refuse (whole function falls back to the tree-walker) rather than
        // compile wrong code.
        std::unordered_set<std::string> names;
        bool saw_eval = false, saw_class = false, unknown = false;
        collect_closure_names(node, /*inside_closure=*/true, names, saw_eval, saw_class, unknown);
        if (unknown || saw_class || saw_eval) return false;
        for (const auto& n : names) {
            if (locals_.count(n) && !env_names_.count(n)) return false;
        }
    }
    if (chunk_->closures.size() >= 0xFFFF) return false;
    chunk_->closures.push_back(node);
    emit(Op::CreateClosure);
    emit_u16(static_cast<uint16_t>(chunk_->closures.size() - 1));
    return !failed_;
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
        if (name == "eval" || name == "arguments") return false;  // strict SyntaxError forms
        if (is_named_evaluation_rhs(expr->get_right())) {
            return emit_treewalker_delegate(expr);
        }
        if (is_local(name)) {
            emit_read_local(name);
            size_t skip = emit_jump(skip_op);
            if (!compile_expression(expr->get_right())) return false;
            emit_write_local(name, /*is_declaration=*/false);
            return patch_jump(skip) && !failed_;
        }
        emit(Op::LdaLookup);
        emit_u16(add_name(name));
        size_t skip = emit_jump(skip_op);
        if (!compile_expression(expr->get_right())) return false;
        emit(Op::StaLookup);
        emit_u16(add_name(name));
        return patch_jump(skip) && !failed_;
    }

    if (expr->get_left()->get_type() != ASTNode::Type::MEMBER_EXPRESSION) return false;
    const auto* mem = static_cast<const MemberExpression*>(expr->get_left());
    const bool priv = member_is_private(mem);
    if (!priv && !member_is_supported(mem)) return emit_treewalker_delegate(expr);
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
    size_t skip = emit_jump(skip_op);
    if (!compile_expression(expr->get_right())) return false;
    emit(Op::SetKeyed);
    emit_u8(static_cast<uint8_t>(obj_reg));
    emit_u8(static_cast<uint8_t>(key_reg));
    if (!patch_jump(skip)) return false;
    free_temp(key_reg);
    free_temp(obj_reg);
    return !failed_;
}

size_t BytecodeCompiler::emit_jump(Op op) {
    emit(op);
    size_t pos = chunk_->code.size();
    emit_u16(0);
    return pos;
}

bool BytecodeCompiler::patch_jump(size_t operand_pos) {
    // Offset is relative to the pc after the 2-byte operand.
    ptrdiff_t offset = static_cast<ptrdiff_t>(chunk_->code.size()) -
                       static_cast<ptrdiff_t>(operand_pos + 2);
    if (offset < INT16_MIN || offset > INT16_MAX) { failed_ = true; return false; }
    uint16_t enc = static_cast<uint16_t>(static_cast<int16_t>(offset));
    chunk_->code[operand_pos] = static_cast<uint8_t>(enc & 0xFF);
    chunk_->code[operand_pos + 1] = static_cast<uint8_t>(enc >> 8);
    return true;
}

bool BytecodeCompiler::emit_jump_back(Op op, size_t target_pc) {
    emit(op);
    ptrdiff_t offset = static_cast<ptrdiff_t>(target_pc) -
                       static_cast<ptrdiff_t>(chunk_->code.size() + 2);
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
        case ASTNode::Type::FUNCTION_DECLARATION:
            return hoisted_fn_decls_.count(node) > 0;

        case ASTNode::Type::BLOCK_STATEMENT: {
            const auto* block = static_cast<const BlockStatement*>(node);
            // A block with its own direct let/const gets its own Environment
            // -- a nested block's own names are its own scope's concern.
            int block_env_idx = -1;
            {
                std::vector<BytecodeChunk::LoopEnvVar> vars;
                bool needs_own_env = false;
                if (!collect_direct_lexical_decls(block, vars, needs_own_env)) return false;
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
                    chunk_->loop_envs.push_back(std::move(env_vars));
                    block_env_idx = static_cast<int>(chunk_->loop_envs.size() - 1);
                    emit(Op::EnterLoopEnv);
                    emit_u16(static_cast<uint16_t>(block_env_idx));
                    env_depth_++;
                }
            }
            for (const auto& stmt : block->get_statements()) {
                if (!compile_statement(stmt.get())) return false;
            }
            if (block_env_idx >= 0) { emit(Op::ExitLoopEnv); env_depth_--; }
            return true;
        }

        case ASTNode::Type::EXPRESSION_STATEMENT: {
            const auto* stmt = static_cast<const ExpressionStatement*>(node);
            return compile_expression(stmt->get_expression());
        }

        case ASTNode::Type::VARIABLE_DECLARATION: {
            const auto* decl = static_cast<const VariableDeclaration*>(node);
            bool is_var = decl->get_kind() == VariableDeclarator::Kind::VAR;
            bool is_const = decl->get_kind() == VariableDeclarator::Kind::CONST;
            for (const auto& d : decl->get_declarations()) {
                const std::string& name = d->get_id()->get_name();

                if (name.empty() && d->get_init() &&
                    d->get_init()->get_type() == ASTNode::Type::DESTRUCTURING_ASSIGNMENT) {
                    // Delegates to DestructuringAssignment::evaluate_with_value via Op::DestructureBind.
                    if (!env_mode_) return false;
                    const auto* da = static_cast<const DestructuringAssignment*>(d->get_init());
                    if (!compile_expression(da->get_source())) return false;
                    if (chunk_->destructuring_patterns.size() >= 0xFFFF) return false;
                    chunk_->destructuring_patterns.push_back({da, !is_var, is_const});
                    emit(Op::DestructureBind);
                    emit_u16(static_cast<uint16_t>(chunk_->destructuring_patterns.size() - 1));
                    continue;
                }

                if (!is_local(name)) {
                    if (!script_mode_) return false;  // prescan declared everything
                    // Top-level script declaration: the binding pre-exists
                    // (vars on the global object, let/const uninitialized in
                    // the script env). NamedEvaluation stays on the
                    // tree-walker via a whole-declaration delegate.
                    if (d->get_init() && is_named_evaluation_rhs(d->get_init())) {
                        if (!emit_treewalker_delegate(node)) return false;
                        break;
                    }
                    bool is_lex = decl->get_kind() != VariableDeclarator::Kind::VAR;
                    if (!d->get_init()) {
                        if (is_var) continue;
                        emit(Op::LdaUndefined);
                    } else {
                        if (!compile_expression(d->get_init())) return false;
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
                    if (!compile_expression(d->get_init())) return false;
                }
                // `var` is function-scoped even inside a loop body -- a plain
                // chain-walked write, never StaEnvInit's per-iteration scope.
                emit_write_local(name, /*is_declaration=*/!is_var);
            }
            return true;
        }

        case ASTNode::Type::IF_STATEMENT: {
            const auto* stmt = static_cast<const IfStatement*>(node);
            if (!compile_expression(stmt->get_test())) return false;
            size_t else_jump = emit_jump(Op::JumpIfFalse);
            if (!compile_statement(stmt->get_consequent())) return false;
            if (stmt->has_alternate()) {
                size_t end_jump = emit_jump(Op::Jump);
                if (!patch_jump(else_jump)) return false;
                if (!compile_statement(stmt->get_alternate())) return false;
                if (!patch_jump(end_jump)) return false;
            } else {
                if (!patch_jump(else_jump)) return false;
            }
            return true;
        }

        case ASTNode::Type::WHILE_STATEMENT: {
            const auto* stmt = static_cast<const WhileStatement*>(node);
            int loop_env_idx = setup_loop_env({}, stmt->get_body());
            if (loop_env_idx >= 0) {
                emit(Op::EnterLoopEnv);
                emit_u16(static_cast<uint16_t>(loop_env_idx));
                env_depth_++;
            }
            size_t loop_start = chunk_->code.size();
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
                emit(Op::AdvanceLoopEnv);
                emit_u16(static_cast<uint16_t>(loop_env_idx));
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
            const auto* stmt = static_cast<const DoWhileStatement*>(node);
            int loop_env_idx = setup_loop_env({}, stmt->get_body());
            if (loop_env_idx >= 0) {
                emit(Op::EnterLoopEnv);
                emit_u16(static_cast<uint16_t>(loop_env_idx));
                env_depth_++;
            }
            size_t body_start = chunk_->code.size();

            loop_stack_.push_back({0, {}, {}, true, env_depth_, try_env_depth_, false, take_pending_labels()});
            if (!compile_statement(stmt->get_body())) return false;
            LoopScope scope = std::move(loop_stack_.back());
            loop_stack_.pop_back();

            for (size_t pos : scope.continue_patches) {
                if (!patch_jump(pos)) return false;  // continue lands on the advance step / test
            }
            if (loop_env_idx >= 0) {
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
            const auto* stmt = static_cast<const ForStatement*>(node);
            // Only a let/const header needs a per-iteration copy-forward binding.
            std::vector<BytecodeChunk::LoopEnvVar> header_vars;
            if (stmt->get_init() && stmt->get_init()->get_type() == ASTNode::Type::VARIABLE_DECLARATION) {
                const auto* vd = static_cast<const VariableDeclaration*>(stmt->get_init());
                if (vd->get_kind() != VariableDeclarator::Kind::VAR) {
                    bool is_const = vd->get_kind() == VariableDeclarator::Kind::CONST;
                    for (const auto& d : vd->get_declarations()) {
                        if (d->get_id()) header_vars.push_back({d->get_id()->get_name(), true, is_const, true});
                    }
                }
            }
            int loop_env_idx = setup_loop_env(std::move(header_vars), stmt->get_body());
            if (loop_env_idx >= 0) {
                emit(Op::EnterLoopEnv);
                emit_u16(static_cast<uint16_t>(loop_env_idx));
                env_depth_++;
            }
            if (stmt->get_init()) {
                if (stmt->get_init()->get_type() == ASTNode::Type::VARIABLE_DECLARATION) {
                    if (!compile_statement(stmt->get_init())) return false;
                } else {
                    if (!compile_expression(stmt->get_init())) return false;
                }
            }
            if (loop_env_idx >= 0) {
                // Spec 14.7.4.3 ForBodyEvaluation step 1: CreatePerIterationEnvironment
                // runs once more right after the init, before the first test --
                // a closure created during init (e.g. a second declarator's
                // initializer) must NOT alias the environment the first test/
                // body/update mutate.
                emit(Op::AdvanceLoopEnv);
                emit_u16(static_cast<uint16_t>(loop_env_idx));
            }
            size_t loop_start = chunk_->code.size();
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
            if (loop_env_idx >= 0) {
                emit(Op::AdvanceLoopEnv);
                emit_u16(static_cast<uint16_t>(loop_env_idx));
            }
            if (stmt->get_update()) {
                if (!compile_expression(stmt->get_update())) return false;
            }
            if (!emit_jump_back(Op::Jump, loop_start)) return false;
            if (has_test && !patch_jump(exit_jump)) return false;
            for (size_t pos : scope.break_patches) {
                if (!patch_jump(pos)) return false;
            }
            if (loop_env_idx >= 0) { emit(Op::ExitLoopEnv); env_depth_--; }
            return true;
        }

        case ASTNode::Type::FOR_OF_STATEMENT: {
            const auto* stmt = static_cast<const ForOfStatement*>(node);
            // for-await-of: fiber/async machinery, permanent fallback (same
            // reasoning as generators -- see vm-architecture.md 2.9).
            if (stmt->is_await()) return false;
            return compile_for_each_loop(stmt->get_left(), stmt->get_right(), stmt->get_body(), false,
                                          stmt->get_left_decl_kind());
        }

        case ASTNode::Type::FOR_IN_STATEMENT: {
            const auto* stmt = static_cast<const ForInStatement*>(node);
            return compile_for_each_loop(stmt->get_left(), stmt->get_right(), stmt->get_body(), true,
                                          stmt->get_left_decl_kind());
        }

        case ASTNode::Type::BREAK_STATEMENT: {
            const auto* stmt = static_cast<const BreakStatement*>(node);
            if (loop_stack_.empty()) return false;
            int target = static_cast<int>(loop_stack_.size()) - 1;
            if (!stmt->get_label().empty()) {
                target = -1;
                for (int i = static_cast<int>(loop_stack_.size()) - 1; i >= 0; i--) {
                    for (const auto& l : loop_stack_[i].labels) {
                        if (l == stmt->get_label()) { target = i; break; }
                    }
                    if (target >= 0) break;
                }
                if (target < 0) return false;
            }
            // IteratorClose every for-of/for-in strictly inside the target
            // (the target's own iterator is closed by its landing pad).
            for (int i = static_cast<int>(loop_stack_.size()) - 1; i > target; i--) {
                if (loop_stack_[i].iterator_reg < 0) continue;
                emit(Op::IteratorClose);
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
            scope.break_patches.push_back(emit_jump(Op::Jump));
            return true;
        }

        case ASTNode::Type::CONTINUE_STATEMENT: {
            const auto* stmt = static_cast<const ContinueStatement*>(node);
            if (loop_stack_.empty()) return false;
            int target = -1;
            if (stmt->get_label().empty()) {
                // A switch isn't a loop -- continue skips past it to the loop below.
                for (int i = static_cast<int>(loop_stack_.size()) - 1; i >= 0; i--) {
                    if (!loop_stack_[i].is_switch) { target = i; break; }
                }
            } else {
                for (int i = static_cast<int>(loop_stack_.size()) - 1; i >= 0 && target < 0; i--) {
                    for (const auto& l : loop_stack_[i].labels) {
                        if (l == stmt->get_label()) { target = i; break; }
                    }
                }
                if (target >= 0 && loop_stack_[target].is_switch) return false;  // not a loop
            }
            if (target < 0) return false;
            // IteratorClose every for-of/for-in strictly inside the target
            // (the target itself keeps iterating).
            for (int i = static_cast<int>(loop_stack_.size()) - 1; i > target; i--) {
                if (loop_stack_[i].iterator_reg < 0) continue;
                emit(Op::IteratorClose);
                emit_u8(static_cast<uint8_t>(loop_stack_[i].iterator_reg));
                emit_u8(0);
            }
            LoopScope& scope = loop_stack_[target];
            for (int i = env_depth_ - scope.base_env_depth; i > 0; i--) {
                emit(Op::ExitLoopEnv);
            }
            for (int i = try_env_depth_ - scope.base_try_depth; i > 0; i--) {
                emit(Op::PopEnvSave);
            }
            if (scope.continue_is_forward) {
                scope.continue_patches.push_back(emit_jump(Op::Jump));
            } else {
                if (!emit_jump_back(Op::Jump, scope.continue_target)) return false;
            }
            return true;
        }

        case ASTNode::Type::RETURN_STATEMENT: {
            const auto* stmt = static_cast<const ReturnStatement*>(node);
            if (stmt->get_argument()) {
                // ReturnStatement::evaluate carries semantics Op::Return lacks: an
                // async generator's `return <expr>` awaits the value, and its
                // set_return_value tells `return undefined` from falling off.
                if (suspendable_) {
                    if (!emit_treewalker_delegate(node)) return false;
                    emit(Op::Return);
                    return true;
                }
                if (!compile_expression(stmt->get_argument())) return false;
            } else {
                emit(Op::LdaUndefined);
            }
            // Return abruptly completes every enclosing for-of/for-in:
            // IteratorClose innermost-first (mode 0 leaves acc untouched).
            for (auto it = loop_stack_.rbegin(); it != loop_stack_.rend(); ++it) {
                if (it->iterator_reg < 0) continue;
                emit(Op::IteratorClose);
                emit_u8(static_cast<uint8_t>(it->iterator_reg));
                emit_u8(0);
            }
            emit(Op::Return);
            return true;
        }

        case ASTNode::Type::THROW_STATEMENT: {
            const auto* stmt = static_cast<const ThrowStatement*>(node);
            if (!compile_expression(stmt->get_expression())) return false;
            emit(Op::Throw);
            return true;
        }

        case ASTNode::Type::TRY_STATEMENT: {
            const auto* stmt = static_cast<const TryStatement*>(node);
            const ASTNode* catch_node = stmt->get_catch_clause();
            const ASTNode* finally_node = stmt->get_finally_block();
            if (!catch_node && !finally_node) return false;

            // A finally must run on every exit; return/break/continue aren't
            // implemented, so refuse rather than silently skip it.
            if (finally_node) {
                bool escapes = contains_control_escape(stmt->get_try_block()) ||
                    (catch_node && contains_control_escape(
                        static_cast<const CatchClause*>(catch_node)->get_body()));
                if (escapes) return false;
            }

            // A throw skips straight to the handler, bypassing any loop
            // Environment pop inside the try -- see VM::run's env_saves side-stack.
            bool save_env = env_mode_;
            if (save_env) {
                if (++try_env_depth_ > 64) return false;  // matches VM::run's fixed env_saves[64]
                emit(Op::SaveEnv);
            }

            size_t try_start = chunk_->code.size();
            if (!compile_statement(stmt->get_try_block())) return false;
            size_t try_end = chunk_->code.size();
            if (save_env) {
                emit(Op::PopEnvSave);  // no exception: already correctly restored
                try_env_depth_--;
            }
            size_t jump_try_ok = emit_jump(Op::Jump);

            // Patched with genreturn_pc below, once that pc is known.
            size_t handler_idx_try = SIZE_MAX, handler_idx_catch = SIZE_MAX;

            size_t catch_body_start = 0, catch_body_end = 0, jump_catch_ok = 0;
            if (catch_node) {
                const auto* clause = static_cast<const CatchClause*>(catch_node);
                size_t catch_pc = chunk_->code.size();
                if (save_env) emit(Op::RestoreEnv);
                // A catch-body throw still reaches finally's own RestoreEnv --
                // give it its own save slot or that underflows env_saves.
                bool save_env_for_catch_body = save_env && finally_node;
                if (save_env_for_catch_body) {
                    if (++try_env_depth_ > 64) return false;
                    emit(Op::SaveEnv);
                }
                int catch_env_idx = -1;
                if (!clause->get_parameter_name().empty()) {
                    if (!is_local(clause->get_parameter_name())) return false;
                    // Spec: a fresh Environment per catch, else it would
                    // overwrite an outer same-named binding instead of shadowing it.
                    if (env_mode_) {
                        std::vector<BytecodeChunk::LoopEnvVar> vars;
                        vars.push_back({clause->get_parameter_name(), false, false, false});
                        chunk_->loop_envs.push_back(std::move(vars));
                        catch_env_idx = static_cast<int>(chunk_->loop_envs.size() - 1);
                        emit(Op::EnterLoopEnv);
                        emit_u16(static_cast<uint16_t>(catch_env_idx));
                        env_depth_++;
                    }
                    emit_write_local(clause->get_parameter_name(), /*is_declaration=*/true);
                }
                // else: optional catch binding (`catch {}`) -- exception in
                // acc is simply discarded, nothing to store.
                catch_body_start = chunk_->code.size();
                if (!compile_statement(clause->get_body())) return false;
                catch_body_end = chunk_->code.size();
                if (catch_env_idx >= 0) { emit(Op::ExitLoopEnv); env_depth_--; }
                if (save_env_for_catch_body) {
                    emit(Op::PopEnvSave);
                    try_env_depth_--;
                }
                jump_catch_ok = emit_jump(Op::Jump);

                handler_idx_try = chunk_->handlers.size();
                chunk_->handlers.push_back({static_cast<uint32_t>(try_start),
                                             static_cast<uint32_t>(try_end),
                                             static_cast<uint32_t>(catch_pc)});
            }

            if (finally_node) {
                // Exception path: try (if no catch) or catch (if present)
                // raised past this point -- save it, run finally, re-raise.
                size_t reraise_pc = chunk_->code.size();
                if (save_env) emit(Op::RestoreEnv);
                int temp = alloc_temp();
                if (failed_) return false;
                emit(Op::Star);
                emit_u8(static_cast<uint8_t>(temp));
                if (!compile_statement(finally_node)) return false;
                emit(Op::Ldar);
                emit_u8(static_cast<uint8_t>(temp));
                free_temp(temp);
                emit(Op::Throw);

                if (catch_node) {
                    handler_idx_catch = chunk_->handlers.size();
                    chunk_->handlers.push_back({static_cast<uint32_t>(catch_body_start),
                                                 static_cast<uint32_t>(catch_body_end),
                                                 static_cast<uint32_t>(reraise_pc)});
                } else {
                    handler_idx_try = chunk_->handlers.size();
                    chunk_->handlers.push_back({static_cast<uint32_t>(try_start),
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
                    size_t genreturn_pc = chunk_->code.size();
                    if (save_env) emit(Op::RestoreEnv);
                    int gr_temp = alloc_temp();
                    if (failed_) return false;
                    emit(Op::Star);
                    emit_u8(static_cast<uint8_t>(gr_temp));
                    if (!compile_statement(finally_node)) return false;
                    emit(Op::Ldar);
                    emit_u8(static_cast<uint8_t>(gr_temp));
                    free_temp(gr_temp);
                    emit(Op::ReraiseGeneratorReturn);

                    if (try_needs_genreturn && handler_idx_try != SIZE_MAX) {
                        chunk_->handlers[handler_idx_try].genreturn_pc = static_cast<int32_t>(genreturn_pc);
                    }
                    if (catch_needs_genreturn && handler_idx_catch != SIZE_MAX) {
                        chunk_->handlers[handler_idx_catch].genreturn_pc = static_cast<int32_t>(genreturn_pc);
                    }
                }
            }

            // Normal-completion path: both "try succeeded" and "catch
            // succeeded" land here and share the same finally emission.
            if (!patch_jump(jump_try_ok)) return false;
            if (catch_node && !patch_jump(jump_catch_ok)) return false;
            if (finally_node) {
                if (!compile_statement(finally_node)) return false;
            }
            return true;
        }

        case ASTNode::Type::SWITCH_STATEMENT: {
            const auto* stmt = static_cast<const SwitchStatement*>(node);
            const auto& cases = stmt->get_cases();

            if (!compile_expression(stmt->get_discriminant())) return false;
            int disc_reg = alloc_temp();
            if (failed_) return false;
            emit(Op::Star);
            emit_u8(static_cast<uint8_t>(disc_reg));

            // Spec: every case shares ONE lexical scope, not one each --
            // entered after the discriminant, before any case test runs.
            int switch_env_idx = -1;
            {
                std::vector<BytecodeChunk::LoopEnvVar> vars;
                bool needs_own_env = false;
                for (const auto& c : cases) {
                    for (const auto& s : static_cast<const CaseClause*>(c.get())->get_consequent()) {
                        if (!collect_direct_lexical_decls(s.get(), vars, needs_own_env)) return false;
                    }
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
                    chunk_->loop_envs.push_back(std::move(env_vars));
                    switch_env_idx = static_cast<int>(chunk_->loop_envs.size() - 1);
                    emit(Op::EnterLoopEnv);
                    emit_u16(static_cast<uint16_t>(switch_env_idx));
                    env_depth_++;
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

            for (size_t i = 0; i < cases.size(); i++) {
                const auto* cc = static_cast<const CaseClause*>(cases[i].get());
                if (cc->is_default()) {
                    if (!patch_jump(jump_to_default_or_end)) return false;
                } else if (!patch_jump(test_jumps[i])) {
                    return false;
                }
                for (const auto& s : cc->get_consequent()) {
                    if (!compile_statement(s.get())) return false;
                }
            }
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
                    if (!compile_statement(stmt->get_statement())) return false;
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
            if (chunk_->closures.size() >= 0xFFFF) return false;
            chunk_->closures.push_back(node);
            emit(Op::CreateClosure);
            emit_u16(static_cast<uint16_t>(chunk_->closures.size() - 1));
            const Identifier* class_id = static_cast<const ClassDeclaration*>(node)->get_id();
            if (class_id && !class_id->get_name().empty() && !env_names_.count(class_id->get_name())) {
                emit_write_local(class_id->get_name(), /*is_declaration=*/true);
            }
            return true;
        }

        default:
            return false;
    }
}

bool BytecodeCompiler::compile_expression(const ASTNode* node) {
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
                emit(Op::LdaConst);
                emit_u16(add_constant(Value(v)));
            }
            return !failed_;
        }
        case ASTNode::Type::STRING_LITERAL: {
            emit(Op::LdaConst);
            emit_u16(add_constant(Value(static_cast<const StringLiteral*>(node)->get_value())));
            return !failed_;
        }
        case ASTNode::Type::TEMPLATE_LITERAL: {
            const auto& elements = static_cast<const TemplateLiteral*>(node)->get_elements();
            using Elem = TemplateLiteral::Element;
            int result_reg = alloc_temp();
            if (failed_) return false;
            emit(Op::LdaConst);
            emit_u16(add_constant(Value(std::string())));
            emit(Op::Star);
            emit_u8(static_cast<uint8_t>(result_reg));
            for (const auto& el : elements) {
                if (el.type == Elem::Type::TEXT) {
                    if (el.text.empty()) continue;
                    emit(Op::LdaConst);
                    emit_u16(add_constant(Value(el.text)));
                } else {
                    if (!compile_expression(el.expression.get())) return false;
                    emit(Op::ToTemplateString);
                }
                emit(Op::Add);
                emit_u8(static_cast<uint8_t>(result_reg));
                emit(Op::Star);
                emit_u8(static_cast<uint8_t>(result_reg));
            }
            emit(Op::Ldar);
            emit_u8(static_cast<uint8_t>(result_reg));
            free_temp(result_reg);
            return !failed_;
        }

        case ASTNode::Type::REGEX_LITERAL:
            // RegexLiteral::evaluate reads no bindings (built-in RegExp ctor
            // only), so this skips emit_treewalker_delegate's env_mode
            // requirement -- register-pure functions keep compiling.
            if (chunk_->closures.size() >= 0xFFFF) return false;
            chunk_->closures.push_back(node);
            emit(Op::CreateClosure);
            emit_u16(static_cast<uint16_t>(chunk_->closures.size() - 1));
            return !failed_;

        case ASTNode::Type::BOOLEAN_LITERAL:
            emit(static_cast<const BooleanLiteral*>(node)->get_value() ? Op::LdaTrue : Op::LdaFalse);
            return true;
        case ASTNode::Type::NULL_LITERAL:
            emit(Op::LdaNull);
            return true;
        case ASTNode::Type::UNDEFINED_LITERAL:
            emit(Op::LdaUndefined);
            return true;

        case ASTNode::Type::IDENTIFIER: {
            const std::string& name = static_cast<const Identifier*>(node)->get_name();
            if (name == "this") {
                // this_needs_super never enters the VM, so no TDZ check here.
                emit(Op::LdaThis);
                return true;
            }
            if (is_local(name)) {
                emit_read_local(name);
                return !failed_;
            }
            // Binding magic beyond a plain lookup stays on the tree-walker.
            // ("this" isn't here -- Function::call already resolves it;
            // "arguments" is a plain lookup too once needs_arguments made
            // Function::call materialize the binding.)
            if ((name == "arguments" && !allow_arguments_) || name == "eval" ||
                name == "super" || name == "new") {
                return false;
            }
            emit(Op::LdaLookup);
            emit_u16(add_name(name));
            return !failed_;
        }

        case ASTNode::Type::MEMBER_EXPRESSION: {
            const auto* mem = static_cast<const MemberExpression*>(node);
            const bool priv = member_is_private(mem);
            if (!priv && !member_is_supported(mem)) return emit_treewalker_delegate(node);
            if (!compile_expression(mem->get_object())) return false;
            int obj_reg = alloc_temp();
            if (failed_) return false;
            emit(Op::Star);
            emit_u8(static_cast<uint8_t>(obj_reg));

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
            }
            free_temp(obj_reg);
            return !failed_;
        }

        case ASTNode::Type::OPTIONAL_CHAINING_EXPRESSION: {
            const auto* mem = static_cast<const OptionalChainingExpression*>(node);
            if (mem->get_object()->get_type() == ASTNode::Type::IDENTIFIER &&
                static_cast<const Identifier*>(mem->get_object())->get_name() == "super") {
                return false;
            }
            if (!mem->is_computed()) {
                if (mem->get_property()->get_type() != ASTNode::Type::IDENTIFIER) return false;
                const std::string& name = static_cast<const Identifier*>(mem->get_property())->get_name();
                if (!name.empty() && name[0] == '#') return false;  // private field: tree-walker's job
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
                emit(Op::GetNamed);
                emit_u8(static_cast<uint8_t>(obj_reg));
                emit_u16(add_name(name));
                emit_u16(alloc_feedback_slot());
            } else {
                {
                    ChainMaskScope mask(chain_shortcircuit_jumps_);
                    if (!compile_expression(mem->get_property())) return false;
                }
                emit(Op::GetKeyed);
                emit_u8(static_cast<uint8_t>(obj_reg));
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
                // `#name in obj`: special-cased in BinaryExpression::evaluate, not apply_operator.
                return emit_treewalker_delegate(node);
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
                        } else if (is_local(name)) {
                            emit_read_local(name);
                        } else if ((name == "arguments" && !allow_arguments_) || name == "eval" ||
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
                    bool is_post = expr->get_operator() == UnOp::POST_INCREMENT ||
                                   expr->get_operator() == UnOp::POST_DECREMENT;

                    if (operand->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
                        const auto* mem = static_cast<const MemberExpression*>(operand);
                        const bool priv = member_is_private(mem);
                        if (!priv && !member_is_supported(mem)) return emit_treewalker_delegate(node);
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
                    if (name == "eval" || name == "arguments") return false;  // strict SyntaxError forms
                    if (!is_local(name)) {
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
                        // super/private forms (ReferenceError / brand ceremony)
                        // stay on the tree-walker.
                        if (!member_is_supported(mem)) return emit_treewalker_delegate(node);
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
                    // Identifier deletes (sloppy binding removal, strict
                    // SyntaxError) bail; any other operand is `true` without
                    // evaluation, same as the tree-walker.
                    if (operand->get_type() == ASTNode::Type::IDENTIFIER) return false;
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
                if (!is_local(name)) {
                    // Outer/global write via chain lookup.
                    if (name == "eval" || name == "arguments") return false;  // strict SyntaxError forms
                    if (!compound && is_named_evaluation_rhs(expr->get_right())) {
                        return emit_treewalker_delegate(node);
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
                        emit(Op::StaLookupChecked);
                        emit_u8(static_cast<uint8_t>(resolved));
                        emit_u16(add_name(name));
                        free_temp(resolved);
                        return !failed_;
                    }
                    // Spec order: the old value is read (and an unresolvable
                    // reference throws) BEFORE the rhs runs.
                    emit(Op::LdaLookup);
                    emit_u16(add_name(name));
                    int temp = alloc_temp();
                    if (failed_) return false;
                    emit(Op::Star);
                    emit_u8(static_cast<uint8_t>(temp));
                    if (!compile_expression(expr->get_right())) return false;
                    emit(vm_op);
                    emit_u8(static_cast<uint8_t>(temp));
                    free_temp(temp);
                    emit(Op::StaLookup);
                    emit_u16(add_name(name));
                    return !failed_;
                }

                if (!compound) {
                    if (!compile_expression(expr->get_right())) return false;
                    emit_write_local(name, /*is_declaration=*/false);
                    return !failed_;
                }
                // Spec order: the old lhs value is read BEFORE the rhs runs, so
                // `x += (x = 5)` sees the original x on the left.
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
            // plain `=` only. AssignmentExpression::evaluate() handles it.
            if (!compound && (expr->get_left()->get_type() == ASTNode::Type::ARRAY_LITERAL ||
                              expr->get_left()->get_type() == ASTNode::Type::OBJECT_LITERAL)) {
                return emit_treewalker_delegate(node);
            }

            if (expr->get_left()->get_type() != ASTNode::Type::MEMBER_EXPRESSION) return false;
            const auto* mem = static_cast<const MemberExpression*>(expr->get_left());
            const bool priv = member_is_private(mem);
            if (!priv && !member_is_supported(mem)) return emit_treewalker_delegate(node);

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
                free_temp(key_reg);
            }
            free_temp(obj_reg);
            return !failed_;
        }

        case ASTNode::Type::CALL_EXPRESSION: {
            const auto* call = static_cast<const CallExpression*>(node);
            if (call->is_tagged_template()) return false;
            // Optional forms need the chain wrapper's short-circuit collector;
            // without it (non-expression contexts) bail like before.
            if ((call->is_optional() || chain_contains_optional(call)) &&
                !chain_shortcircuit_jumps_) {
                return false;
            }
            const ASTNode* callee = call->get_callee();
            const auto& call_args = call->get_arguments();
            if (call_args.size() > 200) return false;
            // process_arguments_with_spread's iterator-protocol expansion has
            // no register-mode equivalent -- delegate whole.
            if (has_spread(call_args)) return emit_treewalker_delegate(node);

            // obj.method(...): the receiver must be `obj`, not undefined --
            // needs CallResolved, not a plain Call of the loaded function value.
            if (callee->get_type() == ASTNode::Type::MEMBER_EXPRESSION ||
                callee->get_type() == ASTNode::Type::OPTIONAL_CHAINING_EXPRESSION) {
                const ASTNode* mem_obj;
                const ASTNode* mem_prop;
                bool mem_computed;
                bool mem_optional = callee->get_type() == ASTNode::Type::OPTIONAL_CHAINING_EXPRESSION;
                bool mem_private = false;
                if (!mem_optional) {
                    const auto* mem = static_cast<const MemberExpression*>(callee);
                    mem_private = member_is_private(mem);
                    // super.method(...): delegate the whole call to the
                    // tree-walker instead of bailing the function.
                    if (!mem_private && !member_is_supported(mem)) return emit_treewalker_delegate(node);
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
                        if (!pname.empty() && pname[0] == '#') return emit_treewalker_delegate(node);
                    }
                    mem_obj = opt->get_object();
                    mem_prop = opt->get_property();
                    mem_computed = opt->is_computed();
                }

                if (!compile_expression(mem_obj)) return false;
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
                if (!mem_computed) {
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
                }
                int func_reg = alloc_temp();
                if (failed_) return false;
                emit(Op::Star);
                emit_u8(static_cast<uint8_t>(func_reg));
                if (call->is_optional()) {
                    // a.b?.(...): the resolved method is still in the accumulator.
                    chain_shortcircuit_jumps_->push_back(emit_jump(Op::JumpIfNullish));
                }

                int args_start = next_register_;
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

                emit(Op::CallResolved);
                emit_u8(static_cast<uint8_t>(func_reg));
                emit_u8(static_cast<uint8_t>(obj_reg));
                emit_u8(static_cast<uint8_t>(args_start));
                emit_u8(static_cast<uint8_t>(call_args.size()));
                emit_u16(add_name(method_name));
                free_temp(obj_reg);
                return !failed_;
            }

            // Plain-identifier callees only: direct eval never compiles.
            if (callee->get_type() != ASTNode::Type::IDENTIFIER) return false;
            const std::string& callee_name = static_cast<const Identifier*>(callee)->get_name();
            if (callee_name == "super") {
                // super(...) constructor call: delegate to the tree-walker, which
                // handles the derived-constructor `this`-binding ceremony.
                return emit_treewalker_delegate(node);
            }
            if (callee_name == "eval" || callee_name == "import") {
                return false;
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

            emit(Op::Call);
            emit_u8(static_cast<uint8_t>(callee_reg));
            emit_u8(static_cast<uint8_t>(args_start));
            emit_u8(static_cast<uint8_t>(call_args.size()));
            emit_u16(add_name(callee_name));
            free_temp(callee_reg);
            return !failed_;
        }

        case ASTNode::Type::NEW_EXPRESSION: {
            const auto* expr = static_cast<const NewExpression*>(node);
            const auto& new_args = expr->get_arguments();
            if (new_args.size() > 200) return false;
            if (has_spread(new_args)) return emit_treewalker_delegate(node);

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
        case ASTNode::Type::YIELD_EXPRESSION:
        case ASTNode::Type::AWAIT_EXPRESSION:
            return emit_treewalker_delegate(node);

        // Delegates to the tree-walker's own evaluate() (Op::CreateClosure) --
        // correct since env_mode guarantees every local this closure could
        // reference lives in ctx.get_lexical_environment(). CLASS_DECLARATION
        // here is the expression form (`const C = class {}`): evaluate()
        // returns the class without binding a name. Generator forms ride the
        // FUNCTION_EXPRESSION node; async fns/arrows have their own type.
        case ASTNode::Type::FUNCTION_EXPRESSION:
        case ASTNode::Type::ARROW_FUNCTION_EXPRESSION:
        case ASTNode::Type::ASYNC_FUNCTION_EXPRESSION:
        case ASTNode::Type::CLASS_DECLARATION: {
            if (!env_mode_) return false;
            if (chunk_->closures.size() >= 0xFFFF) return false;
            chunk_->closures.push_back(node);
            emit(Op::CreateClosure);
            emit_u16(static_cast<uint16_t>(chunk_->closures.size() - 1));
            return true;
        }

        case ASTNode::Type::OBJECT_LITERAL: {
            const auto* lit = static_cast<const ObjectLiteral*>(node);
            // Methods/accessors, computed keys, spread, __proto__:
            // ObjectLiteral::evaluate handles every form, and method closures
            // capture through the ctx chain delegation provides.
            if (object_literal_is_complex(lit)) return emit_treewalker_delegate(node);

            emit(Op::CreateObject);
            emit_u16(static_cast<uint16_t>(lit->get_properties().size()));
            int obj_reg = alloc_temp();
            if (failed_) return false;
            emit(Op::Star);
            emit_u8(static_cast<uint8_t>(obj_reg));

            for (const auto& prop : lit->get_properties()) {
                std::string key;
                auto kt = prop->key->get_type();
                if (kt == ASTNode::Type::IDENTIFIER) {
                    key = static_cast<const Identifier*>(prop->key.get())->get_name();
                } else if (kt == ASTNode::Type::STRING_LITERAL) {
                    key = static_cast<const StringLiteral*>(prop->key.get())->get_value();
                } else {
                    // Matches ObjectLiteral::evaluate's own NUMBER_LITERAL-key formatting exactly.
                    double n = static_cast<const NumberLiteral*>(prop->key.get())->get_value();
                    if (n == std::floor(n) && n >= LLONG_MIN && n <= LLONG_MAX) {
                        key = std::to_string(static_cast<long long>(n));
                    } else {
                        std::ostringstream oss;
                        oss << n;
                        key = oss.str();
                    }
                }
                // CreateDataProperty (a poisoned Object.prototype accessor
                // must not fire) -- DefineOwn.
                if (!compile_expression(prop->value.get())) return false;
                emit(Op::DefineOwn);
                emit_u8(static_cast<uint8_t>(obj_reg));
                emit_u16(add_name(key));
            }
            emit(Op::Ldar);
            emit_u8(static_cast<uint8_t>(obj_reg));
            free_temp(obj_reg);
            return !failed_;
        }

        case ASTNode::Type::ARRAY_LITERAL: {
            const auto* lit = static_cast<const ArrayLiteral*>(node);
            if (lit->get_elements().size() > 200) return false;
            for (const auto& el : lit->get_elements()) {
                if (!el) return false;
            }
            if (has_spread(lit->get_elements())) return emit_treewalker_delegate(node);

            emit(Op::CreateArray);
            emit_u16(static_cast<uint16_t>(lit->get_elements().size()));
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
                    emit(Op::LdaConst);
                    emit_u16(add_constant(Value(static_cast<double>(i))));
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
            emit(Op::Ldar);
            emit_u8(static_cast<uint8_t>(obj_reg));
            free_temp(obj_reg);
            return !failed_;
        }

        case ASTNode::Type::DESTRUCTURING_ASSIGNMENT:
            // Bare form: targets can be arbitrary AssignmentTargets, so
            // delegate whole rather than reuse DestructureBind.
            return emit_treewalker_delegate(node);

        default:
            return false;
    }
}

}
