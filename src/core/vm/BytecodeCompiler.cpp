/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/vm/BytecodeCompiler.h"
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

// Collects every declared name up front (var hoisting), including repeats --
// see contains_nested_lexical_decl for why duplicates are fine here.
bool prescan_declarations(const ASTNode* node, std::vector<DeclInfo>& out) {
    if (!node) return true;
    switch (node->get_type()) {
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
                out.push_back({name, vd->get_kind() != VariableDeclarator::Kind::VAR,
                                vd->get_kind() == VariableDeclarator::Kind::CONST});
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
                out.push_back({name, vd->get_kind() != VariableDeclarator::Kind::VAR,
                                vd->get_kind() == VariableDeclarator::Kind::CONST});
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
        default:
            return false;
    }
}

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

// True if any try statement in the body contains a yield/await. Such bodies
// can't compile: generator return()/throw() unwinds as a C++ exception from
// the suspended point and would skip the VM's finally handling.
bool contains_try_with_suspend(const ASTNode* node) {
    if (!node) return false;
    if (node->get_type() == ASTNode::Type::TRY_STATEMENT) {
        if (contains_suspend(node)) return true;
    }
    switch (node->get_type()) {
        case ASTNode::Type::FUNCTION_EXPRESSION:
        case ASTNode::Type::FUNCTION_DECLARATION:
        case ASTNode::Type::ASYNC_FUNCTION_EXPRESSION:
        case ASTNode::Type::CLASS_DECLARATION:
            return false;
        case ASTNode::Type::BLOCK_STATEMENT: {
            const auto* n = static_cast<const BlockStatement*>(node);
            for (const auto& s : n->get_statements()) {
                if (contains_try_with_suspend(s.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::IF_STATEMENT: {
            const auto* n = static_cast<const IfStatement*>(node);
            return contains_try_with_suspend(n->get_consequent()) ||
                   contains_try_with_suspend(n->get_alternate());
        }
        case ASTNode::Type::WHILE_STATEMENT:
            return contains_try_with_suspend(static_cast<const WhileStatement*>(node)->get_body());
        case ASTNode::Type::DO_WHILE_STATEMENT:
            return contains_try_with_suspend(static_cast<const DoWhileStatement*>(node)->get_body());
        case ASTNode::Type::FOR_STATEMENT:
            return contains_try_with_suspend(static_cast<const ForStatement*>(node)->get_body());
        case ASTNode::Type::FOR_OF_STATEMENT:
            return contains_try_with_suspend(static_cast<const ForOfStatement*>(node)->get_body());
        case ASTNode::Type::FOR_IN_STATEMENT:
            return contains_try_with_suspend(static_cast<const ForInStatement*>(node)->get_body());
        case ASTNode::Type::TRY_STATEMENT: {
            const auto* n = static_cast<const TryStatement*>(node);
            if (contains_try_with_suspend(n->get_try_block())) return true;
            if (const ASTNode* cc = n->get_catch_clause()) {
                if (contains_try_with_suspend(static_cast<const CatchClause*>(cc)->get_body())) return true;
            }
            return contains_try_with_suspend(n->get_finally_block());
        }
        case ASTNode::Type::SWITCH_STATEMENT: {
            const auto* n = static_cast<const SwitchStatement*>(node);
            for (const auto& c : n->get_cases()) {
                const auto* cc = static_cast<const CaseClause*>(c.get());
                for (const auto& s : cc->get_consequent()) {
                    if (contains_try_with_suspend(s.get())) return true;
                }
            }
            return false;
        }
        case ASTNode::Type::LABELED_STATEMENT:
            return contains_try_with_suspend(static_cast<const LabeledStatement*>(node)->get_statement());
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
        case ASTNode::Type::FOR_OF_STATEMENT:
            return contains_destructuring(static_cast<const ForOfStatement*>(node)->get_body());
        case ASTNode::Type::FOR_IN_STATEMENT:
            return contains_destructuring(static_cast<const ForInStatement*>(node)->get_body());
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

// True if a return anywhere in `node` could escape it (for-of needs
// IteratorClose on early exit, not implemented for return -- break is fine).
bool contains_return_statement(const ASTNode* node) {
    if (!node) return false;
    switch (node->get_type()) {
        case ASTNode::Type::RETURN_STATEMENT:
            return true;
        case ASTNode::Type::BLOCK_STATEMENT: {
            const auto* n = static_cast<const BlockStatement*>(node);
            for (const auto& stmt : n->get_statements()) {
                if (contains_return_statement(stmt.get())) return true;
            }
            return false;
        }
        case ASTNode::Type::IF_STATEMENT: {
            const auto* n = static_cast<const IfStatement*>(node);
            return contains_return_statement(n->get_consequent()) ||
                   contains_return_statement(n->get_alternate());
        }
        case ASTNode::Type::WHILE_STATEMENT:
            return contains_return_statement(static_cast<const WhileStatement*>(node)->get_body());
        case ASTNode::Type::DO_WHILE_STATEMENT:
            return contains_return_statement(static_cast<const DoWhileStatement*>(node)->get_body());
        case ASTNode::Type::FOR_STATEMENT:
            return contains_return_statement(static_cast<const ForStatement*>(node)->get_body());
        case ASTNode::Type::FOR_OF_STATEMENT:
            return contains_return_statement(static_cast<const ForOfStatement*>(node)->get_body());
        case ASTNode::Type::FOR_IN_STATEMENT:
            return contains_return_statement(static_cast<const ForInStatement*>(node)->get_body());
        case ASTNode::Type::TRY_STATEMENT: {
            const auto* n = static_cast<const TryStatement*>(node);
            if (contains_return_statement(n->get_try_block())) return true;
            if (const ASTNode* cc = n->get_catch_clause()) {
                if (contains_return_statement(static_cast<const CatchClause*>(cc)->get_body())) return true;
            }
            return contains_return_statement(n->get_finally_block());
        }
        case ASTNode::Type::SWITCH_STATEMENT: {
            const auto* n = static_cast<const SwitchStatement*>(node);
            for (const auto& c : n->get_cases()) {
                for (const auto& s : static_cast<const CaseClause*>(c.get())->get_consequent()) {
                    if (contains_return_statement(s.get())) return true;
                }
            }
            return false;
        }
        case ASTNode::Type::LABELED_STATEMENT:
            return contains_return_statement(static_cast<const LabeledStatement*>(node)->get_statement());
        default:
            return false;
    }
}

// True if `node`'s object/callee spine contains an optional-chaining link
// (a?.b, a?.b.c(), ...).
bool chain_contains_optional(const ASTNode* node) {
    if (!node) return false;
    switch (node->get_type()) {
        case ASTNode::Type::OPTIONAL_CHAINING_EXPRESSION:
            return true;
        case ASTNode::Type::MEMBER_EXPRESSION:
            return chain_contains_optional(static_cast<const MemberExpression*>(node)->get_object());
        case ASTNode::Type::CALL_EXPRESSION:
            return chain_contains_optional(static_cast<const CallExpression*>(node)->get_callee());
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
    if (suspendable && contains_try_with_suspend(body)) return nullptr;

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
    // which register-mode parameters don't have.
    bool needs_arguments = uses_arguments(body);

    // Shadowing is covered too: a true duplicate name always has a nested
    // occurrence (a top-level dup is already a parser SyntaxError).
    // super/private-name access also forces env_mode: those forms delegate
    // to the tree-walker's own evaluate() and need `this`/`__super__`/brand
    // bindings and any captured locals resolvable through a real Environment.
    // Suspendable bodies always use env_mode: locals must survive across the
    // fiber suspension that delegated yield/await expressions perform.
    bool env_mode = suspendable || has_complex_params || needs_arguments ||
                     contains_closure(body) || contains_destructuring(body) ||
                     contains_nested_lexical_decl(static_cast<const BlockStatement*>(body)) ||
                     uses_super_or_private(body);

    BytecodeCompiler compiler(param_names, env_mode);
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

        if (env_mode) {
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

    if (!env_mode) {
        // Every let/const register starts in TDZ; the declaring statement's
        // own Star lifts it later.
        for (const auto& info : declared) {
            if (!info.is_lexical) continue;
            compiler.emit(Op::LdaTdz);
            compiler.emit(Op::Star);
            compiler.emit_u8(static_cast<uint8_t>(compiler.lookup_local(info.name)));
        }
    }
    // Non-rest parameters' function-entry bindings are data-driven from the
    // chunk (env_params), set up once by VM::run -- no bytecode needed for
    // those. Rest gets its own immediately-initialized slot here instead,
    // since CreateRestArray/DestructureBind below fills it, not run().
    if (has_rest) {
        compiler.chunk_->env_locals.push_back({rest_name, false, false});
    }

    // Defaults and destructuring resolve once at entry, before the body --
    // by now every parameter's raw argument value is already bound (plain
    // parameters need nothing further).
    for (const auto& p : params) {
        if (p->is_rest()) {
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
            continue;
        }
        if (!p->has_default() && !p->has_destructuring()) continue;
        const std::string& pname = p->get_name()->get_name();
        compiler.emit_read_local(pname);
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
        } else {
            compiler.emit_write_local(pname, false);
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
    compiler.chunk_->needs_arguments = needs_arguments;
    if (env_mode) compiler.chunk_->env_params = param_names;
    return std::move(compiler.chunk_);
}

BytecodeCompiler::BytecodeCompiler(const std::vector<std::string>& param_names, bool env_mode)
    : chunk_(std::make_unique<BytecodeChunk>()), env_mode_(env_mode) {
    if (env_mode_) {
        for (const auto& p : param_names) {
            if (!env_names_.insert(p).second) { failed_ = true; return; }
        }
        return;
    }
    for (const auto& p : param_names) {
        if (!declare_local(p)) { failed_ = true; return; }
    }
}

int BytecodeCompiler::setup_loop_env(std::vector<BytecodeChunk::LoopEnvVar> extra_vars, const ASTNode* body) {
    if (!env_mode_) return -1;
    std::vector<BytecodeChunk::LoopEnvVar> vars = std::move(extra_vars);
    bool needs_own_env = false;
    if (!collect_direct_lexical_decls(body, vars, needs_own_env)) return -1;
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
                                              const ASTNode* body, bool is_for_in) {
    // Only a simple identifier target (declared here, or a bare identifier).
    std::string var_name;
    bool declare_fresh = false;
    bool is_const = false;
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
    } else {
        return false;  // destructuring / member-expression LHS
    }
    if (!is_local(var_name)) return false;

    // A `return` inside the body would need IteratorClose on the way out too
    // (not implemented) -- break IS supported, via Op::IteratorClose.
    if (contains_return_statement(body)) return false;

    // Entered before compiling `right`: a lexical ForDeclaration's bound name
    // is in TDZ even during the head's own iterable/object expression (spec).
    std::vector<BytecodeChunk::LoopEnvVar> extra_vars;
    if (declare_fresh && env_mode_) {
        extra_vars.push_back({var_name, true, is_const, false});
    }
    int loop_env_idx = setup_loop_env(std::move(extra_vars), body);
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

    size_t loop_start = chunk_->code.size();
    emit(Op::IteratorNextOrJump);
    emit_u8(static_cast<uint8_t>(iterator_reg));
    emit_u8(static_cast<uint8_t>(next_fn_reg));
    size_t next_jump = chunk_->code.size();
    emit_u16(0);  // patched below to pre_exit (done, or the iterator threw)

    emit_write_local(var_name, /*is_declaration=*/declare_fresh);

    size_t body_start = chunk_->code.size();
    loop_stack_.push_back({0, {}, {}, true, env_depth_, try_env_depth_, false, take_pending_labels()});
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
    return env_mode_ ? env_names_.count(name) > 0 : locals_.count(name) > 0;
}

int BytecodeCompiler::lookup_local(const std::string& name) const {
    auto it = locals_.find(name);
    return it != locals_.end() ? it->second : -1;
}

bool BytecodeCompiler::declare_local(const std::string& name) {
    if (env_mode_) return env_names_.insert(name).second;
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
    if (env_mode_) {
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
    if (env_mode_) {
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
    if (chunk_->closures.size() >= 0xFFFF) return false;
    chunk_->closures.push_back(node);
    emit(Op::CreateClosure);
    emit_u16(static_cast<uint16_t>(chunk_->closures.size() - 1));
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
            if (env_mode_) {
                std::vector<BytecodeChunk::LoopEnvVar> vars;
                bool needs_own_env = false;
                if (!collect_direct_lexical_decls(block, vars, needs_own_env)) return false;
                if (!vars.empty() || needs_own_env) {
                    chunk_->loop_envs.push_back(std::move(vars));
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

                if (!is_local(name)) return false;  // prescan declared everything
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
            return compile_for_each_loop(stmt->get_left(), stmt->get_right(), stmt->get_body(), false);
        }

        case ASTNode::Type::FOR_IN_STATEMENT: {
            const auto* stmt = static_cast<const ForInStatement*>(node);
            return compile_for_each_loop(stmt->get_left(), stmt->get_right(), stmt->get_body(), true);
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
                    chunk_->handlers.push_back({static_cast<uint32_t>(catch_body_start),
                                                 static_cast<uint32_t>(catch_body_end),
                                                 static_cast<uint32_t>(reraise_pc)});
                } else {
                    chunk_->handlers.push_back({static_cast<uint32_t>(try_start),
                                                 static_cast<uint32_t>(try_end),
                                                 static_cast<uint32_t>(reraise_pc)});
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
            if (env_mode_) {
                std::vector<BytecodeChunk::LoopEnvVar> vars;
                bool needs_own_env = false;
                for (const auto& c : cases) {
                    for (const auto& s : static_cast<const CaseClause*>(c.get())->get_consequent()) {
                        if (!collect_direct_lexical_decls(s.get(), vars, needs_own_env)) return false;
                    }
                }
                if (!vars.empty() || needs_own_env) {
                    chunk_->loop_envs.push_back(std::move(vars));
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
            // Same delegation as closures: ClassDeclaration::evaluate builds
            // the whole class AND binds its name in the current environment.
            if (!env_mode_) return false;
            if (chunk_->closures.size() >= 0xFFFF) return false;
            chunk_->closures.push_back(node);
            emit(Op::CreateClosure);
            emit_u16(static_cast<uint16_t>(chunk_->closures.size() - 1));
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
         node->get_type() == ASTNode::Type::OPTIONAL_CHAINING_EXPRESSION) &&
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
            if (!member_is_supported(mem)) return emit_treewalker_delegate(node);
            if (!compile_expression(mem->get_object())) return false;
            int obj_reg = alloc_temp();
            if (failed_) return false;
            emit(Op::Star);
            emit_u8(static_cast<uint8_t>(obj_reg));

            if (!mem->is_computed()) {
                const std::string& name = static_cast<const Identifier*>(mem->get_property())->get_name();
                emit(Op::GetNamed);
                emit_u8(static_cast<uint8_t>(obj_reg));
                emit_u16(add_name(name));
                emit_u16(alloc_feedback_slot());
            } else {
                // Evaluating the key leaves it in the accumulator, exactly
                // what GetKeyed expects -- no extra register needed for reads.
                if (!compile_expression(mem->get_property())) return false;
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
                if (!compile_expression(mem->get_property())) return false;
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
                        if (is_local(name)) {
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
                    if (operand->get_type() != ASTNode::Type::IDENTIFIER) return false;
                    const std::string& name = static_cast<const Identifier*>(operand)->get_name();
                    if (!is_local(name)) return false;
                    bool is_inc = expr->get_operator() == UnOp::PRE_INCREMENT ||
                                  expr->get_operator() == UnOp::POST_INCREMENT;
                    bool is_post = expr->get_operator() == UnOp::POST_INCREMENT ||
                                   expr->get_operator() == UnOp::POST_DECREMENT;
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
                default:
                    return false;  // delete: V2+
            }
        }

        case ASTNode::Type::ASSIGNMENT_EXPRESSION: {
            const auto* expr = static_cast<const AssignmentExpression*>(node);
            using AsOp = AssignmentExpression::Operator;

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
                    return false;  // &&= / ||= / ??=: V1+
            }

            if (expr->get_left()->get_type() == ASTNode::Type::IDENTIFIER) {
                const std::string& name = static_cast<const Identifier*>(expr->get_left())->get_name();
                if (!is_local(name)) return false;

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

            if (expr->get_left()->get_type() != ASTNode::Type::MEMBER_EXPRESSION) return false;
            const auto* mem = static_cast<const MemberExpression*>(expr->get_left());
            if (!member_is_supported(mem)) return emit_treewalker_delegate(node);

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
                    emit(Op::GetNamed);
                    emit_u8(static_cast<uint8_t>(obj_reg));
                    emit_u16(name_idx);
                    emit_u16(alloc_feedback_slot());
                } else {
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
                if (!compile_expression(expr->get_right())) return false;
            }

            if (!mem->is_computed()) {
                emit(Op::SetNamed);
                emit_u8(static_cast<uint8_t>(obj_reg));
                emit_u16(name_idx);
                emit_u16(alloc_feedback_slot());
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
            if (call->is_optional() || call->is_tagged_template()) return false;
            const ASTNode* callee = call->get_callee();
            const auto& call_args = call->get_arguments();
            if (call_args.size() > 200) return false;
            for (const auto& arg : call_args) {
                if (arg->get_type() == ASTNode::Type::SPREAD_ELEMENT) return false;
            }

            // obj.method(...): the receiver must be `obj`, not undefined --
            // needs CallResolved, not a plain Call of the loaded function value.
            if (callee->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
                const auto* mem = static_cast<const MemberExpression*>(callee);
                if (mem->is_computed()) return false;
                // super.method(...) / obj.#method(...) / this.#method(...): delegate
                // the whole call to the tree-walker instead of bailing the function.
                if (!member_is_supported(mem)) return emit_treewalker_delegate(node);
                if (chain_contains_optional(mem)) return false;  // `a?.b.c()`: call short-circuiting not implemented
                const std::string& method_name =
                    static_cast<const Identifier*>(mem->get_property())->get_name();

                if (!compile_expression(mem->get_object())) return false;
                int obj_reg = alloc_temp();
                if (failed_) return false;
                emit(Op::Star);
                emit_u8(static_cast<uint8_t>(obj_reg));

                // Resolve the method before compiling arguments (spec order):
                // this throws on a null/undefined receiver, args must not run first.
                emit(Op::GetNamed);
                emit_u8(static_cast<uint8_t>(obj_reg));
                emit_u16(add_name(method_name));
                emit_u16(alloc_feedback_slot());
                int func_reg = alloc_temp();
                if (failed_) return false;
                emit(Op::Star);
                emit_u8(static_cast<uint8_t>(func_reg));

                int args_start = next_register_;
                for (const auto& arg : call_args) {
                    int arg_reg = alloc_temp();
                    if (failed_) return false;
                    if (!compile_expression(arg.get())) return false;
                    emit(Op::Star);
                    emit_u8(static_cast<uint8_t>(arg_reg));
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

            // Arguments occupy consecutive temps: each argument expression
            // balances its own temps, so alloc_temp stays contiguous here.
            int args_start = next_register_;
            for (const auto& arg : call_args) {
                int arg_reg = alloc_temp();
                if (failed_) return false;
                if (!compile_expression(arg.get())) return false;
                emit(Op::Star);
                emit_u8(static_cast<uint8_t>(arg_reg));
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
            for (const auto& arg : new_args) {
                if (arg->get_type() == ASTNode::Type::SPREAD_ELEMENT) return false;
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
            using PropType = ObjectLiteral::PropertyType;
            // Plain data properties with a static string/number/identifier key only.
            for (const auto& prop : lit->get_properties()) {
                if (!prop->key) return false;  // spread property: null key
                if (prop->type != PropType::Value || prop->computed) return false;
                auto kt = prop->key->get_type();
                if (kt != ASTNode::Type::IDENTIFIER && kt != ASTNode::Type::STRING_LITERAL &&
                    kt != ASTNode::Type::NUMBER_LITERAL) {
                    return false;
                }
            }
            if (lit->get_properties().size() > 200) return false;

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
                if (!compile_expression(prop->value.get())) return false;
                emit(Op::SetNamed);
                emit_u8(static_cast<uint8_t>(obj_reg));
                emit_u16(add_name(key));
                emit_u16(alloc_feedback_slot());
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
                // Holes are represented as a null element pointer; spread
                // needs iterator protocol -- both stay on the tree-walker.
                if (!el || el->get_type() == ASTNode::Type::SPREAD_ELEMENT) return false;
            }

            emit(Op::CreateArray);
            emit_u16(static_cast<uint16_t>(lit->get_elements().size()));
            int obj_reg = alloc_temp();
            if (failed_) return false;
            emit(Op::Star);
            emit_u8(static_cast<uint8_t>(obj_reg));

            const auto& elements = lit->get_elements();
            for (size_t i = 0; i < elements.size(); i++) {
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
                emit(Op::SetKeyed);
                emit_u8(static_cast<uint8_t>(obj_reg));
                emit_u8(static_cast<uint8_t>(key_reg));
                free_temp(key_reg);
            }
            emit(Op::Ldar);
            emit_u8(static_cast<uint8_t>(obj_reg));
            free_temp(obj_reg);
            return !failed_;
        }

        default:
            return false;
    }
}

}
