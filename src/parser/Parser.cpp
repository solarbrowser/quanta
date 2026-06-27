/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/parser/Parser.h"
#include "quanta/core/runtime/RegExp.h"
#include <algorithm>
#include <functional>
#include <iostream>
#include <map>
#include <unordered_set>

namespace Quanta {


Parser::Parser(TokenSequence tokens)
    : tokens_(std::move(tokens)), current_token_index_(0) {
    options_.allow_return_outside_function = false;
    options_.allow_await_outside_async = false;
    options_.strict_mode = false;
    options_.source_type_module = false;
    
    while (current_token_index_ < tokens_.size() && 
           (current_token().get_type() == TokenType::NEWLINE || 
            current_token().get_type() == TokenType::WHITESPACE ||
            current_token().get_type() == TokenType::COMMENT)) {
        if (current_token_index_ < tokens_.size() - 1) {
            current_token_index_++;
        } else {
            break;
        }
    }
}

Parser::Parser(TokenSequence tokens, const ParseOptions& options)
    : tokens_(std::move(tokens)), options_(options), current_token_index_(0) {
    while (current_token_index_ < tokens_.size() && 
           (current_token().get_type() == TokenType::NEWLINE || 
            current_token().get_type() == TokenType::WHITESPACE ||
            current_token().get_type() == TokenType::COMMENT)) {
        if (current_token_index_ < tokens_.size() - 1) {
            current_token_index_++;
        } else {
            break;
        }
    }
}

std::unique_ptr<Program> Parser::parse_program() {
    std::vector<std::unique_ptr<ASTNode>> statements;
    Position start = get_current_position();
    
    check_for_use_strict_directive();
    
    while (!at_end()) {
        try {
            auto statement = parse_statement();
            if (statement) {
                statements.push_back(std::move(statement));
            } else {
                skip_to_statement_boundary();
            }
        } catch (const std::exception& e) {
            add_error("Parse error: " + std::string(e.what()));
            skip_to_statement_boundary();
        }
    }
    
    // Always check class/let/const duplicates; also check function decl dups in strict/module.
    {
        std::vector<std::string> lex_only;   // class + let/const -- always checked
        std::vector<std::string> fn_strict;  // function decls -- checked in strict/module
        std::vector<std::string> var_names;  // var declarations -- checked for lex conflict in module

        for (const auto& stmt : statements) {
            if (!stmt) continue;
            if (stmt->get_type() == ASTNode::Type::FUNCTION_DECLARATION) {
                auto* fn = static_cast<FunctionDeclaration*>(stmt.get());
                if (fn->get_id()) fn_strict.push_back(fn->get_id()->get_name());
            } else if (stmt->get_type() == ASTNode::Type::CLASS_DECLARATION) {
                auto* cls = static_cast<ClassDeclaration*>(stmt.get());
                if (cls->get_id()) lex_only.push_back(cls->get_id()->get_name());
            } else if (stmt->get_type() == ASTNode::Type::VARIABLE_DECLARATION) {
                auto* vd = static_cast<VariableDeclaration*>(stmt.get());
                if (vd->get_kind() != VariableDeclarator::Kind::VAR) {
                    for (const auto& d : vd->get_declarations())
                        if (d->get_id()) lex_only.push_back(d->get_id()->get_name());
                } else {
                    for (const auto& d : vd->get_declarations())
                        if (d->get_id()) var_names.push_back(d->get_id()->get_name());
                }
            } else if (stmt->get_type() == ASTNode::Type::EXPORT_STATEMENT && options_.source_type_module) {
                auto* exp = static_cast<ExportStatement*>(stmt.get());
                ASTNode* decl = exp->is_declaration_export() ? exp->get_declaration() :
                                exp->is_default_export() ? exp->get_default_export() : nullptr;
                if (decl) {
                    if (decl->get_type() == ASTNode::Type::FUNCTION_DECLARATION) {
                        auto* fn = static_cast<FunctionDeclaration*>(decl);
                        if (fn->get_id()) fn_strict.push_back(fn->get_id()->get_name());
                    } else if (decl->get_type() == ASTNode::Type::FUNCTION_EXPRESSION) {
                        auto* fn = static_cast<FunctionExpression*>(decl);
                        if (fn->get_id()) fn_strict.push_back(fn->get_id()->get_name());
                    } else if (decl->get_type() == ASTNode::Type::ASYNC_FUNCTION_EXPRESSION) {
                        auto* fn = static_cast<AsyncFunctionExpression*>(decl);
                        if (fn->get_id()) fn_strict.push_back(fn->get_id()->get_name());
                    } else if (decl->get_type() == ASTNode::Type::CLASS_DECLARATION) {
                        auto* cls = static_cast<ClassDeclaration*>(decl);
                        if (cls->get_id()) lex_only.push_back(cls->get_id()->get_name());
                    } else if (decl->get_type() == ASTNode::Type::VARIABLE_DECLARATION) {
                        auto* vd = static_cast<VariableDeclaration*>(decl);
                        if (vd->get_kind() != VariableDeclarator::Kind::VAR) {
                            for (const auto& d : vd->get_declarations())
                                if (d->get_id()) lex_only.push_back(d->get_id()->get_name());
                        } else {
                            for (const auto& d : vd->get_declarations())
                                if (d->get_id()) var_names.push_back(d->get_id()->get_name());
                        }
                    }
                }
            }
        }

        // Check class/let/const duplicates unconditionally
        for (size_t i = 0; i < lex_only.size(); i++)
            for (size_t j = i+1; j < lex_only.size(); j++)
                if (lex_only[i] == lex_only[j]) {
                    add_error("SyntaxError: Identifier '" + lex_only[i] + "' has already been declared");
                    goto dup_done;
                }

        // Also check for lex vs function-decl name collisions (strict/module)
        if (options_.source_type_module || options_.strict_mode) {
            std::vector<std::string> all_lex = lex_only;
            all_lex.insert(all_lex.end(), fn_strict.begin(), fn_strict.end());
            for (size_t i = 0; i < all_lex.size(); i++)
                for (size_t j = i+1; j < all_lex.size(); j++)
                    if (all_lex[i] == all_lex[j]) {
                        add_error("SyntaxError: Identifier '" + all_lex[i] + "' has already been declared");
                        goto dup_done;
                    }
            // Module: var declarations must not conflict with lexical declarations
            if (options_.source_type_module) {
                for (const auto& vn : var_names)
                    for (const auto& ln : all_lex)
                        if (vn == ln) {
                            add_error("SyntaxError: Identifier '" + vn + "' has already been declared");
                            goto dup_done;
                        }
            }
        }
        dup_done:;
    }

    // Module: check for duplicate exported names (early error)
    if (options_.source_type_module) {
        std::vector<std::string> exported_names;
        for (const auto& stmt : statements) {
            if (!stmt || stmt->get_type() != ASTNode::Type::EXPORT_STATEMENT) continue;
            auto* exp = static_cast<ExportStatement*>(stmt.get());
            if (exp->is_default_export()) {
                exported_names.push_back("default");
            } else if (exp->is_declaration_export()) {
                ASTNode* decl = exp->get_declaration();
                if (decl) {
                    if (decl->get_type() == ASTNode::Type::FUNCTION_DECLARATION) {
                        auto* fn = static_cast<FunctionDeclaration*>(decl);
                        if (fn->get_id()) exported_names.push_back(fn->get_id()->get_name());
                    } else if (decl->get_type() == ASTNode::Type::CLASS_DECLARATION) {
                        auto* cls = static_cast<ClassDeclaration*>(decl);
                        if (cls->get_id()) exported_names.push_back(cls->get_id()->get_name());
                    } else if (decl->get_type() == ASTNode::Type::VARIABLE_DECLARATION) {
                        auto* vd = static_cast<VariableDeclaration*>(decl);
                        for (const auto& d : vd->get_declarations())
                            if (d->get_id()) exported_names.push_back(d->get_id()->get_name());
                    }
                }
            } else {
                for (const auto& spec : exp->get_specifiers()) {
                    const std::string& en = spec->get_exported_name();
                    if (en != "*") exported_names.push_back(en); // skip bare re-export *
                }
            }
        }
        for (size_t i = 0; i < exported_names.size(); i++)
            for (size_t j = i + 1; j < exported_names.size(); j++)
                if (exported_names[i] == exported_names[j]) {
                    add_error("SyntaxError: Duplicate export of '" + exported_names[i] + "'");
                    goto export_dup_done;
                }
        export_dup_done:;

        // Module early error: export { x } without 'from' -- x must be a locally declared binding
        {
            std::unordered_set<std::string> local_bindings;
            for (const auto& stmt : statements) {
                if (!stmt) continue;
                auto t = stmt->get_type();
                if (t == ASTNode::Type::FUNCTION_DECLARATION) {
                    auto* fn = static_cast<FunctionDeclaration*>(stmt.get());
                    if (fn->get_id()) local_bindings.insert(fn->get_id()->get_name());
                } else if (t == ASTNode::Type::CLASS_DECLARATION) {
                    auto* cls = static_cast<ClassDeclaration*>(stmt.get());
                    if (cls->get_id()) local_bindings.insert(cls->get_id()->get_name());
                } else if (t == ASTNode::Type::VARIABLE_DECLARATION) {
                    auto* vd = static_cast<VariableDeclaration*>(stmt.get());
                    for (const auto& d : vd->get_declarations())
                        if (d->get_id()) local_bindings.insert(d->get_id()->get_name());
                } else if (t == ASTNode::Type::IMPORT_STATEMENT) {
                    auto* imp = static_cast<ImportStatement*>(stmt.get());
                    if (!imp->get_default_alias().empty()) local_bindings.insert(imp->get_default_alias());
                    if (!imp->get_namespace_alias().empty()) local_bindings.insert(imp->get_namespace_alias());
                    for (const auto& spec : imp->get_specifiers()) local_bindings.insert(spec->get_local_name());
                } else if (t == ASTNode::Type::EXPORT_STATEMENT) {
                    auto* exp = static_cast<ExportStatement*>(stmt.get());
                    if (exp->is_declaration_export()) {
                        ASTNode* decl = exp->get_declaration();
                        if (decl) {
                            if (decl->get_type() == ASTNode::Type::FUNCTION_DECLARATION) {
                                auto* fn = static_cast<FunctionDeclaration*>(decl);
                                if (fn->get_id()) local_bindings.insert(fn->get_id()->get_name());
                            } else if (decl->get_type() == ASTNode::Type::CLASS_DECLARATION) {
                                auto* cls = static_cast<ClassDeclaration*>(decl);
                                if (cls->get_id()) local_bindings.insert(cls->get_id()->get_name());
                            } else if (decl->get_type() == ASTNode::Type::VARIABLE_DECLARATION) {
                                auto* vd = static_cast<VariableDeclaration*>(decl);
                                for (const auto& d : vd->get_declarations())
                                    if (d->get_id()) local_bindings.insert(d->get_id()->get_name());
                            }
                        }
                    }
                }
            }
            for (const auto& stmt : statements) {
                if (!stmt || stmt->get_type() != ASTNode::Type::EXPORT_STATEMENT) continue;
                auto* exp = static_cast<ExportStatement*>(stmt.get());
                // Only check non-re-export named exports (no source module)
                if (exp->is_default_export() || exp->is_declaration_export() || !exp->get_source_module().empty()) continue;
                for (const auto& spec : exp->get_specifiers()) {
                    const std::string& lname = spec->get_local_name();
                    if (lname == "*") continue;
                    if (local_bindings.find(lname) == local_bindings.end()) {
                        add_error("SyntaxError: Export of undeclared binding '" + lname + "'");
                        goto export_binding_done;
                    }
                }
            }
            export_binding_done:;
        }
    }

    Position end = get_current_position();
    auto program = std::make_unique<Program>(std::move(statements), start, end);
    if (options_.strict_mode) {
        program->set_strict(true);
    }
    return program;
}

std::unique_ptr<ASTNode> Parser::parse_statement() {
    // Decorators before class declarations
    if (current_token().get_type() == TokenType::AT) {
        skip_decorator_list();
        if (current_token().get_type() == TokenType::CLASS) {
            return parse_class_declaration();
        }
    }

    TokenType current_type = current_token().get_type();

    switch (current_type) {
        case TokenType::VAR:
        case TokenType::CONST:
            return parse_variable_declaration();
        case TokenType::LET:
            // Non-strict: "let" followed by "=", "(", ";", etc. is an identifier expression.
            // ASI: `let [` with a line terminator between is ExpressionStatement (array subscript).
            // `let id` (including let/yield/await/static) NEVER gets ASI even across lines.
            if (!options_.strict_mode) {
                size_t peek_idx = current_token_index_ + 1;
                bool has_lt = false;
                while (peek_idx < tokens_.size() &&
                       (tokens_[peek_idx].get_type() == TokenType::WHITESPACE ||
                        tokens_[peek_idx].get_type() == TokenType::NEWLINE ||
                        tokens_[peek_idx].get_type() == TokenType::COMMENT)) {
                    if (tokens_[peek_idx].get_type() == TokenType::NEWLINE)
                        has_lt = true;
                    peek_idx++;
                }
                TokenType next = (peek_idx < tokens_.size())
                                 ? tokens_[peek_idx].get_type()
                                 : TokenType::EOF_TOKEN;
                bool can_start_binding =
                    next == TokenType::IDENTIFIER || next == TokenType::LEFT_BRACKET ||
                    next == TokenType::LEFT_BRACE  || next == TokenType::LET        ||
                    next == TokenType::YIELD       || next == TokenType::AWAIT      ||
                    next == TokenType::STATIC      || next == TokenType::ASYNC;
                // `let [` or `let {` with line terminator: ASI applies (let as identifier)
                if (!can_start_binding ||
                    (has_lt && (next == TokenType::LEFT_BRACKET || next == TokenType::LEFT_BRACE))) {
                    return parse_expression_statement();
                }
            }
            return parse_variable_declaration();
            
        case TokenType::LEFT_BRACE:
            return parse_block_statement();
            
        case TokenType::IF:
            return parse_if_statement();
            
        case TokenType::FOR:
            return parse_for_statement();
            
        case TokenType::WHILE:
            return parse_while_statement();
            
        case TokenType::DO:
            return parse_do_while_statement();

        case TokenType::WITH:
            return parse_with_statement();

        case TokenType::FUNCTION:
            return parse_function_declaration();
            
        case TokenType::ASYNC: {
            // Only parse as async function declaration if 'function' is on the same line
            size_t async_line = current_token().get_end().line;
            size_t aft_idx = current_token_index_ + 1;
            while (aft_idx < tokens_.size() && tokens_[aft_idx].get_type() == TokenType::NEWLINE)
                aft_idx++;
            if (aft_idx < tokens_.size() && tokens_[aft_idx].get_type() == TokenType::FUNCTION &&
                tokens_[aft_idx].get_start().line == async_line)
                return parse_async_function_declaration();
            return parse_expression_statement();
        }
            
        case TokenType::CLASS:
            return parse_class_declaration();
            
        case TokenType::RETURN:
            return parse_return_statement();
            
        case TokenType::BREAK:
            return parse_break_statement();
            
        case TokenType::CONTINUE:
            return parse_continue_statement();
            
        case TokenType::TRY:
            return parse_try_statement();
            
        case TokenType::THROW:
            return parse_throw_statement();
            
        case TokenType::SWITCH:
            return parse_switch_statement();

        case TokenType::DEBUGGER:
            {
                Position start = current_token().get_start();
                Position end = current_token().get_end();
                advance();
                if (current_token().get_type() == TokenType::SEMICOLON) {
                    end = current_token().get_end();
                    advance();
                }
                return std::make_unique<EmptyStatement>(start, end);
            }

        case TokenType::IMPORT:
            if (peek_token().get_type() == TokenType::LEFT_PAREN ||
                peek_token().get_type() == TokenType::DOT) {
                return parse_expression_statement();
            } else {
                if (!options_.source_type_module) {
                    add_error("SyntaxError: import declarations are not allowed in scripts");
                    return nullptr;
                }
                if (options_.function_depth > 0 || options_.in_substatement_body ||
                    options_.in_block_context || options_.switch_depth > 0) {
                    add_error("SyntaxError: import declarations are only permitted at the top level of a module");
                    return nullptr;
                }
                return parse_import_statement();
            }

        case TokenType::EXPORT:
            if (!options_.source_type_module) {
                add_error("SyntaxError: export declarations are not allowed in scripts");
                return nullptr;
            }
            if (options_.function_depth > 0 || options_.in_substatement_body ||
                options_.in_block_context || options_.switch_depth > 0) {
                add_error("SyntaxError: export declarations are only permitted at the top level of a module");
                return nullptr;
            }
            return parse_export_statement();

        case TokenType::SEMICOLON:
            {
                Position start = current_token().get_start();
                Position end = current_token().get_end();
                advance();
                return std::make_unique<EmptyStatement>(start, end);
            }

        case TokenType::IDENTIFIER: {
            // Contextual keyword 'using': 'using id = expr' is a UsingDeclaration
            // when followed by an identifier (not '(' or '[' which would be a call/subscript)
            const std::string& val = current_token().get_value();
            if (val == "using") {
                // 'using [no LineTerminator here] BindingIdentifier' -- do not skip newlines.
                // Since whitespace is not in the token stream, just peek at the next token.
                // A NEWLINE token means line-terminator is present -- do NOT treat as declaration.
                size_t using_peek_idx = current_token_index_ + 1;
                // (whitespace tokens are not emitted by the lexer, so no need to skip them)
                TokenType next = (using_peek_idx < tokens_.size())
                                 ? tokens_[using_peek_idx].get_type()
                                 : TokenType::EOF_TOKEN;
                // 'using x =' pattern: next is an identifier that is NOT a keyword-as-expression
                // Exclude: 'using(', 'using[', 'using;', 'using=', operator follows, etc.
                // Allow 'await' as identifier when not in async context and not directly in static block
                bool next_is_await_id = (next == TokenType::AWAIT && !options_.in_async_body
                                         && !options_.in_class_static_block);
                if (next == TokenType::IDENTIFIER || next == TokenType::LET ||
                    next == TokenType::STATIC || next_is_await_id) {
                    if (options_.in_substatement_body) {
                        add_error("SyntaxError: 'using' declaration not allowed as substatement body");
                        return nullptr;
                    }
                    if (options_.in_switch_case_list) {
                        add_error("SyntaxError: 'using' declaration not allowed directly in switch case/default");
                        return nullptr;
                    }
                    if (!options_.in_block_context && !options_.source_type_module
                        && options_.function_depth == 0 && !options_.in_class_static_block) {
                        add_error("SyntaxError: 'using' declaration not allowed at top level of a script");
                        return nullptr;
                    }
                    return parse_using_declaration(false);
                }
            }
            return parse_expression_statement();
        }

        case TokenType::AWAIT: {
            // 'await [no LineTerminator here] using [no LineTerminator here] BindingIdentifier'
            // Only skip single-line whitespace/comments -- a NEWLINE breaks the no-LT-here restriction.
            size_t aw_peek_idx = current_token_index_ + 1;
            while (aw_peek_idx < tokens_.size() &&
                   tokens_[aw_peek_idx].get_type() == TokenType::WHITESPACE) {
                aw_peek_idx++;
            }
            if (aw_peek_idx < tokens_.size() &&
                tokens_[aw_peek_idx].get_type() == TokenType::IDENTIFIER &&
                tokens_[aw_peek_idx].get_value() == "using") {
                // No newline between await and using -- check what follows 'using'
                size_t aw_peek_idx2 = aw_peek_idx + 1;
                while (aw_peek_idx2 < tokens_.size() &&
                       tokens_[aw_peek_idx2].get_type() == TokenType::WHITESPACE) {
                    aw_peek_idx2++;
                }
                // If there's a NEWLINE between 'using' and the binding, it's two separate statements.
                // Otherwise check if it can be a BindingIdentifier.
                bool has_nl = (aw_peek_idx2 < tokens_.size() &&
                               tokens_[aw_peek_idx2].get_type() == TokenType::NEWLINE);
                if (!has_nl) {
                    TokenType aw_next2 = (aw_peek_idx2 < tokens_.size())
                                         ? tokens_[aw_peek_idx2].get_type()
                                         : TokenType::EOF_TOKEN;
                    bool aw_can_be_decl = (aw_next2 == TokenType::IDENTIFIER ||
                                           aw_next2 == TokenType::LET ||
                                           aw_next2 == TokenType::STATIC);
                    if (aw_can_be_decl) {
                        if (options_.in_substatement_body) {
                            add_error("SyntaxError: 'await using' declaration not allowed as substatement body");
                            return nullptr;
                        }
                        if (options_.in_switch_case_list) {
                            add_error("SyntaxError: 'await using' declaration not allowed directly in switch case/default");
                            return nullptr;
                        }
                        if (!options_.in_async_body && !options_.source_type_module) {
                            add_error("SyntaxError: 'await using' declaration not allowed outside async context");
                            return nullptr;
                        }
                        if (!options_.in_block_context && !options_.source_type_module
                            && options_.function_depth == 0 && !options_.in_class_static_block) {
                            add_error("SyntaxError: 'await using' declaration not allowed at top level of a script");
                            return nullptr;
                        }
                        return parse_using_declaration(true);
                    }
                }
            }
            if (options_.source_type_module && options_.function_depth > 0 && !options_.in_async_body) {
                add_error("SyntaxError: await is only valid in async functions and the top level bodies of modules");
                return nullptr;
            }
            return parse_expression_statement();
        }

        default:
            return parse_expression_statement();
    }
}

std::unique_ptr<ASTNode> Parser::parse_expression() {
    auto left = parse_assignment_expression();
    if (!left) return nullptr;
    
    while (match(TokenType::COMMA)) {
        advance();
        auto right = parse_assignment_expression();
        if (!right) {
            add_error("Expected expression after ','");
            return nullptr;
        }
        
        left = std::make_unique<BinaryExpression>(
            std::move(left), 
            BinaryExpression::Operator::COMMA,
            std::move(right), 
            left->get_start(), 
            right->get_end()
        );
    }
    
    return left;
}

std::unique_ptr<ASTNode> Parser::parse_assignment_expression() {
    last_expr_was_parenthesized_ = false;

    if (match(TokenType::IDENTIFIER) && peek_token(1).get_type() == TokenType::ARROW) {
        return parse_arrow_function();
    }

    // Non-strict, non-generator: yield/async can be plain arrow params (e.g. yield => 1)
    if (match(TokenType::YIELD) && !options_.in_generator_body && !options_.strict_mode &&
        peek_token(1).get_type() == TokenType::ARROW) {
        return parse_arrow_function();
    }

    if (match(TokenType::LEFT_PAREN)) {
        size_t saved_pos = current_token_index_;
        if (try_parse_arrow_function_params()) {
            current_token_index_ = saved_pos;
            return parse_arrow_function();
        }
        current_token_index_ = saved_pos;
    }

    auto left = parse_conditional_expression();
    if (!left) return nullptr;

    if (!options_.in_array_element &&
        left->get_type() == ASTNode::Type::OBJECT_LITERAL &&
        current_token().get_type() != TokenType::ASSIGN &&
        current_token().get_type() != TokenType::OF &&
        current_token().get_type() != TokenType::IN) {
        auto* obj = static_cast<ObjectLiteral*>(left.get());
        for (const auto& prop : obj->get_properties()) {
            if (prop->shorthand && prop->value &&
                prop->value->get_type() == ASTNode::Type::ASSIGNMENT_EXPRESSION) {
                add_error("SyntaxError: CoverInitializedName not allowed in object literal");
                return nullptr;
            }
        }
    }

    if (is_assignment_operator(current_token().get_type())) {
        bool lhs_was_paren = last_expr_was_parenthesized_;
        last_expr_was_parenthesized_ = false;
        // ObjectLiteral in parenthesized form ({}) = x → ATT = invalid per spec
        bool lhs_invalid = lhs_was_paren && left->get_type() == ASTNode::Type::OBJECT_LITERAL;
        if (lhs_invalid || !is_valid_assignment_target(left.get())) {
            add_error("SyntaxError: Invalid left-hand side in assignment");
            return nullptr;
        }

        if (options_.strict_mode && left->get_type() == ASTNode::Type::IDENTIFIER) {
            auto* id = static_cast<Identifier*>(left.get());
            if (id->get_name() == "eval" || id->get_name() == "arguments") {
                add_error("'" + id->get_name() + "' cannot be used as assignment target in strict mode");
                return nullptr;
            }
        }

        if (current_token().get_type() == TokenType::ASSIGN &&
            left->get_type() == ASTNode::Type::ARRAY_LITERAL) {
            if (!validate_array_destructuring(static_cast<ArrayLiteral*>(left.get())))
                return nullptr;
        }

        if (current_token().get_type() == TokenType::ASSIGN &&
            left->get_type() == ASTNode::Type::OBJECT_LITERAL) {
            if (!validate_object_destructuring(static_cast<ObjectLiteral*>(left.get())))
                return nullptr;
        }

        TokenType op_token = current_token().get_type();
        Position op_start = current_token().get_start();
        advance();

        auto right = parse_assignment_expression();
        if (!right) {
            add_error("Expected expression after assignment operator");
            return left;
        }

        // Convert TokenType to AssignmentExpression::Operator
        AssignmentExpression::Operator assign_op;
        switch (op_token) {
            case TokenType::ASSIGN:
                assign_op = AssignmentExpression::Operator::ASSIGN;
                break;
            case TokenType::PLUS_ASSIGN:
                assign_op = AssignmentExpression::Operator::PLUS_ASSIGN;
                break;
            case TokenType::MINUS_ASSIGN:
                assign_op = AssignmentExpression::Operator::MINUS_ASSIGN;
                break;
            case TokenType::MULTIPLY_ASSIGN:
                assign_op = AssignmentExpression::Operator::MUL_ASSIGN;
                break;
            case TokenType::DIVIDE_ASSIGN:
                assign_op = AssignmentExpression::Operator::DIV_ASSIGN;
                break;
            case TokenType::MODULO_ASSIGN:
                assign_op = AssignmentExpression::Operator::MOD_ASSIGN;
                break;
            case TokenType::BITWISE_AND_ASSIGN:
                assign_op = AssignmentExpression::Operator::BITWISE_AND_ASSIGN;
                break;
            case TokenType::BITWISE_OR_ASSIGN:
                assign_op = AssignmentExpression::Operator::BITWISE_OR_ASSIGN;
                break;
            case TokenType::BITWISE_XOR_ASSIGN:
                assign_op = AssignmentExpression::Operator::BITWISE_XOR_ASSIGN;
                break;
            case TokenType::LEFT_SHIFT_ASSIGN:
                assign_op = AssignmentExpression::Operator::LEFT_SHIFT_ASSIGN;
                break;
            case TokenType::RIGHT_SHIFT_ASSIGN:
                assign_op = AssignmentExpression::Operator::RIGHT_SHIFT_ASSIGN;
                break;
            case TokenType::UNSIGNED_RIGHT_SHIFT_ASSIGN:
                assign_op = AssignmentExpression::Operator::UNSIGNED_RIGHT_SHIFT_ASSIGN;
                break;
            case TokenType::EXPONENT_ASSIGN: {
                // Desugar: a **= b -> a = a ** b
                std::unique_ptr<ASTNode> bin_expr = std::make_unique<BinaryExpression>(
                    left->clone(), BinaryExpression::Operator::EXPONENT,
                    std::move(right), left->get_start(), right ? right->get_end() : op_start);
                return std::make_unique<AssignmentExpression>(std::move(left), AssignmentExpression::Operator::ASSIGN, std::move(bin_expr), op_start, get_current_position());
            }
            case TokenType::LOGICAL_OR_ASSIGN:
            case TokenType::LOGICAL_AND_ASSIGN:
            case TokenType::NULLISH_ASSIGN: {
                AssignmentExpression::Operator logical_op =
                    (op_token == TokenType::LOGICAL_AND_ASSIGN) ? AssignmentExpression::Operator::LOGICAL_AND_ASSIGN :
                    (op_token == TokenType::LOGICAL_OR_ASSIGN)  ? AssignmentExpression::Operator::LOGICAL_OR_ASSIGN :
                                                                   AssignmentExpression::Operator::NULLISH_ASSIGN;
                Position bend = right->get_end();
                return std::make_unique<AssignmentExpression>(std::move(left), logical_op, std::move(right), op_start, bend);
            }
            default:
                add_error("Unknown assignment operator");
                return left;
        }

        Position end = right->get_end();

        return std::make_unique<AssignmentExpression>(
            std::move(left), assign_op, std::move(right), op_start, end, lhs_was_paren
        );
    }

    return left;
}

std::unique_ptr<ASTNode> Parser::parse_conditional_expression() {
    return parse_conditional_expression_impl(0);
}

std::unique_ptr<ASTNode> Parser::parse_conditional_expression_impl(int depth) {
    if (depth > 100) {
        add_error("ternary nesting depth exceeded");
        return nullptr;
    }
    
    auto test = parse_logical_or_expression();
    if (!test) return nullptr;
    
    if (current_token().get_type() == TokenType::QUESTION) {
        advance();
        Position start = test->get_start();

        // Spec: consequent is AssignmentExpression[+In] -- always allow 'in'
        // Alternate is AssignmentExpression[?In] -- inherits no_in_mode from context
        bool saved_no_in = no_in_mode_;
        no_in_mode_ = false;  // +In for consequent

        auto consequent = parse_assignment_expression();
        if (!consequent) {
            no_in_mode_ = saved_no_in;
            add_error("Expected expression after '?' in conditional expression");
            return nullptr;
        }

        if (current_token().get_type() != TokenType::COLON) {
            no_in_mode_ = saved_no_in;
            add_error("Expected ':' after consequent in conditional expression");
            return nullptr;
        }
        advance();

        no_in_mode_ = saved_no_in;  // restore ?In for alternate
        auto alternate = parse_conditional_expression_impl(depth + 1);
        if (!alternate) {
            add_error("Expected expression after ':' in conditional expression");
            return nullptr;
        }

        Position end = alternate->get_end();
        return std::make_unique<ConditionalExpression>(std::move(test), std::move(consequent),
                                                      std::move(alternate), start, end);
    }

    return test;
}

std::unique_ptr<ASTNode> Parser::parse_logical_or_expression() {
    size_t left_start_idx = current_token_index_;
    auto left = parse_nullish_coalescing_expression();
    if (!left) return nullptr;

    while (match(TokenType::LOGICAL_OR)) {
        // ?? cannot appear as the left operand of unparenthesized ||
        bool left_is_nc = (left->get_type() == ASTNode::Type::NULLISH_COALESCING_EXPRESSION);
        bool left_was_paren = (left_start_idx < tokens_.size() &&
                               tokens_[left_start_idx].get_type() == TokenType::LEFT_PAREN);
        if (left_is_nc && !left_was_paren) {
            add_error("SyntaxError: Nullish coalescing operator cannot be mixed with || operator");
            return nullptr;
        }
        Position op_start = current_token().get_start();
        advance();
        size_t right_start_idx = current_token_index_;
        auto right = parse_nullish_coalescing_expression();
        if (!right) {
            add_error("Expected expression after '||'");
            return left;
        }
        bool right_is_nc = (right->get_type() == ASTNode::Type::NULLISH_COALESCING_EXPRESSION);
        bool right_was_paren = (right_start_idx < tokens_.size() &&
                                tokens_[right_start_idx].get_type() == TokenType::LEFT_PAREN);
        if (right_is_nc && !right_was_paren) {
            add_error("SyntaxError: Nullish coalescing operator cannot be mixed with || operator");
            return nullptr;
        }
        Position end = right->get_end();
        left_start_idx = current_token_index_;
        left = std::make_unique<BinaryExpression>(
            std::move(left), BinaryExpression::Operator::LOGICAL_OR, std::move(right), op_start, end);
    }
    return left;
}

std::unique_ptr<ASTNode> Parser::parse_nullish_coalescing_expression() {
    // Per spec: ?? cannot mix with bare (unparenthesized) && or ||
    // A parenthesized operand is fine: (a && b) ?? c  or  a ?? (b || c)
    auto is_unparenthesized_logical = [this](ASTNode* n, size_t start_idx) -> bool {
        if (!n || n->get_type() != ASTNode::Type::BINARY_EXPRESSION) return false;
        auto op = static_cast<BinaryExpression*>(n)->get_operator();
        if (op != BinaryExpression::Operator::LOGICAL_AND &&
            op != BinaryExpression::Operator::LOGICAL_OR) return false;
        // If the operand started with '(' it was parenthesized — allowed
        if (start_idx < tokens_.size() &&
            tokens_[start_idx].get_type() == TokenType::LEFT_PAREN) return false;
        return true;
    };

    size_t left_start_idx = current_token_index_;
    auto left = parse_logical_and_expression();
    if (!left) return nullptr;

    if (!match(TokenType::NULLISH_COALESCING)) return left;

    if (is_unparenthesized_logical(left.get(), left_start_idx)) {
        add_error("SyntaxError: Nullish coalescing operator cannot be mixed with && or || operators");
        return nullptr;
    }

    while (match(TokenType::NULLISH_COALESCING)) {
        Position start = left->get_start();
        advance();

        size_t right_start_idx = current_token_index_;
        auto right = parse_logical_and_expression();
        if (!right) {
            add_error("Expected expression after '??'");
            return left;
        }
        if (is_unparenthesized_logical(right.get(), right_start_idx)) {
            add_error("SyntaxError: Nullish coalescing operator cannot be mixed with && or || operators");
            return nullptr;
        }

        Position end = right->get_end();
        left = std::make_unique<NullishCoalescingExpression>(
            std::move(left), std::move(right), start, end
        );
    }

    return left;
}

std::unique_ptr<ASTNode> Parser::parse_logical_and_expression() {
    return parse_binary_expression(
        [this]() { return parse_bitwise_or_expression(); },
        {TokenType::LOGICAL_AND}
    );
}

std::unique_ptr<ASTNode> Parser::parse_bitwise_or_expression() {
    return parse_binary_expression(
        [this]() { return parse_bitwise_xor_expression(); },
        {TokenType::BITWISE_OR}
    );
}

std::unique_ptr<ASTNode> Parser::parse_bitwise_xor_expression() {
    return parse_binary_expression(
        [this]() { return parse_bitwise_and_expression(); },
        {TokenType::BITWISE_XOR}
    );
}

std::unique_ptr<ASTNode> Parser::parse_bitwise_and_expression() {
    return parse_binary_expression(
        [this]() { return parse_equality_expression(); },
        {TokenType::BITWISE_AND}
    );
}

std::unique_ptr<ASTNode> Parser::parse_equality_expression() {
    return parse_binary_expression(
        [this]() { return parse_relational_expression(); },
        {TokenType::EQUAL, TokenType::NOT_EQUAL, TokenType::STRICT_EQUAL, TokenType::STRICT_NOT_EQUAL}
    );
}

static bool is_private_identifier(const ASTNode* n) {
    if (!n || n->get_type() != ASTNode::Type::IDENTIFIER) return false;
    const auto& nm = static_cast<const Identifier*>(n)->get_name();
    return !nm.empty() && nm[0] == '#';
}

std::unique_ptr<ASTNode> Parser::parse_relational_expression() {
    auto left = parse_shift_expression();
    if (!left) return nullptr;

    if (is_private_identifier(left.get())) {
        if (no_in_mode_ || !match(TokenType::IN)) {
            add_error("SyntaxError: Private identifier '" +
                      static_cast<Identifier*>(left.get())->get_name() +
                      "' is not valid here");
            return nullptr;
        }
        Position op_start = current_token().get_start();
        advance();
        bool saved_bin = options_.in_binary_expr;
        options_.in_binary_expr = true;
        auto right = parse_shift_expression();
        options_.in_binary_expr = saved_bin;
        if (!right) {
            add_error("Expected expression after 'in'");
            return left;
        }
        if (is_private_identifier(right.get())) {
            add_error("SyntaxError: Private identifier '" +
                      static_cast<Identifier*>(right.get())->get_name() +
                      "' is not valid here");
            return nullptr;
        }
        Position end = right->get_end();
        left = std::make_unique<BinaryExpression>(
            std::move(left), BinaryExpression::Operator::IN, std::move(right), op_start, end);
    }

    static const std::vector<TokenType> relops_no_in  = {TokenType::LESS_THAN, TokenType::GREATER_THAN, TokenType::LESS_EQUAL, TokenType::GREATER_EQUAL, TokenType::INSTANCEOF};
    static const std::vector<TokenType> relops_with_in = {TokenType::LESS_THAN, TokenType::GREATER_THAN, TokenType::LESS_EQUAL, TokenType::GREATER_EQUAL, TokenType::INSTANCEOF, TokenType::IN};
    const std::vector<TokenType>& ops = no_in_mode_ ? relops_no_in : relops_with_in;

    while (match_any(ops)) {
        TokenType op_token = current_token().get_type();
        Position op_start = current_token().get_start();
        advance();
        bool saved_bin = options_.in_binary_expr;
        options_.in_binary_expr = true;
        auto right = parse_shift_expression();
        options_.in_binary_expr = saved_bin;
        if (!right) {
            add_error("Expected expression after binary operator");
            return left;
        }
        if (is_private_identifier(right.get())) {
            add_error("SyntaxError: Private identifier '" +
                      static_cast<Identifier*>(right.get())->get_name() +
                      "' is not valid here");
            return nullptr;
        }
        Position end = right->get_end();
        left = std::make_unique<BinaryExpression>(
            std::move(left), token_to_binary_operator(op_token), std::move(right), op_start, end);
    }

    return left;
}

std::unique_ptr<ASTNode> Parser::parse_shift_expression() {
    return parse_binary_expression(
        [this]() { return parse_additive_expression(); },
        {TokenType::LEFT_SHIFT, TokenType::RIGHT_SHIFT, TokenType::UNSIGNED_RIGHT_SHIFT}
    );
}

std::unique_ptr<ASTNode> Parser::parse_additive_expression() {
    return parse_binary_expression(
        [this]() { return parse_multiplicative_expression(); },
        {TokenType::PLUS, TokenType::MINUS}
    );
}

std::unique_ptr<ASTNode> Parser::parse_multiplicative_expression() {
    return parse_binary_expression(
        [this]() { return parse_exponentiation_expression(); },
        {TokenType::MULTIPLY, TokenType::DIVIDE, TokenType::MODULO}
    );
}

std::unique_ptr<ASTNode> Parser::parse_exponentiation_expression() {
    auto left = parse_unary_expression();
    if (!left) return nullptr;

    if (match(TokenType::EXPONENT)) {
        // Spec: UnaryExpression ** ExponentiationExpression is SyntaxError
        // UpdateExpressions (++x, --x, x++, x--) ARE valid before **
        // Parenthesized UnaryExpressions are valid: (-x) ** y is fine
        if (left->get_type() == ASTNode::Type::UNARY_EXPRESSION && !last_expr_was_parenthesized_) {
            auto* ue = static_cast<UnaryExpression*>(left.get());
            auto uop = ue->get_operator();
            bool is_update_expr = (uop == UnaryExpression::Operator::PRE_INCREMENT ||
                                   uop == UnaryExpression::Operator::PRE_DECREMENT ||
                                   uop == UnaryExpression::Operator::POST_INCREMENT ||
                                   uop == UnaryExpression::Operator::POST_DECREMENT);
            if (ue->is_prefix() && !is_update_expr) {
                add_error("SyntaxError: Unary operator before ** requires parentheses");
                return left;
            }
        }
        Position op_start = current_token().get_start();
        advance();
        
        auto right = parse_exponentiation_expression();
        if (!right) {
            add_error("Expected expression after ** operator");
            return left;
        }
        
        BinaryExpression::Operator op = BinaryExpression::Operator::EXPONENT;
        Position end = right->get_end();
        
        return std::make_unique<BinaryExpression>(
            std::move(left), op, std::move(right), op_start, end
        );
    }
    
    return left;
}

std::unique_ptr<ASTNode> Parser::parse_unary_expression() {
    if (current_token().get_type() == TokenType::AWAIT) {
        bool is_await_ctx = options_.in_async_body ||
                            (options_.source_type_module && options_.function_depth == 0) ||
                            options_.in_class_static_block;
        if (is_await_ctx) {
            Position start = current_token().get_start();
            if (options_.in_class_static_block) {
                add_error("SyntaxError: 'await' is not allowed inside class static block");
                return nullptr;
            }
            advance();

            auto argument = parse_unary_expression();
            if (!argument) {
                add_error("Expected expression after 'await'");
                return nullptr;
            }

            Position end = get_current_position();
            return std::make_unique<AwaitExpression>(std::move(argument), start, end);
        }
        // else: await is a valid identifier outside async context -- fall through
    }


    if (is_unary_operator(current_token().get_type())) {
        TokenType op_token = current_token().get_type();
        Position start = current_token().get_start();
        advance();

        bool saved_unary = options_.in_unary_operand;
        options_.in_unary_operand = true;
        auto operand = parse_unary_expression();
        options_.in_unary_operand = saved_unary;
        if (!operand) {
            add_error("Expected expression after unary operator");
            return nullptr;
        }

        UnaryExpression::Operator op = token_to_unary_operator(op_token);
        Position end = operand->get_end();

        // ES5: delete on identifier is SyntaxError in strict mode
        if (options_.strict_mode && op == UnaryExpression::Operator::DELETE &&
            operand->get_type() == ASTNode::Type::IDENTIFIER) {
            auto* id = static_cast<Identifier*>(operand.get());
            if (id->get_name() != "this") {
                add_error("SyntaxError: Delete of an unqualified identifier in strict mode");
                return nullptr;
            }
        }

        // delete of private name is always SyntaxError (class bodies are always strict)
        if (op == UnaryExpression::Operator::DELETE &&
            operand->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
            auto* mem = static_cast<MemberExpression*>(operand.get());
            if (!mem->is_computed() && mem->get_property() &&
                mem->get_property()->get_type() == ASTNode::Type::IDENTIFIER) {
                auto* prop = static_cast<Identifier*>(mem->get_property());
                if (!prop->get_name().empty() && prop->get_name()[0] == '#') {
                    add_error("SyntaxError: Cannot delete a private member");
                    return nullptr;
                }
            }
        }

        // PREFIX ++/-- on always-invalid targets
        if (op == UnaryExpression::Operator::PRE_INCREMENT ||
            op == UnaryExpression::Operator::PRE_DECREMENT) {
            ASTNode::Type et = operand->get_type();
            bool always_invalid = (et == ASTNode::Type::META_PROPERTY ||
                                   et == ASTNode::Type::YIELD_EXPRESSION ||
                                   et == ASTNode::Type::CALL_EXPRESSION ||
                                   et == ASTNode::Type::ARROW_FUNCTION_EXPRESSION ||
                                   et == ASTNode::Type::OPTIONAL_CHAINING_EXPRESSION ||
                                   et == ASTNode::Type::UNARY_EXPRESSION ||
                                   et == ASTNode::Type::NUMBER_LITERAL ||
                                   et == ASTNode::Type::STRING_LITERAL ||
                                   et == ASTNode::Type::BOOLEAN_LITERAL ||
                                   et == ASTNode::Type::NULL_LITERAL ||
                                   et == ASTNode::Type::REGEX_LITERAL ||
                                   et == ASTNode::Type::TEMPLATE_LITERAL ||
                                   et == ASTNode::Type::ARRAY_LITERAL ||
                                   et == ASTNode::Type::OBJECT_LITERAL);
            if (!always_invalid && et == ASTNode::Type::IDENTIFIER) {
                auto* id = static_cast<Identifier*>(operand.get());
                always_invalid = (id->get_name() == "this");
            }
            if (always_invalid) {
                add_error("SyntaxError: Invalid left-hand side in prefix operation");
                return nullptr;
            }
            if (options_.strict_mode && et == ASTNode::Type::IDENTIFIER) {
                auto* id = static_cast<Identifier*>(operand.get());
                if (id->get_name() == "eval" || id->get_name() == "arguments") {
                    add_error("SyntaxError: Invalid left-hand side expression in prefix operation");
                    return nullptr;
                }
            }
        }

        return std::make_unique<UnaryExpression>(op, std::move(operand), true, start, end);
    }

    return parse_postfix_expression();
}

std::unique_ptr<ASTNode> Parser::parse_postfix_expression() {
    auto expr = parse_call_expression();
    if (!expr) return nullptr;

    while (current_token().get_type() == TokenType::INCREMENT ||
           current_token().get_type() == TokenType::DECREMENT) {

        // ES1 ASI: postfix ++ and -- are "restricted productions"
        // If there's a line terminator between expression and ++/--, insert semicolon
        Position expr_end = expr->get_end();
        Position op_start = current_token().get_start();

        if (expr_end.line < op_start.line) {
            // Line terminator found - apply ASI, don't parse as postfix
            break;
        }

        // CallExpression, arrow, optional-chaining, literals etc. are always-invalid update targets
        {
            ASTNode::Type et = expr->get_type();
            bool always_invalid = (et == ASTNode::Type::META_PROPERTY ||
                                   et == ASTNode::Type::YIELD_EXPRESSION ||
                                   et == ASTNode::Type::CALL_EXPRESSION ||
                                   et == ASTNode::Type::ARROW_FUNCTION_EXPRESSION ||
                                   et == ASTNode::Type::OPTIONAL_CHAINING_EXPRESSION ||
                                   et == ASTNode::Type::NUMBER_LITERAL ||
                                   et == ASTNode::Type::STRING_LITERAL ||
                                   et == ASTNode::Type::BOOLEAN_LITERAL ||
                                   et == ASTNode::Type::NULL_LITERAL ||
                                   et == ASTNode::Type::REGEX_LITERAL ||
                                   et == ASTNode::Type::TEMPLATE_LITERAL ||
                                   et == ASTNode::Type::ARRAY_LITERAL ||
                                   et == ASTNode::Type::OBJECT_LITERAL);
            if (!always_invalid && et == ASTNode::Type::IDENTIFIER) {
                auto* id = static_cast<Identifier*>(expr.get());
                always_invalid = (id->get_name() == "this");
            }
            if (always_invalid) {
                add_error("SyntaxError: Invalid left-hand side in postfix operation");
                return expr;
            }
            if (!always_invalid && options_.strict_mode && et == ASTNode::Type::IDENTIFIER) {
                auto* id = static_cast<Identifier*>(expr.get());
                if (id->get_name() == "eval" || id->get_name() == "arguments") {
                    add_error("SyntaxError: Invalid left-hand side expression in postfix operation");
                    return expr;
                }
            }
        }

        TokenType op_token = current_token().get_type();
        Position start = expr->get_start();
        Position end = current_token().get_end();
        advance();

        UnaryExpression::Operator op = (op_token == TokenType::INCREMENT) ?
            UnaryExpression::Operator::POST_INCREMENT :
            UnaryExpression::Operator::POST_DECREMENT;

        expr = std::make_unique<UnaryExpression>(op, std::move(expr), false, start, end);
    }

    return expr;
}

std::unique_ptr<ASTNode> Parser::parse_call_expression() {
    std::unique_ptr<ASTNode> expr;

    if (current_token().get_type() == TokenType::NEW) {
        Position start = current_token().get_start();

        // new.target: spec allows line terminators between 'new', '.', and 'target'
        // peek past any NEWLINE tokens to find the DOT
        size_t new_peek_idx = current_token_index_ + 1;
        while (new_peek_idx < tokens_.size() &&
               tokens_[new_peek_idx].get_type() == TokenType::NEWLINE)
            new_peek_idx++;
        if (new_peek_idx < tokens_.size() && tokens_[new_peek_idx].get_type() == TokenType::DOT) {
            advance();  // past NEW -> now at first NEWLINE or DOT (advance skips NEWLINEs)
            advance();  // past DOT

            if (current_token().get_type() == TokenType::IDENTIFIER &&
                current_token().get_value() == "target") {
                if (current_token().has_escaped_keyword()) {
                    add_error("SyntaxError: 'target' in new.target cannot contain unicode escape sequences");
                    return nullptr;
                }
                // new.target is valid inside non-arrow functions, classes, or eval that is
                // itself inside function code. It's a SyntaxError in global code and in eval
                // called from global scope.
                bool in_function_or_class = options_.non_arrow_function_depth > 0 || options_.class_depth > 0;
                bool in_valid_eval = options_.in_eval_context && options_.eval_in_function_code;
                if (!in_function_or_class && !in_valid_eval) {
                    add_error("SyntaxError: new.target is only valid inside a function or class body");
                    return nullptr;
                }
                Position end = current_token().get_end();
                advance();
                expr = std::make_unique<MetaProperty>("new", "target", start, end);
                // Skip regular new expression processing; fall through to optional chain loop
                goto parse_call_suffix;
            } else {
                add_error("Expected 'target' after 'new.'");
                return nullptr;
            }
        }

        advance();

        if (current_token().get_type() == TokenType::IMPORT) {
            // new import.meta is valid (though runtime TypeError); new import(...) is SyntaxError
            size_t peek_idx = current_token_index_ + 1;
            while (peek_idx < tokens_.size() &&
                   (tokens_[peek_idx].get_type() == TokenType::NEWLINE ||
                    tokens_[peek_idx].get_type() == TokenType::WHITESPACE))
                peek_idx++;
            bool is_import_meta = (peek_idx < tokens_.size() &&
                                   tokens_[peek_idx].get_type() == TokenType::DOT);
            if (is_import_meta) {
                size_t after_dot = peek_idx + 1;
                while (after_dot < tokens_.size() &&
                       (tokens_[after_dot].get_type() == TokenType::NEWLINE ||
                        tokens_[after_dot].get_type() == TokenType::WHITESPACE))
                    after_dot++;
                is_import_meta = (after_dot < tokens_.size() &&
                                  tokens_[after_dot].get_type() == TokenType::IDENTIFIER &&
                                  tokens_[after_dot].get_value() == "meta");
            }
            if (!is_import_meta) {
                add_error("SyntaxError: 'import' cannot be used with 'new'");
                return nullptr;
            }
            // Fall through: parse_primary_expression will handle import.meta
        }

        // Allow nested new: `new new X()` -- inner new is itself a NewExpression
        std::unique_ptr<ASTNode> constructor;
        if (current_token().get_type() == TokenType::NEW) {
            constructor = parse_call_expression();
        } else {
            constructor = parse_primary_expression();
        }
        if (!constructor) {
            add_error("Expected constructor expression after 'new'");
            return nullptr;
        }

        while (match(TokenType::DOT) || match(TokenType::LEFT_BRACKET)) {
            Position ctor_start = constructor->get_start();

            if (match(TokenType::DOT)) {
                advance();

                std::string name;
                Position prop_start;
                Position prop_end;

                if (match(TokenType::HASH)) {
                    prop_start = current_token().get_start();
                    advance();

                    if (!match(TokenType::IDENTIFIER)) {
                        add_error("Expected identifier after '#' in member access");
                        return nullptr;
                    }

                    name = "#" + current_token().get_value();
                    prop_end = current_token().get_end();
                    advance();

                    // Validate private name scope: allowed inside class body, or in eval if
                    // the specific name is declared in the enclosing class's private scope.
                    if (options_.class_depth == 0) {
                        bool allowed = options_.in_eval_context &&
                                       !options_.eval_private_names.empty() &&
                                       options_.eval_private_names.count(name) > 0;
                        if (!allowed) {
                            add_error("SyntaxError: Private names are not allowed outside class bodies");
                            return nullptr;
                        }
                    }
                } else if (match(TokenType::IDENTIFIER) || is_keyword_token(current_token().get_type())) {
                    const Token& token = current_token();
                    name = token.get_value();
                    prop_start = token.get_start();
                    prop_end = token.get_end();
                    advance();
                } else {
                    add_error("Expected property name after '.'");
                    return nullptr;
                }

                auto property = std::make_unique<Identifier>(name, prop_start, prop_end);

                Position end = property->get_end();
                constructor = std::make_unique<MemberExpression>(
                    std::move(constructor), std::move(property), false, ctor_start, end
                );
            } else if (match(TokenType::LEFT_BRACKET)) {
                advance();
                auto computed_prop = parse_assignment_expression();
                if (!computed_prop) {
                    add_error("Expected expression in computed member access");
                    return nullptr;
                }
                if (!consume(TokenType::RIGHT_BRACKET)) {
                    add_error("Expected ']' after computed member expression");
                    return nullptr;
                }
                Position end = get_current_position();
                constructor = std::make_unique<MemberExpression>(
                    std::move(constructor), std::move(computed_prop), true, ctor_start, end
                );
            }
        }

        std::vector<std::unique_ptr<ASTNode>> arguments;

        if (current_token().get_type() == TokenType::LEFT_PAREN) {
            advance();

            if (current_token().get_type() != TokenType::RIGHT_PAREN) {
                do {
                    if (match(TokenType::ELLIPSIS)) {
                        auto spread = parse_spread_element();
                        if (!spread) {
                            add_error("Invalid spread argument in new expression");
                            return nullptr;
                        }
                        arguments.push_back(std::move(spread));
                    } else {
                        auto arg = parse_assignment_expression();
                        if (!arg) {
                            add_error("Expected argument expression");
                            return nullptr;
                        }
                        arguments.push_back(std::move(arg));
                    }

                    if (consume_if_match(TokenType::COMMA)) {
                        if (current_token().get_type() == TokenType::RIGHT_PAREN) {
                            break;
                        }
                    } else {
                        break;
                    }
                } while (true);
            }

            if (!consume(TokenType::RIGHT_PAREN)) {
                add_error("Expected ')' after arguments");
                return nullptr;
            }
        }

        Position end = get_current_position();
        expr = std::make_unique<NewExpression>(std::move(constructor), std::move(arguments), start, end);
    } else {
        expr = parse_primary_expression();
        if (!expr) return nullptr;
    }

    parse_call_suffix:
    while (match(TokenType::DOT) || match(TokenType::LEFT_BRACKET) ||
           match(TokenType::OPTIONAL_CHAINING) || match(TokenType::LEFT_PAREN) ||
           match(TokenType::TEMPLATE_LITERAL)) {
        Position start = expr->get_start();
        
        if (match(TokenType::DOT)) {
            advance();

            std::string name;
            Position prop_start;
            Position prop_end;

            if (match(TokenType::HASH)) {
                // super.#name is not valid
                if (expr->get_type() == ASTNode::Type::IDENTIFIER &&
                    static_cast<Identifier*>(expr.get())->get_name() == "super") {
                    add_error("SyntaxError: Private fields cannot be accessed via 'super'");
                    return nullptr;
                }
                prop_start = current_token().get_start();
                size_t hash_offset = current_token().get_start().offset;
                advance();

                if (!match(TokenType::IDENTIFIER)) {
                    add_error("Expected identifier after '#' in member access");
                    return expr;
                }

                if (current_token().get_start().offset != hash_offset + 1) {
                    add_error("SyntaxError: No whitespace allowed between '#' and identifier");
                    return nullptr;
                }

                name = "#" + current_token().get_value();
                prop_end = current_token().get_end();
                advance();

                // Validate private name scope: allowed inside class body, or in eval if
                // the specific name is declared in the enclosing class's private scope.
                if (options_.class_depth == 0) {
                    bool allowed = options_.in_eval_context &&
                                   !options_.eval_private_names.empty() &&
                                   options_.eval_private_names.count(name) > 0;
                    if (!allowed) {
                        add_error("SyntaxError: Private names are not allowed outside class bodies");
                        return nullptr;
                    }
                }
            } else if (match(TokenType::IDENTIFIER) || is_keyword_token(current_token().get_type())) {
                const Token& token = current_token();
                name = token.get_value();
                prop_start = token.get_start();
                prop_end = token.get_end();
                advance();
            } else {
                add_error("Expected property name after '.'");
                return expr;
            }

            auto property = std::make_unique<Identifier>(name, prop_start, prop_end);
            expr = std::make_unique<MemberExpression>(
                std::move(expr), std::move(property), false, start, prop_end
            );
        } else if (match(TokenType::LEFT_BRACKET)) {
            advance();
            
            auto property = parse_expression();
            if (!property) {
                add_error("Expected expression inside []");
                return expr;
            }
            
            if (!consume(TokenType::RIGHT_BRACKET)) {
                add_error("Expected ']' after computed property");
                return expr;
            }
            
            Position end = get_current_position();
            expr = std::make_unique<MemberExpression>(
                std::move(expr), std::move(property), true, start, end
            );
        } else if (match(TokenType::OPTIONAL_CHAINING)) {
            advance();
            
            if (match(TokenType::LEFT_BRACKET)) {
                advance();
                
                auto property = parse_expression();
                if (!property) {
                    add_error("Expected expression inside []");
                    return expr;
                }
                
                if (!consume(TokenType::RIGHT_BRACKET)) {
                    add_error("Expected ']' after computed property");
                    return expr;
                }
                
                Position end = get_current_position();
                expr = std::make_unique<OptionalChainingExpression>(
                    std::move(expr), std::move(property), true, start, end
                );
            } else if (match(TokenType::LEFT_PAREN)) {
                advance();

                std::vector<std::unique_ptr<ASTNode>> arguments;
                if (!match(TokenType::RIGHT_PAREN)) {
                    do {
                        if (match(TokenType::ELLIPSIS)) {
                            auto spread = parse_spread_element();
                            if (!spread) { add_error("Invalid spread in optional call"); break; }
                            arguments.push_back(std::move(spread));
                        } else {
                            auto arg = parse_assignment_expression();
                            if (!arg) {
                                add_error("Expected argument in optional call");
                                break;
                            }
                            arguments.push_back(std::move(arg));
                        }

                        if (consume_if_match(TokenType::COMMA)) {
                            if (current_token().get_type() == TokenType::RIGHT_PAREN) {
                                break;
                            }
                        } else {
                            break;
                        }
                    } while (true);
                }

                if (!consume(TokenType::RIGHT_PAREN)) {
                    add_error("Expected ')' after optional call arguments");
                    return expr;
                }

                Position end = get_current_position();
                expr = std::make_unique<CallExpression>(std::move(expr), std::move(arguments), start, end, true);
            } else {
                if (match(TokenType::HASH)) {
                    auto private_field = parse_private_field();
                    if (!private_field) {
                        add_error("Invalid private field after '?.'");
                        return expr;
                    }
                    auto property = std::unique_ptr<Identifier>(static_cast<Identifier*>(private_field.release()));
                    Position end = property->get_end();
                    expr = std::make_unique<OptionalChainingExpression>(
                        std::move(expr), std::move(property), false, start, end
                    );
                } else {
                if (!match(TokenType::IDENTIFIER) && current_token().get_type() != TokenType::FOR &&
                    current_token().get_type() != TokenType::FROM && current_token().get_type() != TokenType::OF &&
                    current_token().get_type() != TokenType::DELETE) {
                    add_error("Expected property name after '?.'");
                    return expr;
                }

                const Token& token = current_token();
                std::string name = token.get_value();
                Position prop_start = token.get_start();
                Position prop_end = token.get_end();
                advance();
                auto property = std::make_unique<Identifier>(name, prop_start, prop_end);

                Position end = property->get_end();
                expr = std::make_unique<OptionalChainingExpression>(
                    std::move(expr), std::move(property), false, start, end
                );
                }
            }
        } else if (match(TokenType::LEFT_PAREN)) {
            advance();
            
            std::vector<std::unique_ptr<ASTNode>> arguments;
            
            if (!match(TokenType::RIGHT_PAREN)) {
                do {
                    if (match(TokenType::ELLIPSIS)) {
                        auto spread = parse_spread_element();
                        if (!spread) {
                            add_error("Invalid spread argument in function call");
                            break;
                        }
                        arguments.push_back(std::move(spread));
                    } else {
                        auto arg = parse_assignment_expression();
                        if (!arg) {
                            add_error("Expected argument in function call");
                            break;
                        }
                        arguments.push_back(std::move(arg));
                    }

                    if (consume_if_match(TokenType::COMMA)) {
                        if (current_token().get_type() == TokenType::RIGHT_PAREN) {
                            break;
                        }
                    } else {
                        break;
                    }
                } while (true);
            }
            
            if (!consume(TokenType::RIGHT_PAREN)) {
                add_error("Expected ')' after function arguments");
                return expr;
            }
            
            Position end = get_current_position();
            // super() call validation
            if (expr && expr->get_type() == ASTNode::Type::IDENTIFIER) {
                auto* callee_id = static_cast<Identifier*>(expr.get());
                if (callee_id->get_name() == "super") {
                    if (options_.in_class_field_init) {
                        add_error("SyntaxError: super() call is not valid in class field initializer");
                        return nullptr;
                    }
                    if (!options_.in_constructor || !options_.class_has_heritage) {
                        add_error("SyntaxError: super() is only valid in derived class constructor");
                        return nullptr;
                    }
                }
            }
            expr = std::make_unique<CallExpression>(std::move(expr), std::move(arguments), start, end);
        } else if (match(TokenType::TEMPLATE_LITERAL)) {
            if (expr && expr->get_type() == ASTNode::Type::OPTIONAL_CHAINING_EXPRESSION) {
                add_error("SyntaxError: Tagged template literal cannot be used in optional chain");
                return nullptr;
            }
            // Tagged template literal: fn`...`
            auto template_node = parse_template_literal();
            std::vector<std::unique_ptr<ASTNode>> arguments;
            arguments.push_back(std::move(template_node));
            Position end = get_current_position();
            auto call = std::make_unique<CallExpression>(std::move(expr), std::move(arguments), start, end);
            call->set_tagged_template(true);
            expr = std::move(call);
        }
    }
    
    return expr;
}

std::unique_ptr<ASTNode> Parser::parse_member_expression() {
    auto expr = parse_primary_expression();
    if (!expr) return nullptr;
    
    while (match(TokenType::DOT) || match(TokenType::LEFT_BRACKET) || match(TokenType::OPTIONAL_CHAINING)) {
        Position start = expr->get_start();
        
        if (match(TokenType::DOT)) {
            advance();

            if (!match(TokenType::IDENTIFIER) && !is_keyword_token(current_token().get_type()) && !match(TokenType::HASH)) {
                add_error("Expected property name after '.', got token type: " + std::to_string(static_cast<int>(current_token().get_type())));
                return expr;
            }

            std::unique_ptr<Identifier> property;
            if (match(TokenType::HASH)) {
                auto private_field = parse_private_field();
                if (!private_field) {
                    add_error("Invalid private field after '.'");
                    return expr;
                }
                property = std::unique_ptr<Identifier>(static_cast<Identifier*>(private_field.release()));
            } else {
                const Token& token = current_token();
                std::string name = token.get_value();
                Position prop_start = token.get_start();
                Position prop_end = token.get_end();
                advance();
                property = std::make_unique<Identifier>(name, prop_start, prop_end);
            }
            
            Position end = property->get_end();
            expr = std::make_unique<MemberExpression>(
                std::move(expr), std::move(property), false, start, end
            );
        } else if (match(TokenType::OPTIONAL_CHAINING)) {
            advance();
            
            if (match(TokenType::LEFT_BRACKET)) {
                advance();
                
                auto property = parse_expression();
                if (!property) {
                    add_error("Expected expression inside []");
                    return expr;
                }
                
                if (!consume(TokenType::RIGHT_BRACKET)) {
                    add_error("Expected ']' after computed property");
                    return expr;
                }
                
                Position end = get_current_position();
                expr = std::make_unique<OptionalChainingExpression>(
                    std::move(expr), std::move(property), true, start, end
                );
            } else if (match(TokenType::IDENTIFIER) || is_keyword_token(current_token().get_type())) {
                const Token& token = current_token();
                std::string name = token.get_value();
                Position prop_start = token.get_start();
                Position prop_end = token.get_end();
                advance();
                auto property = std::make_unique<Identifier>(name, prop_start, prop_end);
                
                Position end = property->get_end();
                expr = std::make_unique<OptionalChainingExpression>(
                    std::move(expr), std::move(property), false, start, end
                );
            } else {
                add_error("Expected property name or '[' after '?.'");
                return expr;
            }
        } else {
            advance();
            
            auto property = parse_expression();
            if (!property) {
                add_error("Expected expression inside []");
                return expr;
            }
            
            if (!consume(TokenType::RIGHT_BRACKET)) {
                add_error("Expected ']' after computed property");
                return expr;
            }
            
            Position end = get_current_position();
            expr = std::make_unique<MemberExpression>(
                std::move(expr), std::move(property), true, start, end
            );
        }
    }
    
    return expr;
}

std::unique_ptr<ASTNode> Parser::parse_primary_expression() {
    const Token& token = current_token();
    
    switch (token.get_type()) {
        case TokenType::NUMBER:
            return parse_number_literal();
        case TokenType::STRING:
            return parse_string_literal();
        case TokenType::BOOLEAN:
            return parse_boolean_literal();
        case TokenType::NULL_LITERAL:
            return parse_null_literal();
        case TokenType::BIGINT_LITERAL:
            return parse_bigint_literal();
        case TokenType::UNDEFINED:
            // In ES5, undefined is treated as an identifier (can be reassigned)
            {
                Position start = current_token().get_start();
                Position end = current_token().get_end();
                advance();
                return std::make_unique<Identifier>("undefined", start, end);
            }
        case TokenType::IDENTIFIER:
            if (options_.strict_mode) {
                static const std::unordered_set<std::string> strict_future = {
                    "implements","interface","package","private","protected","public"
                };
                if (strict_future.count(current_token().get_value())) {
                    add_error("SyntaxError: '" + current_token().get_value() + "' is a reserved word in strict mode");
                    return nullptr;
                }
            }
            return parse_identifier();
        case TokenType::HASH:
            return parse_private_field();
        case TokenType::THIS:
            return parse_this_expression();
        case TokenType::SUPER:
            return parse_super_expression();
        case TokenType::LEFT_PAREN:
            return parse_parenthesized_expression();
        case TokenType::ASYNC:
            return parse_async_function_expression();
        case TokenType::FUNCTION:
            return parse_function_expression();
        case TokenType::CLASS:
            return parse_class_expression();
        case TokenType::AT: {
            // Decorator before class expression: @expr class {}
            skip_decorator_list();
            if (current_token().get_type() == TokenType::CLASS) {
                return parse_class_expression();
            }
            add_error("SyntaxError: Unexpected token '@'");
            return nullptr;
        }
        case TokenType::YIELD:
            // Outside generator bodies (non-strict), yield is a valid identifier
            if (!options_.in_generator_body && !options_.strict_mode) {
                auto id = std::make_unique<Identifier>("yield",
                    current_token().get_start(), current_token().get_end());
                advance();
                return id;
            }
            if (options_.in_arrow_params) {
                add_error("SyntaxError: 'yield' is not allowed in arrow function parameters");
                return nullptr;
            }
            if (!options_.in_generator_body && options_.strict_mode) {
                add_error("SyntaxError: 'yield' is a reserved word in strict mode");
                return nullptr;
            }
            if ((options_.in_binary_expr || options_.in_unary_operand) && options_.in_generator_body) {
                add_error("SyntaxError: 'yield' cannot be used in this expression context in a generator");
                return nullptr;
            }
            return parse_yield_expression();
        case TokenType::LET:
            // Non-strict: let is a valid identifier
            if (!options_.strict_mode) {
                auto id = std::make_unique<Identifier>("let",
                    current_token().get_start(), current_token().get_end());
                advance();
                return id;
            }
            break;
        case TokenType::OF:
        {   // 'of' is always a contextual keyword -- valid as identifier in expressions
            auto id = std::make_unique<Identifier>("of",
                current_token().get_start(), current_token().get_end());
            advance();
            return id;
        }
        case TokenType::IMPORT:
            return parse_import_expression();
        case TokenType::LEFT_BRACE:
            return parse_object_literal();
        case TokenType::LEFT_BRACKET:
            return parse_array_literal();
        case TokenType::TEMPLATE_LITERAL: {
            // Untagged: invalid escape (\x01 sentinel in cooked) -> SyntaxError
            const std::string& tv = current_token().get_value();
            if (tv.size() >= 4) {
                uint32_t cooked_len = (static_cast<unsigned char>(tv[0]) << 24) |
                                      (static_cast<unsigned char>(tv[1]) << 16) |
                                      (static_cast<unsigned char>(tv[2]) << 8)  |
                                       static_cast<unsigned char>(tv[3]);
                // search for sentinel in cooked portion
                for (size_t ci = 4; ci < 4 + cooked_len && ci < tv.size(); ci++) {
                    if (static_cast<unsigned char>(tv[ci]) == 0x01) {
                        add_error("SyntaxError: Invalid escape sequence in template literal");
                        return nullptr;
                    }
                }
            }
            return parse_template_literal();
        }
        case TokenType::REGEX:
            return parse_regex_literal();
        case TokenType::LESS_THAN:
            return parse_jsx_element();
        case TokenType::AWAIT:
            // Outside async context: await is a valid identifier
            if (!options_.in_async_body && !options_.source_type_module && !options_.in_class_static_block) {
                auto id = std::make_unique<Identifier>("await",
                    token.get_start(), token.get_end());
                advance();
                return id;
            }
            // Fall through to error
            [[fallthrough]];
        default: {
            std::string tok_val = token.get_value().empty()
                ? Token::token_type_name(token.get_type())
                : "'" + token.get_value() + "'";
            std::string error_msg = "SyntaxError: Unexpected token " + tok_val;
            add_error(error_msg, token.get_start());
            advance();
            return nullptr;
        }
    }
}

std::unique_ptr<ASTNode> Parser::parse_number_literal() {
    const Token& token = current_token();
    double value = token.has_numeric_value() ? token.get_numeric_value() : 0.0;
    
    Position start = token.get_start();
    Position end = token.get_end();
    advance();
    
    return std::make_unique<NumberLiteral>(value, start, end);
}

std::unique_ptr<ASTNode> Parser::parse_string_literal() {
    const Token& token = current_token();
    std::string value = token.get_value();
    bool has_escapes = token.string_has_escapes();

    Position start = token.get_start();
    Position end = token.get_end();
    advance();

    return std::make_unique<StringLiteral>(value, start, end, has_escapes);
}

std::unique_ptr<ASTNode> Parser::parse_this_expression() {
    const Token& token = current_token();
    Position start = token.get_start();
    Position end = token.get_end();
    advance();
    
    return std::make_unique<Identifier>("this", start, end);
}

std::unique_ptr<ASTNode> Parser::parse_super_expression() {
    const Token& token = current_token();
    Position start = token.get_start();
    Position end = token.get_end();

    // super is valid inside class/object methods, static blocks, field inits, or eval inside methods.
    // In eval called from global or regular function code (no [[HomeObject]]), super is a SyntaxError.
    bool in_method_or_eval_in_method = options_.in_class_method ||
        (options_.in_eval_context && options_.eval_in_method_code);
    if (!in_method_or_eval_in_method) {
        add_error("SyntaxError: 'super' keyword unexpected here");
        advance();
        return nullptr;
    }

    advance();
    // Peek: if super() call, check it's in derived constructor
    // (actual call check happens in parse_call_expression via in_constructor/class_has_heritage)
    return std::make_unique<Identifier>("super", start, end);
}

// Helper: extract text parts from a template string by splitting on ${...} markers
static std::vector<std::string> extract_template_text_parts(const std::string& str) {
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos <= str.length()) {
        size_t expr_start = str.find("${", pos);
        if (expr_start == std::string::npos) {
            parts.push_back(str.substr(pos));
            break;
        }
        parts.push_back(str.substr(pos, expr_start - pos));
        int brace_count = 1;
        size_t i = expr_start + 2;
        while (i < str.length() && brace_count > 0) {
            if (str[i] == '{') brace_count++;
            else if (str[i] == '}') brace_count--;
            i++;
        }
        pos = i;
    }
    return parts;
}

std::unique_ptr<ASTNode> Parser::parse_template_literal() {
    const Token& token = current_token();
    Position start = token.get_start();
    Position end = token.get_end();
    std::string template_str = token.get_value();

    if (!match(TokenType::TEMPLATE_LITERAL)) {
        add_error("Expected template literal");
        return nullptr;
    }

    advance();

    // Split cooked and raw parts: 4-byte big-endian cooked length prefix, then cooked, then raw.
    std::string cooked_str, raw_str;
    if (template_str.size() >= 4) {
        uint32_t cooked_len = (static_cast<uint8_t>(template_str[0]) << 24) |
                              (static_cast<uint8_t>(template_str[1]) << 16) |
                              (static_cast<uint8_t>(template_str[2]) << 8)  |
                               static_cast<uint8_t>(template_str[3]);
        if (4 + cooked_len <= template_str.size()) {
            cooked_str = template_str.substr(4, cooked_len);
            raw_str = template_str.substr(4 + cooked_len);
        } else {
            cooked_str = template_str.substr(4);
        }
    } else {
        cooked_str = template_str;
        raw_str = template_str;
    }

    // Extract raw text parts
    auto raw_text_parts = extract_template_text_parts(raw_str);

    // Extract cooked text parts and expressions
    auto cooked_text_parts = extract_template_text_parts(cooked_str);

    // Parse expressions from cooked string
    std::vector<std::unique_ptr<ASTNode>> expressions;
    {
        size_t pos = 0;
        while (pos < cooked_str.length()) {
            size_t expr_start = cooked_str.find("${", pos);
            if (expr_start == std::string::npos) break;

            size_t expr_end = std::string::npos;
            int brace_count = 1;
            for (size_t i = expr_start + 2; i < cooked_str.length(); ++i) {
                if (cooked_str[i] == '{') brace_count++;
                else if (cooked_str[i] == '}') {
                    brace_count--;
                    if (brace_count == 0) { expr_end = i; break; }
                }
            }

            if (expr_end == std::string::npos) {
                add_error("Unterminated expression in template literal");
                return nullptr;
            }

            std::string expr_str = cooked_str.substr(expr_start + 2, expr_end - expr_start - 2);
            Lexer expr_lexer(expr_str);
            TokenSequence expr_tokens = expr_lexer.tokenize();
            // Inherit outer parser context so `yield`/`await` inside a `${...}` substitution resolve correctly.
            Parser expr_parser(std::move(expr_tokens), options_);
            auto expression = expr_parser.parse_expression();
            if (!expression) {
                add_error("Invalid expression in template literal: " + expr_str);
                return nullptr;
            }
            expressions.push_back(std::move(expression));
            pos = expr_end + 1;
        }
    }

    // Build elements: interleave text parts and expressions
    // Template: text[0] ${expr[0]} text[1] ${expr[1]} ... text[n]
    std::vector<TemplateLiteral::Element> elements;
    for (size_t i = 0; i < cooked_text_parts.size(); i++) {
        std::string raw = (i < raw_text_parts.size()) ? raw_text_parts[i] : cooked_text_parts[i];
        elements.emplace_back(cooked_text_parts[i], raw);
        if (i < expressions.size()) {
            elements.emplace_back(std::move(expressions[i]));
        }
    }

    return std::make_unique<TemplateLiteral>(std::move(elements), start, end);
}

static uint32_t decode_utf8_cp(const std::string& s, size_t& i) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) { return c; }
    if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
        uint32_t cp = ((c & 0x1F) << 6) | ((unsigned char)s[i+1] & 0x3F);
        i += 1; return cp;
    }
    if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
        uint32_t cp = ((c & 0x0F) << 12) | (((unsigned char)s[i+1] & 0x3F) << 6) | ((unsigned char)s[i+2] & 0x3F);
        i += 2; return cp;
    }
    if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
        uint32_t cp = ((c & 0x07) << 18) | (((unsigned char)s[i+1] & 0x3F) << 12) |
                      (((unsigned char)s[i+2] & 0x3F) << 6) | ((unsigned char)s[i+3] & 0x3F);
        i += 3; return cp;
    }
    return 0xFFFD;
}

static bool is_unicode_id_start(uint32_t cp) {
    if (cp < 0x80) return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || cp == '_' || cp == '$';
    if (cp >= 0xD800 && cp <= 0xDFFF) return false; // surrogates
    if (cp > 0x10FFFF) return false;
    // reject non-letter symbol blocks
    if (cp >= 0x2000 && cp <= 0x27FF) return false; // punctuation/symbols/dingbats
    if (cp >= 0x2E00 && cp <= 0x2FFF) return false;
    if (cp >= 0x3000 && cp <= 0x3004) return false; // CJK symbols partial
    if (cp >= 0x104A0 && cp <= 0x104A9) return false; // Osmanya digits
    if (cp >= 0x1F000 && cp <= 0x1FFFF) return false; // emoji/special blocks
    if (cp == 0x10FFFF) return false; // last codepoint, not a letter
    return true;
}

static bool is_unicode_id_continue(uint32_t cp) {
    if (cp < 0x80) return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9') || cp == '_' || cp == '$';
    if (cp >= 0xD800 && cp <= 0xDFFF) return false;
    if (cp > 0x10FFFF) return false;
    if (cp >= 0x2000 && cp <= 0x27FF) return false;
    if (cp >= 0x2E00 && cp <= 0x2FFF) return false;
    if (cp >= 0x104A0 && cp <= 0x104A9) return false;
    if (cp >= 0x1F000 && cp <= 0x1FFFF) return false;
    if (cp == 0x10FFFF) return false;
    return true;
}

static bool parse_regex_hex_escape(const std::string& name, size_t& i, uint32_t& out_cp) {
    // parses \uHHHH or \u{HHHHHH} starting after 'u', returns false on failure
    if (i >= name.size()) return false;
    if (name[i] == '{') {
        i++;
        uint32_t val = 0;
        bool has_digit = false;
        while (i < name.size() && name[i] != '}') {
            char c = name[i++];
            val <<= 4;
            if (c >= '0' && c <= '9') val |= (c - '0');
            else if (c >= 'a' && c <= 'f') val |= (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') val |= (c - 'A' + 10);
            else return false;
            has_digit = true;
        }
        if (i >= name.size() || name[i] != '}' || !has_digit) return false;
        i++; // consume '}'
        out_cp = val;
        return true;
    }
    // \uHHHH -- exactly 4 hex digits
    if (i + 4 > name.size()) return false;
    uint32_t val = 0;
    for (int k = 0; k < 4; k++) {
        char c = name[i++];
        val <<= 4;
        if (c >= '0' && c <= '9') val |= (c - '0');
        else if (c >= 'a' && c <= 'f') val |= (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') val |= (c - 'A' + 10);
        else return false;
    }
    out_cp = val;
    return true;
}

// Returns error string or empty string if valid
static std::string validate_regex_group_name(const std::string& name) {
    if (name.empty()) return "SyntaxError: Empty named capture group name";
    size_t i = 0;
    bool first = true;
    while (i < name.size()) {
        unsigned char c = (unsigned char)name[i];
        if (c == '\\') {
            i++;
            if (i >= name.size() || name[i] != 'u')
                return "SyntaxError: Invalid escape sequence in named capture group";
            i++;
            uint32_t cp;
            if (!parse_regex_hex_escape(name, i, cp))
                return "SyntaxError: Invalid unicode escape in named capture group";
            if (cp > 0x10FFFF)
                return "SyntaxError: Unicode escape out of range in named capture group";
            if (cp >= 0xD800 && cp <= 0xDFFF)
                return "SyntaxError: Lone surrogate in named capture group";
            if (first && !is_unicode_id_start(cp))
                return "SyntaxError: Invalid identifier start in named capture group";
            if (!first && !is_unicode_id_continue(cp))
                return "SyntaxError: Invalid identifier character in named capture group";
        } else if (c < 0x80) {
            // ASCII
            if (first) {
                if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '$'))
                    return "SyntaxError: Invalid identifier start in named capture group";
            } else {
                if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '$'))
                    return "SyntaxError: Invalid identifier character in named capture group";
            }
            i++;
        } else {
            // non-ASCII: decode UTF-8
            size_t save = i;
            uint32_t cp = decode_utf8_cp(name, i);
            i++;
            (void)save;
            if (first && !is_unicode_id_start(cp))
                return "SyntaxError: Invalid identifier start in named capture group";
            if (!first && !is_unicode_id_continue(cp))
                return "SyntaxError: Invalid identifier character in named capture group";
        }
        first = false;
    }
    return "";
}

std::unique_ptr<ASTNode> Parser::parse_regex_literal() {
    const Token& token = current_token();
    Position start = token.get_start();
    Position end = token.get_end();
    std::string regex_str = token.get_value();
    
    if (!match(TokenType::REGEX)) {
        add_error("Expected regex literal");
        return nullptr;
    }
    
    advance();
    
    if (regex_str.length() < 2 || regex_str[0] != '/') {
        add_error("Invalid regex literal format");
        return nullptr;
    }
    
    size_t closing_slash = regex_str.find_last_of('/');
    if (closing_slash == 0 || closing_slash == std::string::npos) {
        add_error("Invalid regex literal: missing closing slash");
        return nullptr;
    }
    
    std::string pattern = regex_str.substr(1, closing_slash - 1);
    std::string flags = (closing_slash < regex_str.length() - 1) ?
                        regex_str.substr(closing_slash + 1) : "";

    // Validate flags: only valid flag chars, no duplicates, no unicode escapes
    {
        static const std::string valid_flags = "dgimsuyv";
        std::unordered_set<char> seen_flags;
        for (char fc : flags) {
            if (valid_flags.find(fc) == std::string::npos) {
                add_error("SyntaxError: Invalid regular expression flag: " + std::string(1, fc));
                return nullptr;
            }
            if (!seen_flags.insert(fc).second) {
                add_error("SyntaxError: Duplicate regular expression flag: " + std::string(1, fc));
                return nullptr;
            }
        }
        // u and v flags are mutually exclusive
        if (seen_flags.count('u') && seen_flags.count('v')) {
            add_error("SyntaxError: Regex flags 'u' and 'v' are mutually exclusive");
            return nullptr;
        }
    }

    // Validate named group backreferences: \k<name> must reference an existing (?<name>...)
    {
        std::unordered_set<std::string> named_groups;
        std::vector<std::string> backrefs;
        bool has_unicode_flag = flags.find('u') != std::string::npos || flags.find('v') != std::string::npos;
        const std::string& p = pattern;

        // Pre-scan: collect ALL named groups; detect same-alternative duplicates.
        // Track per-alternative seen-sets: push on '(', pop on ')', clear top on '|'.
        std::vector<std::unordered_set<std::string>> alt_stack;
        alt_stack.push_back({});
        bool esc = false;
        for (size_t i = 0; i < p.size(); i++) {
            if (esc) { esc = false; continue; }
            if (p[i] == '\\') { esc = true; continue; }
            if (p[i] == '(' && i + 3 < p.size() && p[i+1] == '?' && p[i+2] == '<'
                && p[i+3] != '=' && p[i+3] != '!') {
                size_t end = p.find('>', i + 3);
                if (end != std::string::npos) {
                    std::string name = p.substr(i + 3, end - (i + 3));
                    if (!alt_stack.empty() && alt_stack.back().count(name)) {
                        add_error("SyntaxError: Duplicate named capturing groups in the same alternative");
                        return nullptr;
                    }
                    if (!alt_stack.empty()) alt_stack.back().insert(name);
                    named_groups.insert(name);
                }
                alt_stack.push_back({});
            } else if (p[i] == '(') {
                alt_stack.push_back({});
            } else if (p[i] == ')') {
                if (!alt_stack.empty()) alt_stack.pop_back();
            } else if (p[i] == '|') {
                if (!alt_stack.empty()) alt_stack.back().clear();
            }
        }
        std::unordered_set<std::string> seen_groups; // kept for compat with loop below

        // Unicode-mode (/u or /v) strict validation
        if (has_unicode_flag) {
            bool has_v_flag = flags.find('v') != std::string::npos;

            // v-mode: ClassSetSyntaxCharacter and reserved double-punctuators are
            // SyntaxErrors when unescaped inside a character class (breaking change from /u).
            if (has_v_flag) {
                static const std::string reserved_dbl_chars = "!#$%*+,.:;<=>?@^`~";
                bool v_class_error = false;
                std::function<void(size_t, size_t)> check_class_body = [&](size_t content_start, size_t content_end) {
                    if (v_class_error) return;
                    if (content_end - content_start == 1 && p[content_start] == '-') {
                        add_error("SyntaxError: Unescaped '-' in character class in regexp /v");
                        v_class_error = true;
                        return;
                    }
                    for (size_t k = content_start; k < content_end; k++) {
                        char ch = p[k];
                        if (ch == '\\') {
                            char nxt = (k + 1 < content_end) ? p[k+1] : '\0';
                            if ((nxt == 'p' || nxt == 'P' || nxt == 'u' || nxt == 'q') &&
                                k + 2 < content_end && p[k+2] == '{') {
                                size_t close = p.find('}', k + 3);
                                k = (close == std::string::npos || close >= content_end) ? content_end - 1 : close;
                            } else {
                                k++;
                            }
                            continue;
                        }
                        if (ch == '[') {
                            size_t j = k + 1;
                            int depth = 1;
                            while (j < content_end && depth > 0) {
                                if (p[j] == '\\' && j + 1 < content_end) { j += 2; continue; }
                                if (p[j] == '[') { depth++; j++; continue; }
                                if (p[j] == ']') { depth--; j++; continue; }
                                j++;
                            }
                            check_class_body(k + 1, j > k + 1 ? j - 1 : j);
                            if (v_class_error) return;
                            k = (j > 0) ? j - 1 : j;
                            continue;
                        }
                        if (ch == '(' || ch == ')' || ch == '{' || ch == '}' ||
                            ch == '/' || ch == '|') {
                            add_error("SyntaxError: Unescaped syntax character in character class in regexp /v");
                            v_class_error = true;
                            return;
                        }
                        if (k + 1 < content_end && p[k+1] == ch &&
                            reserved_dbl_chars.find(ch) != std::string::npos) {
                            add_error("SyntaxError: Reserved double punctuator in character class in regexp /v");
                            v_class_error = true;
                            return;
                        }
                    }
                };
                size_t ci = 0;
                while (ci < p.size() && !v_class_error) {
                    if (p[ci] == '\\' && ci + 1 < p.size()) { ci += 2; continue; }
                    if (p[ci] != '[') { ci++; continue; }
                    size_t j = ci + 1;
                    int depth = 1;
                    bool negated = (j < p.size() && p[j] == '^');
                    size_t content_start = j + (negated ? 1 : 0);
                    while (j < p.size() && depth > 0) {
                        if (p[j] == '\\' && j + 1 < p.size()) { j += 2; continue; }
                        if (p[j] == '[') { depth++; j++; continue; }
                        if (p[j] == ']') { depth--; j++; continue; }
                        j++;
                    }
                    size_t content_end = (j > 0) ? j - 1 : j;
                    check_class_body(content_start, content_end);
                    ci = j;
                }
                if (v_class_error) return nullptr;
            }

            // Properties of strings (Unicode "binary string" properties): can only appear as
            // \p{Name} under /v, never \P{Name} and never inside a negated character class.
            static const std::unordered_set<std::string> string_properties = {
                "Basic_Emoji", "Emoji_Keycap_Sequence", "RGI_Emoji", "RGI_Emoji_Flag_Sequence",
                "RGI_Emoji_Modifier_Sequence", "RGI_Emoji_Tag_Sequence", "RGI_Emoji_ZWJ_Sequence"
            };
            static const std::unordered_set<std::string> unsupported_properties = {
                "Grapheme_Link", "Prepended_Concatenation_Mark"
            };
            bool in_class = false;
            bool class_negated = false;
            int group_count = 0;
            for (size_t i = 0; i < p.size(); i++) {
                if (p[i] == '[' && (i == 0 || p[i-1] != '\\') && !in_class) {
                    in_class = true;
                    class_negated = (i + 1 < p.size() && p[i+1] == '^');
                } else if (p[i] == ']' && (i == 0 || p[i-1] != '\\') && in_class) {
                    in_class = false;
                } else if (p[i] == '(' && (i == 0 || p[i-1] != '\\')) {
                    group_count++;
                } else if (p[i] == '\\' && i + 1 < p.size()) {
                    char next = p[i+1];
                    // \c must be followed by A-Z or a-z
                    if (next == 'c') {
                        if (i + 2 >= p.size() || !std::isalpha((unsigned char)p[i+2])) {
                            add_error("SyntaxError: Invalid Unicode escape \\c in regexp /u");
                            return nullptr;
                        }
                        i += 2;
                    // \8 and \9 are invalid in unicode mode
                    } else if (next == '8' || next == '9') {
                        add_error("SyntaxError: Invalid escape \\8/\\9 in regexp /u");
                        return nullptr;
                    // \1..\7 are decimal escapes -- valid only with a group
                    } else if (next >= '1' && next <= '7') {
                        i++;
                    // \q{...} string-literal escape: only valid under /v, only inside a class.
                    } else if (next == 'q' && has_v_flag) {
                        if (!in_class) {
                            add_error("SyntaxError: \\q is only valid inside a character class in regexp /v");
                            return nullptr;
                        }
                        i++;
                        if (i + 1 < p.size() && p[i+1] == '{') {
                            i++;
                            while (i + 1 < p.size() && p[i+1] != '}') i++;
                            if (i + 1 < p.size()) i++;
                        }
                    // Non-identity escapes: letters that aren't valid
                    } else if (std::isalpha((unsigned char)next) &&
                               next != 'b' && next != 'B' && next != 'd' && next != 'D' &&
                               next != 's' && next != 'S' && next != 'w' && next != 'W' &&
                               next != 'n' && next != 'r' && next != 't' && next != 'v' &&
                               next != 'f' && next != '0' && next != 'u' && next != 'x' &&
                               next != 'p' && next != 'P' && next != 'k' && next != 'c') {
                        add_error("SyntaxError: Invalid escape \\" + std::string(1, next) + " in regexp /u");
                        return nullptr;
                    } else if (next == 'u' || next == 'x') {
                        // Skip \uXXXX, \u{XXXX}, \xXX
                        i++;
                        if (next == 'u' && i + 1 < p.size() && p[i+1] == '{') {
                            i++;  // skip {
                            while (i + 1 < p.size() && p[i+1] != '}') i++;
                            if (i + 1 < p.size()) i++;  // skip }
                        } else {
                            size_t skip = (next == 'u') ? 4 : 2;
                            for (size_t s = 0; s < skip && i + 1 < p.size(); s++) i++;
                        }
                    } else if (next == 'p' || next == 'P') {
                        // Skip \p{Name} / \p{Name=Value} so its { isn't seen as a lone brace
                        bool negated_escape = (next == 'P');
                        i++;
                        if (i + 1 < p.size() && p[i+1] == '{') {
                            size_t name_start = i + 2;
                            i++;
                            while (i + 1 < p.size() && p[i+1] != '}') i++;
                            size_t name_end = i + 1;
                            if (i + 1 < p.size()) i++;
                            std::string content = p.substr(name_start, name_end - name_start);
                            if (content.find(' ') != std::string::npos) {
                                add_error("SyntaxError: Invalid property name (loose matching not allowed) in regexp");
                                return nullptr;
                            }
                            // Only the property NAME part matters for the string-property checks
                            // (Name=Value forms are binary/value properties, never string properties).
                            std::string name = content.substr(0, content.find('='));
                            if (unsupported_properties.count(name)) {
                                add_error("SyntaxError: Unsupported Unicode property " + name + " in regexp");
                                return nullptr;
                            }
                            if (string_properties.count(name)) {
                                if (!has_v_flag || negated_escape || (in_class && class_negated)) {
                                    add_error("SyntaxError: Property of strings " + name + " cannot be negated or used in /u mode");
                                    return nullptr;
                                }
                            }
                        }
                    } else {
                        i++;
                    }
                } else if (p[i] == '{' && (i == 0 || p[i-1] != '\\')) {
                    // Lone { not part of quantifier is invalid in unicode mode
                    // Check if it's a valid quantifier: {n}, {n,}, {n,m}
                    size_t j = i + 1;
                    while (j < p.size() && std::isdigit((unsigned char)p[j])) j++;
                    bool valid_quantifier = j > i + 1 && j < p.size() &&
                        (p[j] == '}' || (p[j] == ',' &&
                            (j + 1 < p.size() && (p[j+1] == '}' || std::isdigit((unsigned char)p[j+1])))));
                    if (!valid_quantifier) {
                        add_error("SyntaxError: Lone { not allowed in regexp /u");
                        return nullptr;
                    }
                } else if (p[i] == '}' && (i == 0 || p[i-1] != '\\')) {
                    // Lone } is invalid in unicode mode if not closing a valid quantifier
                    // (simplified: only flag if not following a digit or after {n,m})
                    if (i == 0 || p[i-1] != '{') {
                    }
                }
            }
        }
        for (size_t i = 0; i < p.size(); i++) {
            // Detect invalid group syntax: (?x where x is not :, =, !, <, or a modifier
            // group (?ims-ims:...) per the regexp-modifiers proposal (ES2025).
            if (p[i] == '(' && i + 1 < p.size() && p[i+1] == '?') {
                if (i + 2 < p.size()) {
                    char c3 = p[i+2];
                    if (c3 == 'i' || c3 == 'm' || c3 == 's' || c3 == '-') {
                        size_t j = i + 2;
                        std::string add_mods, remove_mods;
                        while (j < p.size() && (p[j] == 'i' || p[j] == 'm' || p[j] == 's')) add_mods += p[j++];
                        bool has_dash = j < p.size() && p[j] == '-';
                        if (has_dash) {
                            j++;
                            while (j < p.size() && (p[j] == 'i' || p[j] == 'm' || p[j] == 's')) remove_mods += p[j++];
                        }
                        auto has_dup = [](const std::string& s) {
                            return (s.find('i') != s.rfind('i')) || (s.find('m') != s.rfind('m')) || (s.find('s') != s.rfind('s'));
                        };
                        bool overlaps = add_mods.find_first_of(remove_mods) != std::string::npos;
                        if (j >= p.size() || p[j] != ':' || has_dup(add_mods) || has_dup(remove_mods) ||
                            overlaps || (has_dash && add_mods.empty() && remove_mods.empty())) {
                            add_error("SyntaxError: Invalid group syntax in regular expression");
                            return nullptr;
                        }
                    } else if (c3 != ':' && c3 != '=' && c3 != '!' && c3 != '<') {
                        add_error("SyntaxError: Invalid group syntax in regular expression");
                        return nullptr;
                    }
                }
            }
            if (p[i] == '(' && i + 2 < p.size() && p[i+1] == '?' && p[i+2] == '<') {
                // find '>' but not crossing a ')' (find end of group specifier)
                size_t end_pos = std::string::npos;
                for (size_t j = i + 3; j < p.size(); j++) {
                    if (p[j] == '>') { end_pos = j; break; }
                    if (p[j] == ')') break;
                }
                if (end_pos == std::string::npos) {
                    // not lookahead/lookbehind? check if it could be (?<= or (?<!
                    if (i + 3 < p.size() && (p[i+3] == '=' || p[i+3] == '!')) {
                        // it's lookbehind assertion, not a named group
                    } else {
                        add_error("SyntaxError: Unterminated named capture group");
                        return nullptr;
                    }
                } else {
                    std::string name = p.substr(i + 3, end_pos - (i + 3));
                    if (name.empty()) {
                        add_error("SyntaxError: Empty named capture group name");
                        return nullptr;
                    } else if (name[0] != '=' && name[0] != '!') {
                        std::string err = validate_regex_group_name(name);
                        if (!err.empty()) {
                            add_error(err);
                            return nullptr;
                        }
                        // ES2025: duplicate named groups in different alternatives are allowed.
                        seen_groups.insert(name);
                        named_groups.insert(name);
                    }
                }
            } else if (p[i] == '\\' && i + 1 < p.size() && p[i+1] == 'k') {
                if (i + 2 >= p.size() || p[i+2] != '<') {
                    // \k not followed by < → SyntaxError
                    add_error("SyntaxError: Invalid named backreference \\k in regular expression");
                    return nullptr;
                }
                size_t end_pos = p.find('>', i + 3);
                if (end_pos == std::string::npos) {
                    add_error("SyntaxError: Unterminated named backreference \\k< in regular expression");
                    return nullptr;
                }
                backrefs.push_back(p.substr(i + 3, end_pos - (i + 3)));
                i = end_pos;
            } else if (p[i] == '\\' && i + 1 < p.size()) {
                i++;
            }
        }
        for (const auto& ref : backrefs) {
            if (named_groups.find(ref) == named_groups.end()) {
                // In non-Unicode mode, \k<name> with no group is a legacy identity escape (not an error).
                if (has_unicode_flag) {
                    add_error("SyntaxError: Invalid named capture group reference: \\k<" + ref + ">");
                    return nullptr;
                }
            }
        }

        // Quantifier at start of pattern or after assertion
        auto is_quantifier_start = [](const std::string& s, size_t pos) -> bool {
            char c = s[pos];
            if (c == '?' || c == '*' || c == '+') return true;
            if (c == '{') {
                size_t j = pos + 1;
                while (j < s.size() && std::isdigit(s[j])) j++;
                if (j > pos + 1 && j < s.size() && (s[j] == '}' || s[j] == ',')) return true;
            }
            return false;
        };
        if (!p.empty() && is_quantifier_start(p, 0)) {
            add_error("SyntaxError: Quantifier without preceding atom in regular expression");
            return nullptr;
        }
        // Quantifier after lookbehind (?<=...) or (?<!...) or lookahead (?=...) or (?!...) in u mode
        for (size_t i = 0; i + 3 < p.size(); i++) {
            if (p[i] == '(' && p[i+1] == '?') {
                bool is_lookahead = (p[i+2] == '=' || p[i+2] == '!');
                bool is_lookbehind = (p[i+2] == '<' && i + 3 < p.size() && (p[i+3] == '=' || p[i+3] == '!'));
                if (is_lookahead || is_lookbehind) {
                    size_t depth = 1, j = i + 1;
                    while (j < p.size() && depth > 0) {
                        if (p[j] == '(' && (j == 0 || p[j-1] != '\\')) depth++;
                        else if (p[j] == ')' && (j == 0 || p[j-1] != '\\')) depth--;
                        j++;
                    }
                    if (j < p.size() && is_quantifier_start(p, j)) {
                        if (is_lookbehind || has_unicode_flag) {
                            add_error("SyntaxError: Quantifier cannot follow a lookahead/lookbehind assertion");
                            return nullptr;
                        }
                    }
                }
            }
        }
    }

    if (flags.find('u') != std::string::npos) {
        if (!RegExp::is_valid_unicode_pattern(pattern, flags)) {
            add_error("SyntaxError: Invalid regular expression: " + pattern);
            return nullptr;
        }
    }

    return std::make_unique<RegexLiteral>(pattern, flags, start, end);
}

std::unique_ptr<ASTNode> Parser::parse_boolean_literal() {
    const Token& token = current_token();
    bool value = (token.get_value() == "true");
    
    Position start = token.get_start();
    Position end = token.get_end();
    advance();
    
    return std::make_unique<BooleanLiteral>(value, start, end);
}

std::unique_ptr<ASTNode> Parser::parse_null_literal() {
    Position start = current_token().get_start();
    Position end = current_token().get_end();
    advance();
    
    return std::make_unique<NullLiteral>(start, end);
}

std::unique_ptr<ASTNode> Parser::parse_bigint_literal() {
    Position start = current_token().get_start();
    Position end = current_token().get_end();
    std::string value = current_token().get_value();
    advance();
    
    return std::make_unique<BigIntLiteral>(value, start, end);
}

std::unique_ptr<ASTNode> Parser::parse_undefined_literal() {
    Position start = current_token().get_start();
    Position end = current_token().get_end();
    advance();
    
    return std::make_unique<UndefinedLiteral>(start, end);
}

std::unique_ptr<ASTNode> Parser::parse_identifier() {
    const Token& token = current_token();
    std::string name = token.get_value();
    bool escaped_kw = token.has_escaped_keyword();
    Position start = token.get_start();
    Position end = token.get_end();

    if (escaped_kw) {
        // Always-reserved words can never appear as identifier references, even with escapes
        static const std::unordered_set<std::string> always_reserved_ids = {
            "false","true","null","this","super",
            "break","case","catch","class","const","continue","debugger",
            "default","delete","do","else","export","extends","finally",
            "for","function","if","import","in","instanceof","new",
            "return","switch","throw","try","typeof","var","void","while","with","enum"
        };
        if (always_reserved_ids.count(name)) {
            add_error("SyntaxError: '" + name + "' cannot contain unicode escape sequences");
            return nullptr;
        }
        if ((name == "await" && (options_.in_async_body || options_.source_type_module)) ||
            (name == "yield" && options_.in_generator_body)) {
            add_error("SyntaxError: '" + name + "' cannot contain unicode escape sequences in this context");
            return nullptr;
        }
        if (options_.strict_mode) {
            static const std::unordered_set<std::string> strict_future = {
                "implements","interface","let","package","private","protected","public","static","yield"
            };
            if (strict_future.count(name)) {
                add_error("SyntaxError: '" + name + "' cannot contain unicode escape sequences in strict mode");
                return nullptr;
            }
        }
        // escaped `async` followed by `function` on same line = SyntaxError
        if (name == "async") {
            size_t async_end_line = token.get_end().line;
            if (peek_token().get_type() == TokenType::FUNCTION &&
                peek_token().get_start().line == async_end_line) {
                add_error("SyntaxError: `async` cannot contain unicode escape sequences");
                return nullptr;
            }
        }
    }

    if (options_.in_class_field_init && name == "arguments") {
        add_error("SyntaxError: 'arguments' is not valid in class field initializer");
        return nullptr;
    }

    if (options_.in_class_static_block && name == "arguments") {
        add_error("SyntaxError: 'arguments' is not valid in class static block");
        return nullptr;
    }

    advance();
    auto id = std::make_unique<Identifier>(name, start, end);
    if (escaped_kw) id->set_escaped_keyword(true);
    return id;
}

std::unique_ptr<ASTNode> Parser::parse_private_field() {
    Position start = get_current_position();
    size_t hash_start_offset = current_token().get_start().offset;

    if (!consume(TokenType::HASH)) {
        add_error("Expected '#'");
        return nullptr;
    }

    if (!match(TokenType::IDENTIFIER) && !is_keyword_token(current_token().get_type())) {
        add_error("Expected identifier after '#'");
        return nullptr;
    }

    // No whitespace allowed between # and name (single char token: end == start)
    if (current_token().get_start().offset != hash_start_offset + 1) {
        add_error("SyntaxError: Whitespace is not allowed between '#' and identifier");
        return nullptr;
    }

    if (options_.class_depth == 0 && !options_.in_eval_context) {
        add_error("SyntaxError: Private names are not allowed outside class bodies");
        return nullptr;
    }

    const Token& token = current_token();
    std::string name = "#" + token.get_value();
    Position end = token.get_end();
    advance();

    return std::make_unique<Identifier>(name, start, end);
}

std::unique_ptr<ASTNode> Parser::parse_parenthesized_expression() {
    if (!consume(TokenType::LEFT_PAREN)) {
        add_error("Expected '('");
        return nullptr;
    }

    // Parentheses start a new AssignmentExpression context -- yield/await valid inside
    bool saved_unary = options_.in_unary_operand;
    bool saved_binary = options_.in_binary_expr;
    options_.in_unary_operand = false;
    options_.in_binary_expr = false;
    auto expr = parse_expression();
    options_.in_unary_operand = saved_unary;
    options_.in_binary_expr = saved_binary;
    if (!expr) {
        add_error("Expected expression inside parentheses");
        return nullptr;
    }

    if (!consume(TokenType::RIGHT_PAREN)) {
        add_error("Expected ')' after expression");
        return expr;
    }

    last_expr_was_parenthesized_ = true;
    return expr;
}


std::unique_ptr<ASTNode> Parser::parse_binary_expression(
    std::function<std::unique_ptr<ASTNode>()> parse_operand,
    const std::vector<TokenType>& operators) {
    
    auto left = parse_operand();
    if (!left) return nullptr;
    
    while (match_any(operators)) {
        TokenType op_token = current_token().get_type();
        Position op_start = current_token().get_start();
        advance();

        bool saved_bin = options_.in_binary_expr;
        options_.in_binary_expr = true;
        auto right = parse_operand();
        options_.in_binary_expr = saved_bin;
        if (!right) {
            add_error("Expected expression after binary operator");
            return left;
        }
        
        BinaryExpression::Operator op = token_to_binary_operator(op_token);
        Position end = right->get_end();
        
        left = std::make_unique<BinaryExpression>(
            std::move(left), op, std::move(right), op_start, end
        );
    }
    
    return left;
}

BinaryExpression::Operator Parser::token_to_binary_operator(TokenType type) {
    return BinaryExpression::token_type_to_operator(type);
}

UnaryExpression::Operator Parser::token_to_unary_operator(TokenType type) {
    switch (type) {
        case TokenType::PLUS: return UnaryExpression::Operator::PLUS;
        case TokenType::MINUS: return UnaryExpression::Operator::MINUS;
        case TokenType::LOGICAL_NOT: return UnaryExpression::Operator::LOGICAL_NOT;
        case TokenType::BITWISE_NOT: return UnaryExpression::Operator::BITWISE_NOT;
        case TokenType::TYPEOF: return UnaryExpression::Operator::TYPEOF;
        case TokenType::VOID: return UnaryExpression::Operator::VOID;
        case TokenType::DELETE: return UnaryExpression::Operator::DELETE;
        case TokenType::INCREMENT: return UnaryExpression::Operator::PRE_INCREMENT;
        case TokenType::DECREMENT: return UnaryExpression::Operator::PRE_DECREMENT;
        default: return UnaryExpression::Operator::PLUS;
    }
}


const Token& Parser::current_token() const {
    return tokens_[current_token_index_];
}

const Token& Parser::peek_token(size_t offset) const {
    return tokens_[current_token_index_ + offset];
}

const Token& Parser::previous_token() const {
    if (current_token_index_ > 0) {
        return tokens_[current_token_index_ - 1];
    }
    return tokens_[0];
}

void Parser::advance() {
    if (current_token_index_ < tokens_.size() - 1) {
        current_token_index_++;
        while (current_token_index_ < tokens_.size() && 
               (current_token().get_type() == TokenType::NEWLINE || 
                current_token().get_type() == TokenType::WHITESPACE ||
                current_token().get_type() == TokenType::COMMENT)) {
            if (current_token_index_ < tokens_.size() - 1) {
                current_token_index_++;
            } else {
                break;
            }
        }
    }
}

bool Parser::match(TokenType type) {
    return current_token().get_type() == type;
}

bool Parser::match_any(const std::vector<TokenType>& types) {
    TokenType current_type = current_token().get_type();
    return std::find(types.begin(), types.end(), current_type) != types.end();
}

bool Parser::consume(TokenType type) {
    if (match(type)) {
        advance();
        return true;
    }
    return false;
}

bool Parser::consume_if_match(TokenType type) {
    if (match(type)) {
        advance();
        return true;
    }
    return false;
}

bool Parser::is_reserved_word_as_property_name() {
    TokenType type = current_token().get_type();
    return type == TokenType::RETURN ||
           type == TokenType::IF ||
           type == TokenType::ELSE ||
           type == TokenType::FOR ||
           type == TokenType::WHILE ||
           type == TokenType::DO ||
           type == TokenType::BREAK ||
           type == TokenType::CONTINUE ||
           type == TokenType::FUNCTION ||
           type == TokenType::VAR ||
           type == TokenType::LET ||
           type == TokenType::CONST ||
           type == TokenType::CLASS ||
           type == TokenType::EXTENDS ||
           type == TokenType::TRY ||
           type == TokenType::CATCH ||
           type == TokenType::FINALLY ||
           type == TokenType::THROW ||
           type == TokenType::DELETE ||
           type == TokenType::TYPEOF ||
           type == TokenType::INSTANCEOF ||
           type == TokenType::IN ||
           type == TokenType::NEW ||
           type == TokenType::THIS ||
           type == TokenType::SUPER ||
           type == TokenType::DEFAULT ||
           type == TokenType::SWITCH ||
           type == TokenType::CASE ||
           type == TokenType::IMPORT ||
           type == TokenType::EXPORT ||
           type == TokenType::FROM ||
           type == TokenType::ASYNC ||
           type == TokenType::AWAIT ||
           type == TokenType::YIELD ||
           type == TokenType::ENUM  ||
           type == TokenType::STATIC;
}

bool Parser::at_end() const {
    return current_token().get_type() == TokenType::EOF_TOKEN;
}

Position Parser::get_current_position() const {
    return current_token().get_start();
}


void Parser::add_error(const std::string& message) {
    errors_.emplace_back(message, get_current_position());
}

void Parser::add_error(const std::string& message, const Position& position) {
    errors_.emplace_back(message, position);
}

void Parser::skip_to_statement_boundary() {
    while (!at_end() && !match(TokenType::SEMICOLON) && !match(TokenType::RIGHT_BRACE)) {
        advance();
    }
    if (match(TokenType::SEMICOLON)) {
        advance();
    }
}

void Parser::skip_to(TokenType type) {
    while (!at_end() && !match(type)) {
        advance();
    }
}

void Parser::skip_decorator_list() {
    // Consume zero or more decorators: @ DecoratorExpr
    while (!at_end() && current_token().get_type() == TokenType::AT) {
        advance(); // consume '@'
        // Now consume the DecoratorMemberExpression / DecoratorCallExpression / ParenthesizedExpr
        if (current_token().get_type() == TokenType::LEFT_PAREN) {
            // @(expr) -- parenthesized decorator
            int depth = 0;
            do {
                if (current_token().get_type() == TokenType::LEFT_PAREN) depth++;
                else if (current_token().get_type() == TokenType::RIGHT_PAREN) depth--;
                advance();
            } while (!at_end() && depth > 0);
        } else {
            // identifier or keyword-as-identifier, then optional .name chains
            // Accept any identifier-like token (IDENTIFIER, keyword tokens used as names)
            if (current_token().get_type() != TokenType::IDENTIFIER &&
                !is_reserved_word_as_property_name() &&
                current_token().get_type() != TokenType::YIELD &&
                current_token().get_type() != TokenType::AWAIT) {
                // malformed decorator -- stop consuming
                break;
            }
            advance(); // consume identifier
            // consume optional .name chains
            while (!at_end() && current_token().get_type() == TokenType::DOT) {
                advance(); // consume '.'
                if (!at_end()) advance(); // consume name
            }
            // optional argument list (makes it a call expression)
            if (!at_end() && current_token().get_type() == TokenType::LEFT_PAREN) {
                int depth = 0;
                do {
                    if (current_token().get_type() == TokenType::LEFT_PAREN) depth++;
                    else if (current_token().get_type() == TokenType::RIGHT_PAREN) depth--;
                    advance();
                } while (!at_end() && depth > 0);
            }
        }
    }
}

bool Parser::is_assignment_operator(TokenType type) const {
    return type == TokenType::ASSIGN ||
           type == TokenType::PLUS_ASSIGN ||
           type == TokenType::MINUS_ASSIGN ||
           type == TokenType::MULTIPLY_ASSIGN ||
           type == TokenType::DIVIDE_ASSIGN ||
           type == TokenType::MODULO_ASSIGN ||
           type == TokenType::BITWISE_AND_ASSIGN ||
           type == TokenType::BITWISE_OR_ASSIGN ||
           type == TokenType::BITWISE_XOR_ASSIGN ||
           type == TokenType::LEFT_SHIFT_ASSIGN ||
           type == TokenType::RIGHT_SHIFT_ASSIGN ||
           type == TokenType::UNSIGNED_RIGHT_SHIFT_ASSIGN ||
           type == TokenType::LOGICAL_OR_ASSIGN ||
           type == TokenType::LOGICAL_AND_ASSIGN ||
           type == TokenType::NULLISH_ASSIGN ||
           type == TokenType::EXPONENT_ASSIGN;
}

bool Parser::is_binary_operator(TokenType type) const {
    return type == TokenType::PLUS ||
           type == TokenType::MINUS ||
           type == TokenType::MULTIPLY ||
           type == TokenType::DIVIDE ||
           type == TokenType::MODULO ||
           type == TokenType::EXPONENT ||
           type == TokenType::EQUAL ||
           type == TokenType::NOT_EQUAL ||
           type == TokenType::STRICT_EQUAL ||
           type == TokenType::STRICT_NOT_EQUAL ||
           type == TokenType::LESS_THAN ||
           type == TokenType::GREATER_THAN ||
           type == TokenType::LESS_EQUAL ||
           type == TokenType::GREATER_EQUAL ||
           type == TokenType::LOGICAL_AND ||
           type == TokenType::LOGICAL_OR ||
           type == TokenType::BITWISE_AND ||
           type == TokenType::BITWISE_OR ||
           type == TokenType::BITWISE_XOR ||
           type == TokenType::LEFT_SHIFT ||
           type == TokenType::RIGHT_SHIFT ||
           type == TokenType::UNSIGNED_RIGHT_SHIFT;
}

bool Parser::is_unary_operator(TokenType type) const {
    return type == TokenType::PLUS ||
           type == TokenType::MINUS ||
           type == TokenType::LOGICAL_NOT ||
           type == TokenType::BITWISE_NOT ||
           type == TokenType::TYPEOF ||
           type == TokenType::VOID ||
           type == TokenType::DELETE ||
           type == TokenType::INCREMENT ||
           type == TokenType::DECREMENT;
}

bool Parser::is_keyword_token(TokenType type) const {
    return type == TokenType::BREAK ||
           type == TokenType::CASE ||
           type == TokenType::CATCH ||
           type == TokenType::CLASS ||
           type == TokenType::CONST ||
           type == TokenType::CONTINUE ||
           type == TokenType::DEBUGGER ||
           type == TokenType::DEFAULT ||
           type == TokenType::DELETE ||
           type == TokenType::DO ||
           type == TokenType::ELSE ||
           type == TokenType::EXPORT ||
           type == TokenType::EXTENDS ||
           type == TokenType::FINALLY ||
           type == TokenType::FOR ||
           type == TokenType::FUNCTION ||
           type == TokenType::IF ||
           type == TokenType::IMPORT ||
           type == TokenType::IN ||
           type == TokenType::INSTANCEOF ||
           type == TokenType::LET ||
           type == TokenType::NEW ||
           type == TokenType::RETURN ||
           type == TokenType::SUPER ||
           type == TokenType::SWITCH ||
           type == TokenType::THIS ||
           type == TokenType::THROW ||
           type == TokenType::TRY ||
           type == TokenType::TYPEOF ||
           type == TokenType::VAR ||
           type == TokenType::VOID ||
           type == TokenType::WHILE ||
           type == TokenType::WITH ||
           type == TokenType::YIELD ||
           type == TokenType::ASYNC ||
           type == TokenType::AWAIT ||
           type == TokenType::FROM ||
           type == TokenType::OF ||
           type == TokenType::STATIC ||
           type == TokenType::ENUM ||
           type == TokenType::UNDEFINED ||
           type == TokenType::NULL_LITERAL ||
           type == TokenType::BOOLEAN;
}

std::string Parser::find_forbidden_expr_in_params(
    const std::vector<std::unique_ptr<Parameter>>& params, bool check_yield, bool check_await) const {
    using T = ASTNode::Type;
    std::string found;

    std::function<void(const ASTNode*)> walk = [&](const ASTNode* nd) {
        if (!nd || !found.empty()) return;
        switch (nd->get_type()) {
            case T::YIELD_EXPRESSION:
                if (check_yield) { found = "yield"; return; }
                walk(static_cast<const YieldExpression*>(nd)->get_argument());
                break;
            case T::AWAIT_EXPRESSION:
                if (check_await) { found = "await"; return; }
                walk(static_cast<const AwaitExpression*>(nd)->get_argument());
                break;
            // Nested functions/classes/methods introduce their own [Yield]/[Await]
            // grammar parameterization -- do not cross into their scopes.
            case T::FUNCTION_EXPRESSION:
            case T::FUNCTION_DECLARATION:
            case T::ASYNC_FUNCTION_EXPRESSION:
            case T::ARROW_FUNCTION_EXPRESSION:
            case T::CLASS_DECLARATION:
            case T::METHOD_DEFINITION:
                break;
            case T::BINARY_EXPRESSION: { auto* be = static_cast<const BinaryExpression*>(nd); walk(be->get_left()); walk(be->get_right()); break; }
            case T::NULLISH_COALESCING_EXPRESSION: { auto* nc = static_cast<const NullishCoalescingExpression*>(nd); walk(nc->get_left()); walk(nc->get_right()); break; }
            case T::UNARY_EXPRESSION: walk(static_cast<const UnaryExpression*>(nd)->get_operand()); break;
            case T::ASSIGNMENT_EXPRESSION: { auto* ae = static_cast<const AssignmentExpression*>(nd); walk(ae->get_left()); walk(ae->get_right()); break; }
            case T::CONDITIONAL_EXPRESSION: { auto* ce = static_cast<const ConditionalExpression*>(nd); walk(ce->get_test()); walk(ce->get_consequent()); walk(ce->get_alternate()); break; }
            case T::CALL_EXPRESSION: { auto* ce = static_cast<const CallExpression*>(nd); walk(ce->get_callee()); for (const auto& a : ce->get_arguments()) walk(a.get()); break; }
            case T::NEW_EXPRESSION: { auto* ne = static_cast<const NewExpression*>(nd); walk(ne->get_constructor()); for (const auto& a : ne->get_arguments()) walk(a.get()); break; }
            case T::MEMBER_EXPRESSION: { auto* me = static_cast<const MemberExpression*>(nd); walk(me->get_object()); if (me->is_computed()) walk(me->get_property()); break; }
            case T::OPTIONAL_CHAINING_EXPRESSION: { auto* oc = static_cast<const OptionalChainingExpression*>(nd); walk(oc->get_object()); if (oc->is_computed()) walk(oc->get_property()); break; }
            case T::SPREAD_ELEMENT: walk(static_cast<const SpreadElement*>(nd)->get_argument()); break;
            case T::ARRAY_LITERAL: for (const auto& e : static_cast<const ArrayLiteral*>(nd)->get_elements()) walk(e.get()); break;
            case T::OBJECT_LITERAL: for (const auto& pr : static_cast<const ObjectLiteral*>(nd)->get_properties()) { if (pr->computed) walk(pr->key.get()); walk(pr->value.get()); } break;
            case T::TEMPLATE_LITERAL:
                for (const auto& el : static_cast<const TemplateLiteral*>(nd)->get_elements())
                    if (el.type == TemplateLiteral::Element::Type::EXPRESSION) walk(el.expression.get());
                break;
            default: break;
        }
    };

    for (const auto& p : params) {
        if (!found.empty()) break;
        if (p->has_default()) walk(p->get_default_value());
        if (!found.empty()) break;
        if (p->has_destructuring()) {
            ASTNode* pat = p->get_destructuring_pattern();
            if (pat && pat->get_type() == T::DESTRUCTURING_ASSIGNMENT) {
                auto* da = static_cast<DestructuringAssignment*>(pat);
                for (const auto& dv : da->get_default_values()) { walk(dv.expr.get()); if (!found.empty()) break; }
                if (found.empty()) {
                    for (const auto& pm : da->get_property_mappings()) { walk(pm.computed_key.get()); if (!found.empty()) break; }
                }
            }
        }
    }
    return found;
}

bool Parser::is_valid_assignment_target(ASTNode* node) const {
    if (!node) return false;

    switch (node->get_type()) {
        case ASTNode::Type::IDENTIFIER: {
            auto* id = static_cast<const Identifier*>(node);
            if (id->get_name() == "this") return false;
            if (id->has_escaped_keyword()) return false;
            return true;
        }
        case ASTNode::Type::MEMBER_EXPRESSION:
            return true;
        case ASTNode::Type::CALL_EXPRESSION:
            return false;
        case ASTNode::Type::META_PROPERTY:
            return false;
        case ASTNode::Type::YIELD_EXPRESSION:
            return false;
        case ASTNode::Type::OBJECT_LITERAL: {
            // Any ObjectLiteral is a valid assignment target via the cover grammar (destructuring).
            // The parenthesized case ({}) = x is handled at the call site.
            return true;
        }
        case ASTNode::Type::ARRAY_LITERAL:
            return true;
        case ASTNode::Type::STRING_LITERAL:
        case ASTNode::Type::NUMBER_LITERAL:
        case ASTNode::Type::BOOLEAN_LITERAL:
        case ASTNode::Type::NULL_LITERAL:
        case ASTNode::Type::FUNCTION_EXPRESSION:
        case ASTNode::Type::ARROW_FUNCTION_EXPRESSION:
        case ASTNode::Type::ASYNC_FUNCTION_EXPRESSION:
        case ASTNode::Type::BINARY_EXPRESSION:
        case ASTNode::Type::UNARY_EXPRESSION:
        case ASTNode::Type::CONDITIONAL_EXPRESSION:
            return false;
        default:
            return false;
    }
}


std::unique_ptr<ASTNode> Parser::parse_using_declaration(bool is_await, bool consume_semicolon) {
    Position start = current_token().get_start();

    if (is_await) {
        // consume 'await'
        advance();
    }
    // consume 'using'
    advance();

    std::vector<UsingBinding> bindings;

    while (true) {
        // using/await using only accept BindingIdentifier, not destructuring patterns
        if (current_token().get_type() == TokenType::LEFT_BRACKET ||
            current_token().get_type() == TokenType::LEFT_BRACE) {
            add_error("SyntaxError: 'using' declarations do not support destructuring patterns");
            return nullptr;
        }
        bool is_await_id = current_token().get_type() == TokenType::AWAIT
                           && !options_.in_async_body && !options_.in_class_static_block;
        if (current_token().get_type() != TokenType::IDENTIFIER &&
            current_token().get_type() != TokenType::LET &&
            current_token().get_type() != TokenType::STATIC &&
            current_token().get_type() != TokenType::OF &&
            !is_await_id) {
            add_error("Expected identifier in 'using' declaration");
            return nullptr;
        }

        std::string name = current_token().get_value();
        advance();

        if (current_token().get_type() != TokenType::ASSIGN) {
            add_error("'using' declaration must have an initializer");
            return nullptr;
        }
        advance();

        auto init = parse_assignment_expression();
        if (!init) return nullptr;

        bindings.emplace_back(name, std::move(init));

        if (current_token().get_type() != TokenType::COMMA) break;
        advance();
    }

    Position end = get_current_position();
    if (consume_semicolon && current_token().get_type() == TokenType::SEMICOLON) {
        end = current_token().get_end();
        advance();
    }

    return std::make_unique<UsingDeclaration>(std::move(bindings), is_await, start, end);
}

std::unique_ptr<ASTNode> Parser::parse_variable_declaration() {
    return parse_variable_declaration(true);
}

std::unique_ptr<ASTNode> Parser::parse_variable_declaration(bool consume_semicolon) {
    Position start = get_current_position();
    
    TokenType kind_token = current_token().get_type();
    VariableDeclarator::Kind kind;
    switch (kind_token) {
        case TokenType::VAR: kind = VariableDeclarator::Kind::VAR; break;
        case TokenType::LET: kind = VariableDeclarator::Kind::LET; break;
        case TokenType::CONST: kind = VariableDeclarator::Kind::CONST; break;
        default:
            add_error("Expected variable declaration keyword");
            return nullptr;
    }
    advance();
    
    std::vector<std::unique_ptr<VariableDeclarator>> declarations;
    
    do {
        if (current_token().get_type() == TokenType::LEFT_BRACKET || 
            current_token().get_type() == TokenType::LEFT_BRACE) {
            auto destructuring = parse_destructuring_pattern();
            if (!destructuring) {
                add_error("Invalid destructuring pattern");
                return nullptr;
            }
            
            if (!consume_if_match(TokenType::ASSIGN)) {
                add_error("Destructuring declaration must have an initializer");
                return nullptr;
            }
            
            auto init = parse_assignment_expression();
            if (!init) {
                add_error("Expected expression after '=' in destructuring declaration");
                return nullptr;
            }
            
            DestructuringAssignment* dest = static_cast<DestructuringAssignment*>(destructuring.get());
            dest->set_source(std::move(init));
            
            auto declarator = std::make_unique<VariableDeclarator>(
                std::make_unique<Identifier>("", start, start),
                std::move(destructuring),
                kind,
                start,
                get_current_position()
            );
            
            declarations.push_back(std::move(declarator));
            break;
        }
        
        // Non-strict: yield/await/let/static can be identifiers outside strict/generator/async
        bool is_yield_id = (current_token().get_type() == TokenType::YIELD &&
                            !options_.strict_mode && !options_.in_generator_body);
        bool is_await_id = (current_token().get_type() == TokenType::AWAIT &&
                            !options_.in_async_body && !options_.in_class_static_block &&
                            !options_.source_type_module);
        bool is_let_id   = (current_token().get_type() == TokenType::LET && !options_.strict_mode);
        bool is_of_id      = (current_token().get_type() == TokenType::OF);
        bool is_async_id   = (current_token().get_type() == TokenType::ASYNC);
        bool is_undef_id   = (current_token().get_type() == TokenType::UNDEFINED);
        bool is_static_id  = (current_token().get_type() == TokenType::STATIC && !options_.strict_mode);
        bool is_from_id    = (current_token().get_type() == TokenType::FROM);
        if (current_token().get_type() != TokenType::IDENTIFIER && !is_yield_id && !is_await_id && !is_let_id && !is_of_id && !is_async_id && !is_undef_id && !is_static_id && !is_from_id) {
            add_error("Expected identifier in variable declaration");
            return nullptr;
        }

        if (current_token().has_escaped_keyword()) {
            static const std::unordered_set<std::string> always_reserved_binding = {
                "false","true","null","this","super",
                "break","case","catch","class","const","continue","debugger",
                "default","delete","do","else","export","extends","finally",
                "for","function","if","import","in","instanceof","new",
                "return","switch","throw","try","typeof","var","void","while","with","enum"
            };
            static const std::unordered_set<std::string> strict_reserved_binding = {
                "implements","interface","let","package","private","protected","public","static","yield"
            };
            const std::string& ek_name = current_token().get_value();
            if (always_reserved_binding.count(ek_name) ||
                (options_.strict_mode && strict_reserved_binding.count(ek_name))) {
                add_error("Keywords cannot be used as identifiers via unicode escape sequences");
                return nullptr;
            }
        }

        const std::string& var_name_check = current_token().get_value();

        // 'let' cannot be a binding name in let/const declarations.
        if ((kind == VariableDeclarator::Kind::LET || kind == VariableDeclarator::Kind::CONST) &&
            var_name_check == "let") {
            add_error("SyntaxError: 'let' is not a valid binding name in 'let'/'const' declarations");
            return nullptr;
        }
        // 'yield' cannot be a binding in generator bodies or strict mode.
        if (var_name_check == "yield" &&
            (options_.in_generator_body || options_.strict_mode)) {
            add_error("SyntaxError: 'yield' cannot be used as a binding name here");
            return nullptr;
        }
        // 'await' cannot be a binding in async bodies, module code, or class static blocks.
        if (var_name_check == "await" &&
            (options_.in_async_body || options_.source_type_module ||
             options_.in_class_static_block)) {
            add_error("SyntaxError: 'await' cannot be used as a binding name here");
            return nullptr;
        }

        // ES5: eval and arguments cannot be used as variable names in strict mode
        if (options_.strict_mode) {
            if (var_name_check == "eval" || var_name_check == "arguments") {
                add_error("'" + var_name_check + "' cannot be used as a variable name in strict mode");
                return nullptr;
            }
            static const std::unordered_set<std::string> strict_future_reserved = {
                "implements","interface","package","private","protected","public"
            };
            if (strict_future_reserved.count(var_name_check)) {
                add_error("SyntaxError: '" + var_name_check + "' is a reserved word in strict mode");
                return nullptr;
            }
        }

        auto id = std::make_unique<Identifier>(current_token().get_value(),
                                             current_token().get_start(), current_token().get_end());
        advance();
        
        std::unique_ptr<ASTNode> init = nullptr;
        if (consume_if_match(TokenType::ASSIGN)) {
            init = parse_assignment_expression();
            if (!init) {
                add_error("Expected expression after '=' in variable declaration");
                return nullptr;
            }
        } else if (kind == VariableDeclarator::Kind::CONST) {
            add_error("const declarations must have an initializer");
            return nullptr;
        }
        
        Position decl_start = id->get_start();
        Position decl_end = init ? init->get_end() : id->get_end();
        auto declarator = std::make_unique<VariableDeclarator>(
            std::move(id), std::move(init), kind, decl_start, decl_end
        );
        declarations.push_back(std::move(declarator));
        
    } while (consume_if_match(TokenType::COMMA));

    if (consume_semicolon) {
        if (!consume_if_match(TokenType::SEMICOLON)) {
            TokenType cur = current_token().get_type();
            if (cur != TokenType::RIGHT_BRACE && cur != TokenType::EOF_TOKEN) {
                // ASI only applies when a line terminator precedes the current token.
                bool has_newline = false;
                size_t i = current_token_index_;
                while (i > 0) {
                    i--;
                    TokenType t = tokens_[i].get_type();
                    if (t == TokenType::NEWLINE) { has_newline = true; break; }
                    if (t != TokenType::WHITESPACE && t != TokenType::COMMENT) break;
                }
                if (!has_newline) {
                    add_error("SyntaxError: unexpected token after variable declaration");
                    return nullptr;
                }
            }
        }
    }

    Position end = get_current_position();
    return std::make_unique<VariableDeclaration>(std::move(declarations), kind, start, end);
}

static void collect_var_declared_names(ASTNode* node, std::vector<std::string>& vars) {
    if (!node) return;
    auto t = node->get_type();
    // stop at function/class boundaries
    if (t == ASTNode::Type::FUNCTION_DECLARATION ||
        t == ASTNode::Type::FUNCTION_EXPRESSION ||
        t == ASTNode::Type::ARROW_FUNCTION_EXPRESSION ||
        t == ASTNode::Type::ASYNC_FUNCTION_EXPRESSION ||
        t == ASTNode::Type::CLASS_DECLARATION ||
        t == ASTNode::Type::CLASS_STATIC_BLOCK) return;
    if (t == ASTNode::Type::VARIABLE_DECLARATION) {
        auto* vd = static_cast<VariableDeclaration*>(node);
        if (vd->get_kind() == VariableDeclarator::Kind::VAR) {
            for (const auto& d : vd->get_declarations())
                if (d->get_id()) vars.push_back(d->get_id()->get_name());
        }
        return;
    }
    if (t == ASTNode::Type::BLOCK_STATEMENT) {
        for (const auto& s : static_cast<BlockStatement*>(node)->get_statements())
            collect_var_declared_names(s.get(), vars);
        return;
    }
    if (t == ASTNode::Type::IF_STATEMENT) {
        auto* n = static_cast<IfStatement*>(node);
        collect_var_declared_names(n->get_consequent(), vars);
        collect_var_declared_names(n->get_alternate(), vars);
        return;
    }
    if (t == ASTNode::Type::FOR_STATEMENT) {
        auto* n = static_cast<ForStatement*>(node);
        collect_var_declared_names(n->get_init(), vars);
        collect_var_declared_names(n->get_body(), vars);
        return;
    }
    if (t == ASTNode::Type::FOR_IN_STATEMENT) {
        auto* n = static_cast<ForInStatement*>(node);
        collect_var_declared_names(n->get_body(), vars);
        return;
    }
    if (t == ASTNode::Type::FOR_OF_STATEMENT) {
        auto* n = static_cast<ForOfStatement*>(node);
        collect_var_declared_names(n->get_body(), vars);
        return;
    }
    if (t == ASTNode::Type::WHILE_STATEMENT) {
        collect_var_declared_names(static_cast<WhileStatement*>(node)->get_body(), vars);
        return;
    }
    if (t == ASTNode::Type::DO_WHILE_STATEMENT) {
        collect_var_declared_names(static_cast<DoWhileStatement*>(node)->get_body(), vars);
        return;
    }
    if (t == ASTNode::Type::WITH_STATEMENT) {
        collect_var_declared_names(static_cast<WithStatement*>(node)->get_body(), vars);
        return;
    }
    if (t == ASTNode::Type::LABELED_STATEMENT) {
        collect_var_declared_names(static_cast<LabeledStatement*>(node)->get_statement(), vars);
        return;
    }
    if (t == ASTNode::Type::TRY_STATEMENT) {
        auto* n = static_cast<TryStatement*>(node);
        collect_var_declared_names(n->get_try_block(), vars);
        if (n->get_catch_clause()) collect_var_declared_names(static_cast<CatchClause*>(n->get_catch_clause())->get_body(), vars);
        collect_var_declared_names(n->get_finally_block(), vars);
        return;
    }
    if (t == ASTNode::Type::SWITCH_STATEMENT) {
        for (const auto& c : static_cast<SwitchStatement*>(node)->get_cases()) {
            auto* cc = static_cast<CaseClause*>(c.get());
            for (const auto& s : cc->get_consequent())
                collect_var_declared_names(s.get(), vars);
        }
        return;
    }
}

static std::string find_block_lexical_duplicate(const std::vector<std::unique_ptr<ASTNode>>& stmts) {
    std::vector<std::string> lexical;
    std::vector<std::string> vars;

    for (const auto& stmt : stmts) {
        if (!stmt) continue;
        if (stmt->get_type() == ASTNode::Type::FUNCTION_DECLARATION) {
            auto* fn = static_cast<FunctionDeclaration*>(stmt.get());
            if (fn->get_id()) lexical.push_back(fn->get_id()->get_name());
        } else if (stmt->get_type() == ASTNode::Type::CLASS_DECLARATION) {
            auto* cls = static_cast<ClassDeclaration*>(stmt.get());
            if (cls->get_id()) lexical.push_back(cls->get_id()->get_name());
        } else if (stmt->get_type() == ASTNode::Type::VARIABLE_DECLARATION) {
            auto* vd = static_cast<VariableDeclaration*>(stmt.get());
            bool is_var = vd->get_kind() == VariableDeclarator::Kind::VAR;
            for (const auto& decl : vd->get_declarations()) {
                if (decl->get_id()) {
                    if (is_var) vars.push_back(decl->get_id()->get_name());
                    else lexical.push_back(decl->get_id()->get_name());
                }
            }
        } else if (stmt->get_type() == ASTNode::Type::USING_DECLARATION) {
            auto* ud = static_cast<UsingDeclaration*>(stmt.get());
            for (const auto& b : ud->get_bindings()) lexical.push_back(b.name);
        } else {
            collect_var_declared_names(stmt.get(), vars);
        }
    }

    for (size_t i = 0; i < lexical.size(); i++)
        for (size_t j = i + 1; j < lexical.size(); j++)
            if (lexical[i] == lexical[j]) return lexical[i];

    for (const auto& l : lexical)
        for (const auto& v : vars)
            if (l == v) return l;

    return "";
}

// Collect bound names from a parameter (including destructuring patterns).
static void collect_param_bound_names(const Parameter* param, std::vector<std::string>& out) {
    if (!param) return;
    if (param->has_destructuring()) {
        collect_var_declared_names(param->get_destructuring_pattern(), out);
    } else if (param->get_name()) {
        out.push_back(param->get_name()->get_name());
    }
}

// Check if any formal parameter name appears in the lexically declared names of the body.
// Returns the first duplicate name, or empty string if none.
static std::string check_params_body_lex_conflict(
    const std::vector<std::unique_ptr<Parameter>>& params,
    const BlockStatement* body)
{
    if (!body) return "";
    std::vector<std::string> param_names;
    for (const auto& p : params) {
        collect_param_bound_names(p.get(), param_names);
    }
    if (param_names.empty()) return "";

    std::vector<std::string> lex_names;
    for (const auto& stmt : body->get_statements()) {
        if (!stmt) continue;
        auto t = stmt->get_type();
        if (t == ASTNode::Type::CLASS_DECLARATION) {
            auto* cls = static_cast<ClassDeclaration*>(stmt.get());
            if (cls->get_id()) lex_names.push_back(cls->get_id()->get_name());
        } else if (t == ASTNode::Type::VARIABLE_DECLARATION) {
            auto* vd = static_cast<VariableDeclaration*>(stmt.get());
            if (vd->get_kind() != VariableDeclarator::Kind::VAR) {
                for (const auto& d : vd->get_declarations()) {
                    if (d->get_id() && !d->get_id()->get_name().empty()) {
                        lex_names.push_back(d->get_id()->get_name());
                    }
                }
            }
        }
    }

    for (const auto& pn : param_names)
        for (const auto& ln : lex_names)
            if (pn == ln) return pn;
    return "";
}

std::unique_ptr<ASTNode> Parser::parse_block_statement(bool is_function_body) {
    Position start = get_current_position();

    if (!consume(TokenType::LEFT_BRACE)) {
        add_error("Expected '{'");
        return nullptr;
    }

    bool outer_strict = options_.strict_mode;
    if (is_function_body && !options_.strict_mode) {
        // Lookahead scan for "use strict" directive without consuming tokens.
        // Scans the directive prologue (leading string literals) to detect strict mode.
        auto next_tok_idx = [&](size_t idx) -> size_t {
            idx++;
            while (idx < tokens_.size() &&
                   (tokens_[idx].get_type() == TokenType::WHITESPACE ||
                    tokens_[idx].get_type() == TokenType::NEWLINE ||
                    tokens_[idx].get_type() == TokenType::COMMENT)) {
                idx++;
            }
            return idx;
        };
        size_t scan = current_token_index_;
        bool found_use_strict = false;
        bool preceding_has_legacy_octal = false;
        while (scan < tokens_.size() && tokens_[scan].get_type() == TokenType::STRING) {
            if (tokens_[scan].get_value() == "use strict") {
                found_use_strict = true;
                options_.strict_mode = true;
                break;
            }
            if (tokens_[scan].string_has_legacy_octal()) {
                preceding_has_legacy_octal = true;
            }
            scan = next_tok_idx(scan);
            if (scan < tokens_.size() && tokens_[scan].get_type() == TokenType::SEMICOLON) {
                scan = next_tok_idx(scan);
            }
        }
        if (found_use_strict && preceding_has_legacy_octal) {
            add_error("SyntaxError: Octal escape sequences are not allowed in strict mode");
        }
    }

    bool saved_block_ctx = options_.in_block_context;
    bool saved_sub = options_.in_substatement_body;
    bool saved_scl = options_.in_switch_case_list;
    options_.in_block_context = true;
    options_.in_substatement_body = false;
    options_.in_switch_case_list = false;

    std::vector<std::unique_ptr<ASTNode>> statements;

    while (!match(TokenType::RIGHT_BRACE) && !at_end()) {
        auto stmt = parse_statement();
        if (stmt) {
            statements.push_back(std::move(stmt));
        } else {
            advance();
        }
    }

    if (!consume(TokenType::RIGHT_BRACE)) {
        add_error("Expected '}'");
        return nullptr;
    }

    options_.in_block_context = saved_block_ctx;
    options_.in_substatement_body = saved_sub;
    options_.in_switch_case_list = saved_scl;

    if (is_function_body) {
        options_.strict_mode = outer_strict;
    }

    if (!is_function_body) {
        std::string dup = find_block_lexical_duplicate(statements);
        if (!dup.empty()) {
            add_error("Identifier '" + dup + "' has already been declared");
            return nullptr;
        }
    }

    Position end = get_current_position();
    return std::make_unique<BlockStatement>(std::move(statements), start, end);
}

bool Parser::validate_array_destructuring(ArrayLiteral* arr) {
    const auto& elems = arr->get_elements();
    bool saw_spread = false;
    for (size_t ei = 0; ei < elems.size(); ei++) {
        const auto& elem = elems[ei];
        if (!elem) continue;
        if (saw_spread) {
            add_error("SyntaxError: Rest element must be last in array destructuring");
            return false;
        }
        if (elem->get_type() == ASTNode::Type::SPREAD_ELEMENT) {
            saw_spread = true;
            auto* se = static_cast<SpreadElement*>(elem.get());
            ASTNode* arg = se->get_argument();
            if (arg) {
                // rest element cannot have initializer
                if (arg->get_type() == ASTNode::Type::ASSIGNMENT_EXPRESSION) {
                    add_error("SyntaxError: Rest element cannot have a default value");
                    return false;
                }
                // validate nested pattern in rest
                if (arg->get_type() == ASTNode::Type::ARRAY_LITERAL) {
                    if (!validate_array_destructuring(static_cast<ArrayLiteral*>(arg)))
                        return false;
                }
                if (arg->get_type() == ASTNode::Type::OBJECT_LITERAL) {
                    if (!validate_object_destructuring(static_cast<ObjectLiteral*>(arg)))
                        return false;
                }
                // comma expr in rest not valid
                if (arg->get_type() == ASTNode::Type::BINARY_EXPRESSION) {
                    auto* be = static_cast<BinaryExpression*>(arg);
                    if (be->get_operator() == BinaryExpression::Operator::COMMA) {
                        add_error("SyntaxError: Invalid destructuring assignment target");
                        return false;
                    }
                }
                // import.meta / new.target are not valid rest targets
                if (arg->get_type() == ASTNode::Type::META_PROPERTY) {
                    add_error("SyntaxError: Invalid destructuring assignment target");
                    return false;
                }
            }
            // rest element must be last
            for (size_t j = ei + 1; j < elems.size(); j++) {
                if (elems[j]) {
                    add_error("SyntaxError: Rest element must be last in array destructuring");
                    return false;
                }
            }
        }
        // nested array
        if (elem->get_type() == ASTNode::Type::ARRAY_LITERAL) {
            if (!validate_array_destructuring(static_cast<ArrayLiteral*>(elem.get())))
                return false;
        }
        // object with method/getter/setter not valid destructuring target
        if (elem->get_type() == ASTNode::Type::OBJECT_LITERAL) {
            auto* obj = static_cast<ObjectLiteral*>(elem.get());
            for (const auto& prop : obj->get_properties()) {
                if (prop->type == ObjectLiteral::PropertyType::Method ||
                    prop->type == ObjectLiteral::PropertyType::Getter ||
                    prop->type == ObjectLiteral::PropertyType::Setter) {
                    add_error("SyntaxError: Invalid destructuring assignment target");
                    return false;
                }
            }
        }
        // comma expression in array elem is invalid
        if (elem->get_type() == ASTNode::Type::BINARY_EXPRESSION) {
            auto* be = static_cast<BinaryExpression*>(elem.get());
            if (be->get_operator() == BinaryExpression::Operator::COMMA) {
                add_error("SyntaxError: Invalid destructuring assignment target");
                return false;
            }
        }
        if (options_.strict_mode && elem->get_type() == ASTNode::Type::IDENTIFIER) {
            auto* id = static_cast<Identifier*>(elem.get());
            if (id->get_name() == "eval" || id->get_name() == "arguments") {
                add_error("SyntaxError: '" + id->get_name() + "' cannot be destructuring target in strict mode");
                return false;
            }
        }
        if (elem->get_type() == ASTNode::Type::META_PROPERTY) {
            add_error("SyntaxError: Invalid destructuring assignment target");
            return false;
        }
    }
    return true;
}

bool Parser::validate_object_destructuring(ObjectLiteral* obj) {
    const auto& props = obj->get_properties();
    for (size_t pi = 0; pi < props.size(); pi++) {
        const auto& prop = props[pi];
        // spread must be last
        if (!prop->key && prop->value &&
            prop->value->get_type() == ASTNode::Type::SPREAD_ELEMENT) {
            if (pi != props.size() - 1) {
                add_error("SyntaxError: Rest element must be last in object destructuring");
                return false;
            }
        }
    }
    for (const auto& prop : props) {
        // method/getter/setter not valid as destructuring target
        if (prop->type == ObjectLiteral::PropertyType::Method ||
            prop->type == ObjectLiteral::PropertyType::Getter ||
            prop->type == ObjectLiteral::PropertyType::Setter) {
            add_error("SyntaxError: Invalid destructuring assignment target");
            return false;
        }
        if (prop->shorthand) {
            if (auto* id = dynamic_cast<Identifier*>(prop->key.get())) {
                if (id->has_escaped_keyword()) {
                    add_error("SyntaxError: Unicode escape sequences are not allowed in keywords");
                    return false;
                }
                const std::string& nm = id->get_name();
                if (options_.strict_mode && (nm == "eval" || nm == "arguments")) {
                    add_error("SyntaxError: '" + nm + "' cannot be destructuring target in strict mode");
                    return false;
                }
                if ((options_.strict_mode || options_.in_generator_body) && nm == "yield") {
                    add_error("SyntaxError: 'yield' cannot be used as identifier here");
                    return false;
                }
                if ((options_.strict_mode || options_.in_async_body) && nm == "await") {
                    add_error("SyntaxError: 'await' cannot be used as identifier here");
                    return false;
                }
                // strict mode future reserved words
                if (options_.strict_mode) {
                    static const std::unordered_set<std::string> strict_future = {
                        "implements","interface","let","package","private",
                        "protected","public","static"
                    };
                    if (strict_future.count(nm)) {
                        add_error("SyntaxError: '" + nm + "' is a reserved word in strict mode");
                        return false;
                    }
                }
            }
        }
        // validate nested value patterns
        if (prop->value) {
            ASTNode* val = prop->value.get();
            if (val->get_type() == ASTNode::Type::META_PROPERTY) {
                add_error("SyntaxError: Invalid destructuring assignment target");
                return false;
            }
            // spread rest: {...import.meta} -- check argument
            if (val->get_type() == ASTNode::Type::SPREAD_ELEMENT) {
                auto* se = static_cast<SpreadElement*>(val);
                if (se->get_argument() && se->get_argument()->get_type() == ASTNode::Type::META_PROPERTY) {
                    add_error("SyntaxError: Invalid destructuring assignment target");
                    return false;
                }
            }
            if (val->get_type() == ASTNode::Type::ARRAY_LITERAL) {
                if (!validate_array_destructuring(static_cast<ArrayLiteral*>(val)))
                    return false;
            }
            if (val->get_type() == ASTNode::Type::OBJECT_LITERAL) {
                if (!validate_object_destructuring(static_cast<ObjectLiteral*>(val)))
                    return false;
            }
            if (val->get_type() == ASTNode::Type::BINARY_EXPRESSION) {
                auto* be = static_cast<BinaryExpression*>(val);
                if (be->get_operator() == BinaryExpression::Operator::COMMA) {
                    add_error("SyntaxError: Invalid destructuring assignment target");
                    return false;
                }
            }
        }
    }
    return true;
}

bool Parser::check_substatement_restrictions(bool is_loop_body) {
    TokenType t = current_token().get_type();

    if (t == TokenType::CLASS) {
        add_error("SyntaxError: Class declarations are not allowed in single-statement context");
        return false;
    }

    if (t == TokenType::FUNCTION) {
        // peek past whitespace to find next real token
        size_t i = current_token_index_ + 1;
        while (i < tokens_.size() &&
               (tokens_[i].get_type() == TokenType::NEWLINE ||
                tokens_[i].get_type() == TokenType::WHITESPACE ||
                tokens_[i].get_type() == TokenType::COMMENT)) {
            i++;
        }
        TokenType next = i < tokens_.size() ? tokens_[i].get_type() : TokenType::EOF_TOKEN;
        if (next == TokenType::MULTIPLY) {
            add_error("SyntaxError: Generator declarations are not allowed in single-statement context");
            return false;
        }
        if (is_loop_body || options_.strict_mode) {
            add_error("SyntaxError: Function declarations are not allowed in single-statement context");
            return false;
        }
    }

    if (t == TokenType::ASYNC) {
        size_t async_end_line = current_token().get_end().line;
        if (peek_token().get_type() == TokenType::FUNCTION &&
            peek_token().get_start().line == async_end_line) {
            add_error("SyntaxError: Async function declarations are not allowed in single-statement context");
            return false;
        }
    }

    if (t == TokenType::CONST) {
        add_error("SyntaxError: Lexical declarations are not allowed in single-statement context");
        return false;
    }

    if (t == TokenType::LET) {
        // peek past whitespace/newlines, tracking if there's a line terminator
        size_t i = current_token_index_ + 1;
        bool has_lt = false;
        while (i < tokens_.size() &&
               (tokens_[i].get_type() == TokenType::NEWLINE ||
                tokens_[i].get_type() == TokenType::WHITESPACE ||
                tokens_[i].get_type() == TokenType::COMMENT)) {
            if (tokens_[i].get_type() == TokenType::NEWLINE) has_lt = true;
            i++;
        }
        TokenType next = i < tokens_.size() ? tokens_[i].get_type() : TokenType::EOF_TOKEN;
        // With a line terminator before `{` or identifier, ASI applies: `let` is an identifier.
        // `let [` is the only form restricted even with line terminators.
        if (next == TokenType::LEFT_BRACKET ||
            (!has_lt && (next == TokenType::IDENTIFIER || next == TokenType::LEFT_BRACE))) {
            add_error("SyntaxError: Lexical declarations are not allowed in single-statement context");
            return false;
        }
    }

    return true;
}

std::unique_ptr<ASTNode> Parser::parse_if_statement() {
    Position start = get_current_position();
    
    if (!consume(TokenType::IF)) {
        add_error("Expected 'if'");
        return nullptr;
    }
    
    if (!consume(TokenType::LEFT_PAREN)) {
        add_error("Expected '(' after 'if'");
        return nullptr;
    }
    
    auto test = parse_expression();
    if (!test) {
        add_error("Expected expression in if condition");
        return nullptr;
    }
    
    if (!consume(TokenType::RIGHT_PAREN)) {
        add_error("Expected ')' after if condition");
        return nullptr;
    }
    
    if (!check_substatement_restrictions(false)) return nullptr;
    options_.in_substatement_body = true;
    auto consequent = parse_statement();
    options_.in_substatement_body = false;
    if (!consequent) {
        add_error("Expected statement after if condition");
        return nullptr;
    }

    std::unique_ptr<ASTNode> alternate = nullptr;
    if (consume_if_match(TokenType::ELSE)) {
        if (!check_substatement_restrictions(false)) return nullptr;
        options_.in_substatement_body = true;
        alternate = parse_statement();
        options_.in_substatement_body = false;
        if (!alternate) {
            add_error("Expected statement after 'else'");
            return nullptr;
        }
    }
    
    Position end = get_current_position();
    return std::make_unique<IfStatement>(std::move(test), std::move(consequent), 
                                        std::move(alternate), start, end);
}

std::unique_ptr<ASTNode> Parser::parse_for_statement() {
    Position start = get_current_position();

    if (!consume(TokenType::FOR)) {
        add_error("Expected 'for'");
        return nullptr;
    }

    // Tracks the declaration kind when `for (let/const/var [pattern] of ...)` is parsed.
    // Used to pass per-iteration lexical scope info to ForOfStatement.
    int for_of_decl_kind = -1;

    bool is_await_loop = false;
    if (match(TokenType::AWAIT)) {
        is_await_loop = true;
        advance();
    }

    if (!consume(TokenType::LEFT_PAREN)) {
        add_error("Expected '(' after 'for'");
        return nullptr;
    }
    
    std::unique_ptr<ASTNode> init = nullptr;
    if (!match(TokenType::SEMICOLON)) {
        // Early error: for (async of ...) is forbidden (lookahead restriction).
        // Does NOT apply to for-await-of, and does NOT apply when =>'async of '
        // starts an async arrow function (the init expression).
        if (!is_await_loop &&
            current_token().get_type() == TokenType::ASYNC && !current_token().has_escaped_keyword()) {
            size_t next_idx = current_token_index_ + 1;
            while (next_idx < tokens_.size() &&
                   (tokens_[next_idx].get_type() == TokenType::WHITESPACE ||
                    tokens_[next_idx].get_type() == TokenType::NEWLINE ||
                    tokens_[next_idx].get_type() == TokenType::COMMENT))
                next_idx++;
            if (next_idx < tokens_.size() && tokens_[next_idx].get_type() == TokenType::OF) {
                // Check if 'of' is followed by '=>' -- then it's an async arrow function, not for-of
                size_t arrow_idx = next_idx + 1;
                while (arrow_idx < tokens_.size() &&
                       (tokens_[arrow_idx].get_type() == TokenType::WHITESPACE ||
                        tokens_[arrow_idx].get_type() == TokenType::COMMENT))
                    arrow_idx++;
                bool is_arrow = (arrow_idx < tokens_.size() &&
                                 tokens_[arrow_idx].get_type() == TokenType::ARROW);
                if (!is_arrow) {
                    add_error("SyntaxError: 'async' is not a valid left-hand side in a for-of loop");
                    return nullptr;
                }
            }
        }
        // Check for 'await using' declaration in for init: for (await using x of ...)
        if (match(TokenType::AWAIT) && (options_.in_async_body || options_.source_type_module)) {
            size_t saved_idx = current_token_index_;
            advance(); // consume 'await'
            if (match(TokenType::IDENTIFIER) && current_token().get_value() == "using") {
                size_t after_using_idx = current_token_index_ + 1;
                // Skip NEWLINE tokens
                while (after_using_idx < tokens_.size() &&
                       (tokens_[after_using_idx].get_type() == TokenType::NEWLINE ||
                        tokens_[after_using_idx].get_type() == TokenType::WHITESPACE))
                    after_using_idx++;
                TokenType next2 = (after_using_idx < tokens_.size())
                                  ? tokens_[after_using_idx].get_type() : TokenType::EOF_TOKEN;
                size_t after_id_idx = after_using_idx + 1;
                while (after_id_idx < tokens_.size() &&
                       (tokens_[after_id_idx].get_type() == TokenType::NEWLINE ||
                        tokens_[after_id_idx].get_type() == TokenType::WHITESPACE))
                    after_id_idx++;
                TokenType next3 = (after_id_idx < tokens_.size())
                                  ? tokens_[after_id_idx].get_type() : TokenType::EOF_TOKEN;
                if ((next2 == TokenType::IDENTIFIER || next2 == TokenType::STATIC || next2 == TokenType::OF) && next3 == TokenType::OF) {
                    // for (await using id of iterable) -- await-using for-of
                    advance(); // consume 'using'
                    std::string binding_name = current_token().get_value();
                    Position binding_pos = get_current_position();
                    advance(); // consume id
                    std::vector<UsingBinding> bindings;
                    bindings.push_back(UsingBinding(binding_name, nullptr));
                    init = std::make_unique<UsingDeclaration>(std::move(bindings), true,
                                                              binding_pos, get_current_position());
                    goto check_for_of;
                } else if ((next2 == TokenType::IDENTIFIER || next2 == TokenType::STATIC) &&
                           next3 == TokenType::ASSIGN) {
                    // for (await using id = expr; ...) -- regular for loop with await using init
                    current_token_index_ = saved_idx; // rewind to 'await'
                    init = parse_using_declaration(true, false);
                    if (!init) return nullptr;
                    goto for_semicolon;
                }
            }
            // Not 'await using': rollback
            current_token_index_ = saved_idx;
        }
        // Check for 'using' declaration in for init: for (using x = expr; ...)
        if (match(TokenType::IDENTIFIER) && current_token().get_value() == "using") {
            TokenType next = peek_token().get_type();
            TokenType next2 = (current_token_index_ + 2 < tokens_.size())
                              ? tokens_[current_token_index_ + 2].get_type()
                              : TokenType::EOF_TOKEN;
            if (next == TokenType::OF && next2 == TokenType::ASSIGN) {
                // for (using of = ...; ...) — using declaration with 'of' as binding name
                // Allow OF as binding name in parse_using_declaration (handled below)
                init = parse_using_declaration(false, false);
                if (!init) return nullptr;
                goto for_semicolon;
            } else if (next == TokenType::IDENTIFIER || next == TokenType::LET || next == TokenType::STATIC) {
                if (next2 == TokenType::OF) {
                    // for (using ID of iterable) — using-for-of: parse binding name only
                    advance(); // consume 'using'
                    std::string binding_name = current_token().get_value();
                    Position binding_pos = get_current_position();
                    // Early error: 'let' cannot be a binding name in using declaration
                    if (current_token().get_type() == TokenType::LET) {
                        add_error("SyntaxError: 'let' cannot be a binding name in 'using' declaration");
                        return nullptr;
                    }
                    advance(); // consume ID
                    std::vector<UsingBinding> bindings;
                    bindings.push_back(UsingBinding(binding_name, nullptr));
                    init = std::make_unique<UsingDeclaration>(std::move(bindings), false,
                                                              binding_pos, get_current_position());
                    goto check_for_of;
                }
                init = parse_using_declaration(false, false);
                if (!init) return nullptr;
                goto for_semicolon;
            }
            // next is OF without '=' after: treat 'using' as identifier, fall through
        }
        // Non-strict: "let" followed by "=", ";", etc. is an identifier, not a declaration
        bool let_as_id = (!options_.strict_mode &&
                          match(TokenType::LET) &&
                          peek_token().get_type() != TokenType::IDENTIFIER &&
                          peek_token().get_type() != TokenType::LEFT_BRACKET &&
                          peek_token().get_type() != TokenType::LEFT_BRACE);
        if (!let_as_id && (match(TokenType::VAR) || match(TokenType::LET) || match(TokenType::CONST))) {

            Position decl_start = get_current_position();

            TokenType kind_token = current_token().get_type();
            VariableDeclarator::Kind kind;
            switch (kind_token) {
                case TokenType::VAR: kind = VariableDeclarator::Kind::VAR; break;
                case TokenType::LET: kind = VariableDeclarator::Kind::LET; break;
                case TokenType::CONST: kind = VariableDeclarator::Kind::CONST; break;
                default:
                    add_error("Expected variable declaration keyword");
                    return nullptr;
            }
            advance();
            
            if (current_token().get_type() == TokenType::LEFT_BRACKET ||
                current_token().get_type() == TokenType::LEFT_BRACE) {

                auto destructuring = parse_destructuring_pattern();
                if (!destructuring) {
                    add_error("Failed to parse destructuring pattern");
                    return nullptr;
                }

                // For const/let: check for duplicate binding names
                if (kind == VariableDeclarator::Kind::CONST || kind == VariableDeclarator::Kind::LET) {
                    auto* da = dynamic_cast<DestructuringAssignment*>(destructuring.get());
                    if (da) {
                        std::unordered_set<std::string> seen;
                        for (const auto& t : da->get_targets()) {
                            if (!t) continue;
                            std::string nm = t->get_name();
                            if (nm.empty() || nm[0] == '_') continue;
                            if (nm.length() >= 3 && nm.substr(0,3) == "...") nm = nm.substr(3);
                            if (!nm.empty() && !seen.insert(nm).second) {
                                add_error("SyntaxError: Identifier '" + nm + "' has already been declared");
                                return nullptr;
                            }
                        }
                    }
                }

                if (current_token().get_type() == TokenType::ASSIGN) {
                    advance();
                    auto initializer = parse_assignment_expression();
                    if (!initializer) {
                        add_error("Expected expression after '=' in destructuring assignment");
                        return nullptr;
                    }

                    Position assign_start = destructuring->get_start();
                    Position assign_end = initializer->get_end();
                    auto assignment = std::make_unique<AssignmentExpression>(
                        std::move(destructuring),
                        AssignmentExpression::Operator::ASSIGN,
                        std::move(initializer),
                        assign_start,
                        assign_end
                    );
                    init = std::move(assignment);
                } else {
                    init = std::move(destructuring);
                }
                for_of_decl_kind = static_cast<int>(kind);

                goto check_for_of;
            }

            // Accept keyword tokens that can serve as BindingIdentifiers in some contexts.
            {
                TokenType ct = current_token().get_type();
                bool is_kw_binding = (ct == TokenType::LET || ct == TokenType::YIELD ||
                                      ct == TokenType::AWAIT || ct == TokenType::STATIC);
                if (ct != TokenType::IDENTIFIER && !is_kw_binding) {
                    add_error("Expected identifier in variable declaration");
                    return nullptr;
                }
            }

            std::string var_name = current_token().get_value();
            Position var_start = current_token().get_start();
            Position var_end = current_token().get_end();
            advance();

            // 'let' cannot be a binding name in let/const declarations (always).
            if ((kind == VariableDeclarator::Kind::LET || kind == VariableDeclarator::Kind::CONST) &&
                var_name == "let") {
                add_error("SyntaxError: 'let' is not a valid binding name for 'let'/'const' declarations");
                return nullptr;
            }
            // 'yield' cannot be a binding name in generators or strict mode.
            if (var_name == "yield" &&
                (options_.in_generator_body || options_.strict_mode)) {
                add_error("SyntaxError: 'yield' cannot be used as a binding name here");
                return nullptr;
            }
            // 'await' cannot be a binding name in async bodies or module code.
            if (var_name == "await" &&
                (options_.in_async_body || options_.source_type_module ||
                 options_.in_class_static_block)) {
                add_error("SyntaxError: 'await' cannot be used as a binding name here");
                return nullptr;
            }

            if (options_.strict_mode && (var_name == "eval" || var_name == "arguments")) {
                add_error("SyntaxError: '" + var_name + "' cannot be used as variable name in strict mode");
                return nullptr;
            }

            auto identifier = std::make_unique<Identifier>(var_name, var_start, var_end);

            std::unique_ptr<ASTNode> initializer = nullptr;
            if (current_token().get_type() == TokenType::ASSIGN) {
                advance();
                // Use no_in_mode so "0 in {}" doesn't consume the 'in' keyword (Annex B)
                bool prev_no_in = no_in_mode_;
                no_in_mode_ = true;
                initializer = parse_assignment_expression();
                no_in_mode_ = prev_no_in;
                if (!initializer) {
                    add_error("Expected expression after '=' in variable declaration");
                    return nullptr;
                }
            }

            auto declarator = std::make_unique<VariableDeclarator>(
                std::move(identifier),
                std::move(initializer),
                kind,
                var_start,
                var_end
            );
            
            std::vector<std::unique_ptr<VariableDeclarator>> declarations;
            declarations.push_back(std::move(declarator));

            while (match(TokenType::COMMA)) {
                advance();

                auto next_identifier = parse_identifier();
                if (!next_identifier) {
                    add_error("Expected identifier after ',' in variable declaration");
                    return nullptr;
                }

                std::unique_ptr<ASTNode> next_initializer = nullptr;
                if (match(TokenType::ASSIGN)) {
                    advance();
                    next_initializer = parse_assignment_expression();
                    if (!next_initializer) {
                        add_error("Expected expression after '=' in variable declaration");
                        return nullptr;
                    }
                }

                Position next_var_end = get_current_position();
                auto next_declarator = std::make_unique<VariableDeclarator>(
                    std::unique_ptr<Identifier>(static_cast<Identifier*>(next_identifier.release())),
                    std::move(next_initializer),
                    kind,
                    var_start,
                    next_var_end
                );
                declarations.push_back(std::move(next_declarator));
            }
            
            Position decl_end = get_current_position();
            init = std::make_unique<VariableDeclaration>(std::move(declarations), kind, decl_start, decl_end);
            
        } else {
            // Parse init without consuming 'in' (reserved for for-in detection).
            // Skip no_in_mode for for-await (always for-of) and destructuring LHS
            // ('{' or '[' can never start a for-in LHS, so 'in' in defaults is fine).
            bool prev_no_in = no_in_mode_;
            if (!is_await_loop) no_in_mode_ = true;
            init = parse_expression();
            no_in_mode_ = prev_no_in;
            if (!init) {
                add_error("Expected initialization in for loop");
                return nullptr;
            }
        }
    }

    if (current_token().get_type() == TokenType::IN) {
        // Early SyntaxError: LHS must be a valid assignment target
        if (init && init->get_type() != ASTNode::Type::VARIABLE_DECLARATION) {
            auto t = init->get_type();
            if (t != ASTNode::Type::IDENTIFIER &&
                t != ASTNode::Type::MEMBER_EXPRESSION &&
                t != ASTNode::Type::ARRAY_LITERAL &&
                t != ASTNode::Type::OBJECT_LITERAL &&
                t != ASTNode::Type::DESTRUCTURING_ASSIGNMENT) {
                add_error("SyntaxError: Invalid left-hand side in for-in");
                return nullptr;
            }
            // 'this' is not a valid assignment target
            if (t == ASTNode::Type::IDENTIFIER) {
                auto* id = static_cast<Identifier*>(init.get());
                if (id->get_name() == "this") {
                    add_error("SyntaxError: Invalid left-hand side in for-in");
                    return nullptr;
                }
            }
            // validate destructuring patterns
            if (t == ASTNode::Type::ARRAY_LITERAL) {
                if (!validate_array_destructuring(static_cast<ArrayLiteral*>(init.get())))
                    return nullptr;
            }
            if (t == ASTNode::Type::OBJECT_LITERAL) {
                if (!validate_object_destructuring(static_cast<ObjectLiteral*>(init.get())))
                    return nullptr;
            }
        }
        if (init && init->get_type() == ASTNode::Type::VARIABLE_DECLARATION) {
            auto* vd = static_cast<VariableDeclaration*>(init.get());
            if (vd->declaration_count() > 1) {
                add_error("SyntaxError: for-in loop may only have one variable declaration");
                return nullptr;
            }
            if (vd->declaration_count() > 0 && vd->get_declarations()[0]->get_init()) {
                bool is_lexical = vd->get_kind() == VariableDeclarator::Kind::LET ||
                                  vd->get_kind() == VariableDeclarator::Kind::CONST;
                if (is_lexical || options_.strict_mode) {
                    add_error("SyntaxError: for-in loop variable declaration may not have an initializer");
                    return nullptr;
                }
            }
        }
        advance();

        if (at_end()) {
            add_error("Unexpected end of input after 'in'");
            return nullptr;
        }

        auto object = parse_expression();
        if (!object) {
            add_error("Expected expression after 'in' in for...in loop");
            return nullptr;
        }

        if (at_end() || current_token().get_type() != TokenType::RIGHT_PAREN) {
            add_error("Expected ')' after for...in object");
            return nullptr;
        }
        advance();

        if (at_end()) {
            add_error("Expected statement after for...in");
            return nullptr;
        }
        if (!check_substatement_restrictions()) return nullptr;
        options_.loop_depth++;
        options_.in_substatement_body = true;
        auto body = parse_statement();
        options_.in_substatement_body = false;
        options_.loop_depth--;
        if (!body) {
            add_error("Failed to parse for...in body");
            return nullptr;
        }

        // BoundNames of for-head must not appear in VarDeclaredNames of body (for const/let)
        if (init && init->get_type() == ASTNode::Type::VARIABLE_DECLARATION) {
            auto* vd = static_cast<VariableDeclaration*>(init.get());
            bool is_lexical = vd->get_kind() == VariableDeclarator::Kind::LET ||
                              vd->get_kind() == VariableDeclarator::Kind::CONST;
            if (is_lexical) {
                std::unordered_set<std::string> head_names;
                for (const auto& d : vd->get_declarations())
                    if (d->get_id()) head_names.insert(d->get_id()->get_name());
                std::vector<std::string> body_vars;
                collect_var_declared_names(body.get(), body_vars);
                for (const auto& v : body_vars) {
                    if (head_names.count(v)) {
                        add_error("SyntaxError: Variable '" + v + "' in for-in head conflicts with var declaration in body");
                        return nullptr;
                    }
                }
            }
        }

        Position end = get_current_position();
        return std::make_unique<ForInStatement>(std::move(init), std::move(object), std::move(body), start, end);
    }

check_for_of:
    if (current_token().get_type() == TokenType::IN) {
        advance();

        if (at_end()) {
            add_error("Unexpected end of input after 'in'");
            return nullptr;
        }

        auto object = parse_expression();
        if (!object) {
            add_error("Expected expression after 'in' in for...in loop");
            return nullptr;
        }

        if (at_end() || current_token().get_type() != TokenType::RIGHT_PAREN) {
            add_error("Expected ')' after for...in object");
            return nullptr;
        }
        advance();

        if (at_end()) {
            add_error("Expected statement after for...in");
            return nullptr;
        }
        if (!check_substatement_restrictions()) return nullptr;
        options_.loop_depth++;
        options_.in_substatement_body = true;
        auto body = parse_statement();
        options_.in_substatement_body = false;
        options_.loop_depth--;
        if (!body) {
            add_error("Failed to parse for...in body");
            return nullptr;
        }

        // BoundNames of for-head must not appear in VarDeclaredNames of body (for const/let)
        if (init && init->get_type() == ASTNode::Type::VARIABLE_DECLARATION) {
            auto* vd = static_cast<VariableDeclaration*>(init.get());
            bool is_lexical = vd->get_kind() == VariableDeclarator::Kind::LET ||
                              vd->get_kind() == VariableDeclarator::Kind::CONST;
            if (is_lexical) {
                std::unordered_set<std::string> head_names;
                for (const auto& d : vd->get_declarations())
                    if (d->get_id()) head_names.insert(d->get_id()->get_name());
                std::vector<std::string> body_vars;
                collect_var_declared_names(body.get(), body_vars);
                for (const auto& v : body_vars) {
                    if (head_names.count(v)) {
                        add_error("SyntaxError: Variable '" + v + "' in for-in head conflicts with var declaration in body");
                        return nullptr;
                    }
                }
            }
        }

        Position end = get_current_position();
        return std::make_unique<ForInStatement>(std::move(init), std::move(object), std::move(body), start, end, for_of_decl_kind);
    }

    if (current_token().get_type() == TokenType::OF) {
        // Early SyntaxError: LHS must be a valid assignment target
        if (init && init->get_type() != ASTNode::Type::VARIABLE_DECLARATION
                 && init->get_type() != ASTNode::Type::USING_DECLARATION) {
            auto t = init->get_type();
            if (t != ASTNode::Type::IDENTIFIER &&
                t != ASTNode::Type::MEMBER_EXPRESSION &&
                t != ASTNode::Type::ARRAY_LITERAL &&
                t != ASTNode::Type::OBJECT_LITERAL &&
                t != ASTNode::Type::DESTRUCTURING_ASSIGNMENT) {
                add_error("SyntaxError: Invalid left-hand side in for-of");
                return nullptr;
            }
        }
        // for-of: declaration with initializer or multiple bindings is SyntaxError
        if (init && init->get_type() == ASTNode::Type::VARIABLE_DECLARATION) {
            auto* vd = static_cast<VariableDeclaration*>(init.get());
            if (vd->declaration_count() > 1) {
                add_error("SyntaxError: for-of loop may only have one variable declaration");
                return nullptr;
            }
            if (vd->declaration_count() > 0 && vd->get_declarations()[0]->get_init()) {
                add_error("SyntaxError: for-of loop variable declaration may not have an initializer");
                return nullptr;
            }
        }
        // for-of LHS: `this` and `let` are not valid
        if (init && init->get_type() == ASTNode::Type::IDENTIFIER) {
            auto* id = static_cast<Identifier*>(init.get());
            if (id->get_name() == "this") {
                add_error("SyntaxError: Invalid left-hand side in for-of");
                return nullptr;
            }
            if (id->get_name() == "let") {
                add_error("SyntaxError: 'let' is not a valid LHS in for-of");
                return nullptr;
            }
        }
        // validate array/object destructuring LHS
        if (init && init->get_type() == ASTNode::Type::ARRAY_LITERAL) {
            if (!validate_array_destructuring(static_cast<ArrayLiteral*>(init.get())))
                return nullptr;
        }
        if (init && init->get_type() == ASTNode::Type::OBJECT_LITERAL) {
            if (!validate_object_destructuring(static_cast<ObjectLiteral*>(init.get())))
                return nullptr;
        }
        advance();

        if (at_end()) {
            add_error("Unexpected end of input after 'of'");
            return nullptr;
        }

        auto iterable = parse_assignment_expression();
        if (!iterable) {
            add_error("Expected expression after 'of' in for...of loop");
            return nullptr;
        }
        
        if (at_end() || current_token().get_type() != TokenType::RIGHT_PAREN) {
            add_error("Expected ')' after for...of iterable");
            return nullptr;
        }
        advance();
        
        if (at_end()) {
            add_error("Expected statement after for...of");
            return nullptr;
        }
        if (!check_substatement_restrictions()) return nullptr;
        options_.loop_depth++;
        options_.in_substatement_body = true;
        auto body = parse_statement();
        options_.in_substatement_body = false;
        options_.loop_depth--;
        if (!body) {
            add_error("Failed to parse for...of body");
            return nullptr;
        }

        // Early error: BoundNames of for-head must not appear in VarDeclaredNames of body
        if (init && (init->get_type() == ASTNode::Type::VARIABLE_DECLARATION ||
                     init->get_type() == ASTNode::Type::USING_DECLARATION) && body) {
            std::unordered_set<std::string> head_names;
            if (init->get_type() == ASTNode::Type::VARIABLE_DECLARATION) {
                auto* vd = static_cast<VariableDeclaration*>(init.get());
                if (vd->get_kind() != VariableDeclarator::Kind::VAR) {
                    for (const auto& d : vd->get_declarations())
                        if (d->get_id()) head_names.insert(d->get_id()->get_name());
                }
            } else {
                auto* ud = static_cast<UsingDeclaration*>(init.get());
                for (const auto& b : ud->get_bindings()) head_names.insert(b.name);
            }
            // Collect var-declared names from body (stops at function boundaries)
            std::function<void(ASTNode*)> collect_vars = [&](ASTNode* node) {
                if (!node) return;
                auto t = node->get_type();
                if (t == ASTNode::Type::FUNCTION_DECLARATION || t == ASTNode::Type::FUNCTION_EXPRESSION ||
                    t == ASTNode::Type::ARROW_FUNCTION_EXPRESSION || t == ASTNode::Type::ASYNC_FUNCTION_EXPRESSION ||
                    t == ASTNode::Type::CLASS_DECLARATION) return; // stop at function boundary
                if (t == ASTNode::Type::VARIABLE_DECLARATION) {
                    auto* vd = static_cast<VariableDeclaration*>(node);
                    if (vd->get_kind() == VariableDeclarator::Kind::VAR) {
                        for (const auto& d : vd->get_declarations())
                            if (d->get_id() && head_names.count(d->get_id()->get_name())) {
                                add_error("SyntaxError: Variable '" + d->get_id()->get_name() +
                                          "' in for-of head conflicts with var declaration in body");
                            }
                    }
                } else if (t == ASTNode::Type::BLOCK_STATEMENT) {
                    auto* blk = static_cast<BlockStatement*>(node);
                    for (const auto& s : blk->get_statements()) collect_vars(s.get());
                } else if (t == ASTNode::Type::IF_STATEMENT) {
                    auto* ifs = static_cast<IfStatement*>(node);
                    collect_vars(ifs->get_consequent());
                    collect_vars(ifs->get_alternate());
                }
            };
            collect_vars(body.get());
            if (has_errors()) return nullptr;
        }

        Position end = get_current_position();
        return std::make_unique<ForOfStatement>(std::move(init), std::move(iterable), std::move(body), is_await_loop, start, end, for_of_decl_kind);
    }
    
for_semicolon:
    if (!consume(TokenType::SEMICOLON)) {
        add_error("Expected ';' after for loop init");
        return nullptr;
    }

    std::unique_ptr<ASTNode> test = nullptr;
    if (!match(TokenType::SEMICOLON)) {
        test = parse_expression();
        if (!test) {
            add_error("Expected test condition in for loop");
            return nullptr;
        }
    }
    
    if (!consume(TokenType::SEMICOLON)) {
        add_error("Expected ';' after for loop test");
        return nullptr;
    }
    
    std::unique_ptr<ASTNode> update = nullptr;
    if (!match(TokenType::RIGHT_PAREN)) {
        update = parse_expression();
        if (!update) {
            add_error("Expected update expression in for loop");
            return nullptr;
        }
    }
    
    if (!consume(TokenType::RIGHT_PAREN)) {
        add_error("Expected ')' after for loop");
        return nullptr;
    }
    if (!check_substatement_restrictions()) return nullptr;
    options_.loop_depth++;
    options_.in_substatement_body = true;
    auto body = parse_statement();
    options_.in_substatement_body = false;
    options_.loop_depth--;
    if (!body) {
        add_error("Expected statement for for loop body");
        return nullptr;
    }

    // BoundNames of lexical declaration in for head must not appear in VarDeclaredNames of body
    if (init && init->get_type() == ASTNode::Type::VARIABLE_DECLARATION) {
        auto* vd = static_cast<VariableDeclaration*>(init.get());
        bool is_lexical = vd->get_kind() == VariableDeclarator::Kind::LET ||
                          vd->get_kind() == VariableDeclarator::Kind::CONST;
        if (is_lexical) {
            std::unordered_set<std::string> head_names;
            for (const auto& d : vd->get_declarations())
                if (d->get_id()) head_names.insert(d->get_id()->get_name());
            std::vector<std::string> body_vars;
            collect_var_declared_names(body.get(), body_vars);
            for (const auto& v : body_vars) {
                if (head_names.count(v)) {
                    add_error("SyntaxError: Variable '" + v + "' in for head conflicts with var declaration in body");
                    return nullptr;
                }
            }
        }
    }

    Position end = get_current_position();
    return std::make_unique<ForStatement>(std::move(init), std::move(test),
                                         std::move(update), std::move(body), start, end,
                                         for_of_decl_kind);
}

std::unique_ptr<ASTNode> Parser::parse_while_statement() {
    Position start = get_current_position();
    
    if (!consume(TokenType::WHILE)) {
        add_error("Expected 'while'");
        return nullptr;
    }
    
    if (!consume(TokenType::LEFT_PAREN)) {
        add_error("Expected '(' after 'while'");
        return nullptr;
    }
    
    auto test = parse_expression();
    if (!test) {
        add_error("Expected condition in while loop");
        return nullptr;
    }
    
    if (!consume(TokenType::RIGHT_PAREN)) {
        add_error("Expected ')' after while condition");
        return nullptr;
    }
    
    if (!check_substatement_restrictions()) return nullptr;
    options_.loop_depth++;
    options_.in_substatement_body = true;
    auto body = parse_statement();
    options_.in_substatement_body = false;
    options_.loop_depth--;
    if (!body) {
        add_error("Expected statement for while loop body");
        return nullptr;
    }

    Position end = get_current_position();
    return std::make_unique<WhileStatement>(std::move(test), std::move(body), start, end);
}

std::unique_ptr<ASTNode> Parser::parse_do_while_statement() {
    Position start = get_current_position();

    if (!consume(TokenType::DO)) {
        add_error("Expected 'do'");
        return nullptr;
    }

    if (!check_substatement_restrictions()) return nullptr;
    options_.loop_depth++;
    options_.in_substatement_body = true;
    auto body = parse_statement();
    options_.in_substatement_body = false;
    options_.loop_depth--;
    if (!body) {
        add_error("Expected statement for do-while loop body");
        return nullptr;
    }
    
    if (!consume(TokenType::WHILE)) {
        add_error("Expected 'while' after do-while body");
        return nullptr;
    }
    
    if (!consume(TokenType::LEFT_PAREN)) {
        add_error("Expected '(' after 'while'");
        return nullptr;
    }
    
    auto test = parse_expression();
    if (!test) {
        add_error("Expected condition in do-while loop");
        return nullptr;
    }
    
    if (!consume(TokenType::RIGHT_PAREN)) {
        add_error("Expected ')' after do-while condition");
        return nullptr;
    }
    
    consume_if_match(TokenType::SEMICOLON);
    
    Position end = get_current_position();
    return std::make_unique<DoWhileStatement>(std::move(body), std::move(test), start, end);
}

std::unique_ptr<ASTNode> Parser::parse_with_statement() {
    Position start = get_current_position();

    if (options_.strict_mode) {
        add_error("SyntaxError: 'with' statements are not allowed in strict mode");
        return nullptr;
    }

    if (!consume(TokenType::WITH)) {
        add_error("Expected 'with'");
        return nullptr;
    }

    if (!consume(TokenType::LEFT_PAREN)) {
        add_error("Expected '(' after 'with'");
        return nullptr;
    }

    auto object = parse_expression();
    if (!object) {
        add_error("Expected expression in with statement");
        return nullptr;
    }

    if (!consume(TokenType::RIGHT_PAREN)) {
        add_error("Expected ')' after with object");
        return nullptr;
    }

    if (!check_substatement_restrictions()) return nullptr;
    auto body = parse_statement();
    if (!body) {
        add_error("Expected statement for with body");
        return nullptr;
    }

    Position end = get_current_position();
    return std::make_unique<WithStatement>(std::move(object), std::move(body), start, end);
}

std::unique_ptr<ASTNode> Parser::parse_expression_statement() {
    Position start = get_current_position();

    // escaped `async` + function on same line = SyntaxError
    if (current_token().get_type() == TokenType::IDENTIFIER &&
        current_token().has_escaped_keyword() &&
        current_token().get_value() == "async") {
        size_t async_end_line = current_token().get_end().line;
        if (peek_token().get_type() == TokenType::FUNCTION &&
            peek_token().get_start().line == async_end_line) {
            add_error("SyntaxError: `async` cannot contain unicode escape sequences");
            return nullptr;
        }
    }

    // yield/await/let are valid label names in certain contexts
    bool cur_is_label_kw = false;
    if (peek_token().get_type() == TokenType::COLON) {
        TokenType ct = current_token().get_type();
        if (ct == TokenType::YIELD && !options_.in_generator_body && !options_.strict_mode)
            cur_is_label_kw = true;
        else if (ct == TokenType::AWAIT && !options_.in_async_body && !options_.source_type_module && !options_.in_class_static_block)
            cur_is_label_kw = true;
        else if (ct == TokenType::LET && !options_.strict_mode)
            cur_is_label_kw = true;
    }

    if ((current_token().get_type() == TokenType::IDENTIFIER || cur_is_label_kw) && peek_token().get_type() == TokenType::COLON) {
        std::string label = current_token().get_value();
        // Escaped reserved words as label identifiers are SyntaxErrors
        if (current_token().has_escaped_keyword()) {
            // yield and await have context-dependent rules; all other reserved words are always invalid
            static const std::unordered_set<std::string> always_reserved_labels = {
                "false","true","null","this","super",
                "break","case","catch","class","const","continue","debugger",
                "default","delete","do","else","export","extends","finally",
                "for","function","if","import","in","instanceof","new",
                "return","switch","throw","try","typeof","var","void","while","with",
                "enum","implements","interface","let","package","private",
                "protected","public","static"
            };
            if (always_reserved_labels.count(label) ||
                (options_.in_async_body && label == "await") ||
                (options_.in_generator_body && label == "yield") ||
                (options_.source_type_module && label == "await") ||
                options_.strict_mode) {
                add_error("SyntaxError: '" + label + "' cannot be used as a label identifier here");
                return nullptr;
            }
        }
        if ((options_.in_async_body && label == "await") ||
            (options_.in_generator_body && label == "yield")) {
            add_error("SyntaxError: '" + label + "' cannot be used as a label identifier here");
            return nullptr;
        }
        if (options_.active_labels.count(label)) {
            add_error("SyntaxError: Duplicate label '" + label + "'");
            return nullptr;
        }
        advance();
        advance();
        options_.active_labels.insert(label);

        // Peek through all labels to find the actual statement type for loop_labels tracking
        {
            size_t peek_idx = current_token_index_;
            while (peek_idx + 1 < tokens_.size() &&
                   tokens_[peek_idx].get_type() == TokenType::IDENTIFIER &&
                   tokens_[peek_idx + 1].get_type() == TokenType::COLON) {
                peek_idx += 2;
                // skip whitespace/newlines
                while (peek_idx < tokens_.size() &&
                       (tokens_[peek_idx].get_type() == TokenType::NEWLINE ||
                        tokens_[peek_idx].get_type() == TokenType::WHITESPACE ||
                        tokens_[peek_idx].get_type() == TokenType::COMMENT))
                    peek_idx++;
            }
            if (peek_idx < tokens_.size()) {
                TokenType inner = tokens_[peek_idx].get_type();
                if (inner == TokenType::FUNCTION) {
                    // Generator function never allowed in labeled position
                    size_t gen_idx = peek_idx + 1;
                    while (gen_idx < tokens_.size() &&
                           (tokens_[gen_idx].get_type() == TokenType::NEWLINE ||
                            tokens_[gen_idx].get_type() == TokenType::WHITESPACE ||
                            tokens_[gen_idx].get_type() == TokenType::COMMENT))
                        gen_idx++;
                    if (gen_idx < tokens_.size() && tokens_[gen_idx].get_type() == TokenType::MULTIPLY) {
                        add_error("SyntaxError: Generator declarations are not allowed as labeled statements");
                        return nullptr;
                    }
                    // labeled function: only 1 total level allowed via Annex B
                    size_t label_count = 0;
                    size_t ci = current_token_index_;
                    while (ci + 1 < tokens_.size() &&
                           tokens_[ci].get_type() == TokenType::IDENTIFIER &&
                           tokens_[ci + 1].get_type() == TokenType::COLON) {
                        label_count++;
                        ci += 2;
                        while (ci < tokens_.size() &&
                               (tokens_[ci].get_type() == TokenType::NEWLINE ||
                                tokens_[ci].get_type() == TokenType::WHITESPACE ||
                                tokens_[ci].get_type() == TokenType::COMMENT))
                            ci++;
                    }
                    if (label_count + (int)options_.active_labels.size() > 1 ||
                        options_.loop_depth > 0 || options_.strict_mode ||
                        options_.in_substatement_body) {
                        add_error("SyntaxError: Function declarations not allowed in this context");
                        return nullptr;
                    }
                }
                if (inner == TokenType::ASYNC || inner == TokenType::CLASS) {
                    add_error("SyntaxError: Declaration not allowed after label");
                    return nullptr;
                }
                if (inner == TokenType::LET || inner == TokenType::CONST) {
                    size_t let_idx = peek_idx;
                    size_t next_idx = let_idx + 1;
                    while (next_idx < tokens_.size() &&
                           (tokens_[next_idx].get_type() == TokenType::WHITESPACE ||
                            tokens_[next_idx].get_type() == TokenType::NEWLINE ||
                            tokens_[next_idx].get_type() == TokenType::COMMENT))
                        next_idx++;
                    if (next_idx < tokens_.size()) {
                        TokenType after_let = tokens_[next_idx].get_type();
                        bool same_line = tokens_[next_idx].get_start().line == tokens_[let_idx].get_end().line;
                        // `let [` always starts a LexicalDeclaration (spec: `let [` lookahead restriction)
                        bool always_decl = after_let == TokenType::LEFT_BRACKET;
                        // `let identifier` / `let {` only a declaration when on same line
                        bool same_line_decl = same_line && (after_let == TokenType::IDENTIFIER ||
                                                            after_let == TokenType::LEFT_BRACE);
                        if (always_decl || same_line_decl) {
                            add_error("SyntaxError: Declaration not allowed after label");
                            return nullptr;
                        }
                    }
                }
                if (inner == TokenType::FOR || inner == TokenType::WHILE || inner == TokenType::DO) {
                    options_.loop_labels.insert(label);
                }
            }
        }
        options_.in_substatement_body = true;
        auto statement = parse_statement();
        options_.in_substatement_body = false;
        if (!statement) {
            add_error("Expected statement after label");
            return nullptr;
        }

        options_.active_labels.erase(label);
        options_.loop_labels.erase(label);
        Position end = statement->get_end();
        return std::make_unique<LabeledStatement>(label, std::move(statement), start, end);
    }

    auto expr = parse_expression();
    if (!expr) {
        return nullptr;
    }

    start = expr->get_start();
    Position end = expr->get_end();

    // Determine if there is a line terminator between the end of the expression
    // and the next token, using raw token stream (since advance() skips newlines,
    // the line-number comparison on get_end() is unreliable when get_current_position()
    // returns the start of the next token).
    // Check for a line terminator between the last consumed token and the current token.
    // Two-pass: (1) scan raw token stream backwards for NEWLINE tokens; (2) if none found,
    // compare line numbers of the preceding meaningful token vs. current token to catch
    // newlines that were absorbed inside multi-line block comments (not emitted as tokens).
    bool has_lt_before_next = false;
    if (current_token_index_ > 0) {
        size_t i = current_token_index_;
        while (i > 0) {
            i--;
            TokenType tt = tokens_[i].get_type();
            if (tt == TokenType::NEWLINE) { has_lt_before_next = true; break; }
            if (tt != TokenType::WHITESPACE) {
                // No NEWLINE token found; check if a multi-line comment caused a line gap
                if (tokens_[i].get_end().line < current_token().get_start().line) {
                    has_lt_before_next = true;
                }
                break;
            }
        }
    }

    if (match(TokenType::SEMICOLON)) {
        advance();
    } else if (at_end() || match(TokenType::RIGHT_BRACE) || has_lt_before_next) {
        // ASI applies
    } else {
        // No semicolon and ASI conditions not met: SyntaxError
        add_error("SyntaxError: Unexpected token - missing semicolon");
        return nullptr;
    }

    return std::make_unique<ExpressionStatement>(std::move(expr), start, end);
}

std::unique_ptr<ASTNode> Parser::parse_function_declaration() {
    Position start = get_current_position();
    
    if (!consume(TokenType::FUNCTION)) {
        add_error("Expected 'function'");
        return nullptr;
    }
    
    bool is_generator = false;
    if (current_token().get_type() == TokenType::MULTIPLY) {
        advance();
        is_generator = true;
    }
    
    {
        TokenType ct = current_token().get_type();
        bool name_ok = ct == TokenType::IDENTIFIER;
        if (!name_ok && ct == TokenType::YIELD && !options_.in_generator_body && !options_.strict_mode)
            name_ok = true;
        if (!name_ok && ct == TokenType::AWAIT && !options_.in_async_body && !options_.source_type_module && !options_.in_class_static_block)
            name_ok = true;
        if (!name_ok && ct == TokenType::LET && !options_.strict_mode)
            name_ok = true;
        if (!name_ok) {
            add_error("Expected function name");
            return nullptr;
        }
    }

    std::string fn_name = current_token().get_value();
    auto id = std::make_unique<Identifier>(fn_name,
                                         current_token().get_start(), current_token().get_end());
    advance();

    if (options_.strict_mode && (fn_name == "eval" || fn_name == "arguments")) {
        add_error("'" + fn_name + "' cannot be used as function name in strict mode");
        return nullptr;
    }

    if (!consume(TokenType::LEFT_PAREN)) {
        add_error("Expected '(' after function name");
        return nullptr;
    }

    std::vector<std::unique_ptr<Parameter>> params;
    bool has_non_simple_params = false;

    // FormalParameters[+Yield] for generator declarations: parse bare `yield` in
    // parameter default values as YieldExpression (not Identifier) so the
    // Contains-YieldExpression early error below can detect it (spec 15.5.1).
    bool saved_gen_for_params_fd = options_.in_generator_body;
    bool saved_csb_params_fd = options_.in_class_static_block;
    options_.in_generator_body = is_generator;
    options_.in_class_static_block = false;

    while (!match(TokenType::RIGHT_PAREN) && !at_end()) {
        Position param_start = current_token().get_start();
        bool is_rest = false;

        if (match(TokenType::ELLIPSIS)) {
            is_rest = true;
            has_non_simple_params = true;
            advance();
        }

        std::unique_ptr<Identifier> param_name = nullptr;

        if (current_token().get_type() == TokenType::LEFT_BRACKET ||
            current_token().get_type() == TokenType::LEFT_BRACE) {
            has_non_simple_params = true;
            auto destructuring = parse_destructuring_pattern();
            if (!destructuring) {
                add_error("Invalid destructuring pattern in function parameters");
                options_.in_generator_body = saved_gen_for_params_fd;
                return nullptr;
            }
            static int destr_param_counter = 0;
            std::string synthetic_name = "__destr_" + std::to_string(destr_param_counter++);
            Position param_pos = get_current_position();
            param_name = std::make_unique<Identifier>(synthetic_name, param_pos, param_pos);

            std::unique_ptr<ASTNode> default_value = nullptr;
            if (!is_rest && match(TokenType::ASSIGN)) {
                advance();
                default_value = parse_assignment_expression();
                if (!default_value) {
                    add_error("Invalid default parameter value");
                    options_.in_generator_body = saved_gen_for_params_fd;
                    return nullptr;
                }
            }

            Position param_end = get_current_position();
            auto param = std::make_unique<Parameter>(std::move(param_name), std::move(default_value), is_rest, param_start, param_end);
            param->set_destructuring_pattern(std::move(destructuring));
            params.push_back(std::move(param));

            if (is_rest) {
                if (!match(TokenType::RIGHT_PAREN)) {
                    add_error("Rest parameter must be last formal parameter");
                    options_.in_generator_body = saved_gen_for_params_fd;
                    return nullptr;
                }
                break;
            }

            if (match(TokenType::COMMA)) {
                advance();
            } else if (!match(TokenType::RIGHT_PAREN)) {
                add_error("Expected ',' or ')' in parameter list");
                options_.in_generator_body = saved_gen_for_params_fd;
                return nullptr;
            }
            continue;
        } else if (current_token().get_type() == TokenType::IDENTIFIER) {
            // ES5: eval and arguments cannot be used as parameter names in strict mode
            if (options_.strict_mode) {
                const std::string& pname = current_token().get_value();
                if (pname == "eval" || pname == "arguments") {
                    add_error("'" + pname + "' cannot be used as a parameter name in strict mode");
                    options_.in_generator_body = saved_gen_for_params_fd;
                    return nullptr;
                }
            }
            param_name = std::make_unique<Identifier>(current_token().get_value(),
                                                      current_token().get_start(), current_token().get_end());
            advance();
        } else if ((current_token().get_type() == TokenType::AWAIT &&
                    !options_.source_type_module) ||
                   (current_token().get_type() == TokenType::YIELD &&
                    !is_generator && !options_.strict_mode) ||
                   ((current_token().get_type() == TokenType::LET ||
                     current_token().get_type() == TokenType::STATIC ||
                     current_token().get_type() == TokenType::UNDEFINED) &&
                    !options_.strict_mode)) {
            param_name = std::make_unique<Identifier>(current_token().get_value(),
                                                      current_token().get_start(), current_token().get_end());
            advance();
        } else {
            add_error("Expected parameter name or destructuring pattern");
            options_.in_generator_body = saved_gen_for_params_fd;
            return nullptr;
        }

        std::unique_ptr<ASTNode> default_value = nullptr;
        if (!is_rest && match(TokenType::ASSIGN)) {
            has_non_simple_params = true;
            advance();
            default_value = parse_assignment_expression();
            if (!default_value) {
                add_error("Invalid default parameter value");
                options_.in_generator_body = saved_gen_for_params_fd;
                return nullptr;
            }
        } else if (is_rest && match(TokenType::ASSIGN)) {
            add_error("Rest parameter cannot have default value");
            options_.in_generator_body = saved_gen_for_params_fd;
            return nullptr;
        }

        Position param_end = get_current_position();
        auto param = std::make_unique<Parameter>(std::move(param_name), std::move(default_value), is_rest, param_start, param_end);
        params.push_back(std::move(param));

        if (is_rest) {
            if (!match(TokenType::RIGHT_PAREN)) {
                add_error("Rest parameter must be last formal parameter");
                options_.in_generator_body = saved_gen_for_params_fd;
                return nullptr;
            }
            break;
        }

        if (match(TokenType::COMMA)) {
            advance();
        } else if (!match(TokenType::RIGHT_PAREN)) {
            add_error("Expected ',' or ')' in parameter list");
            options_.in_generator_body = saved_gen_for_params_fd;
            return nullptr;
        }
    }

    options_.in_generator_body = saved_gen_for_params_fd;
    options_.in_class_static_block = saved_csb_params_fd;

    if (!consume(TokenType::RIGHT_PAREN)) {
        add_error("Expected ')' after parameters");
        return nullptr;
    }

    if (is_generator) {
        std::string forbidden = find_forbidden_expr_in_params(params, true, false);
        if (!forbidden.empty()) {
            add_error("SyntaxError: " + forbidden + " expression not allowed in formal parameters of generator function");
            return nullptr;
        }
    }

    if (!is_generator && options_.source_type_module) {
        std::string forbidden = find_forbidden_expr_in_params(params, false, true);
        if (!forbidden.empty()) {
            add_error("SyntaxError: await expression not allowed in formal parameters of function in module context");
            return nullptr;
        }
    }

    int saved_loop_fn = options_.loop_depth;
    int saved_sw_fn = options_.switch_depth;
    auto saved_al_fn = options_.active_labels;
    auto saved_ll_fn = options_.loop_labels;
    options_.loop_depth = 0;
    options_.switch_depth = 0;
    options_.active_labels.clear();
    options_.loop_labels.clear();
    bool saved_gen_ctx = options_.in_generator_body;
    bool saved_async_fd = options_.in_async_body;
    bool saved_csb_fd = options_.in_class_static_block;
    bool saved_cfi2 = options_.in_class_field_init;
    bool saved_cm2 = options_.in_class_method;
    bool saved_ic_fd = options_.in_constructor;
    options_.in_generator_body = is_generator;
    options_.in_async_body = false;
    options_.in_class_static_block = false;
    options_.in_class_field_init = false;
    options_.in_class_method = false;
    options_.in_constructor = false;
    options_.function_depth++;
    options_.non_arrow_function_depth++;
    auto body = parse_block_statement(true);
    options_.function_depth--;
    options_.non_arrow_function_depth--;
    options_.in_generator_body = saved_gen_ctx;
    options_.in_async_body = saved_async_fd;
    options_.in_class_static_block = saved_csb_fd;
    options_.in_class_field_init = saved_cfi2;
    options_.in_class_method = saved_cm2;
    options_.in_constructor = saved_ic_fd;
    options_.loop_depth = saved_loop_fn;
    options_.switch_depth = saved_sw_fn;
    options_.active_labels = std::move(saved_al_fn);
    options_.loop_labels = std::move(saved_ll_fn);
    if (!body) {
        add_error("Expected function body");
        return nullptr;
    }

    // Duplicate param names: SyntaxError in strict mode OR when params are non-simple
    if (options_.strict_mode || has_non_simple_params) {
        for (size_t pi = 0; pi < params.size(); pi++) {
            if (!params[pi]->get_name()) continue;
            const std::string& pn = params[pi]->get_name()->get_name();
            if (pn.empty() || pn[0] == '_') continue;
            for (size_t pj = pi + 1; pj < params.size(); pj++) {
                if (!params[pj]->get_name()) continue;
                if (params[pj]->get_name()->get_name() == pn) {
                    add_error("SyntaxError: Duplicate parameter name not allowed");
                    return nullptr;
                }
            }
        }
    }


    // ES6: Duplicate binding identifiers inside destructuring params always SyntaxError
    {
        std::unordered_set<std::string> seen_bindings;
        for (const auto& param : params) {
            if (param->has_destructuring()) {
                ASTNode* pattern = param->get_destructuring_pattern();
                if (pattern && pattern->get_type() == ASTNode::Type::DESTRUCTURING_ASSIGNMENT) {
                    DestructuringAssignment* da = static_cast<DestructuringAssignment*>(pattern);
                    if (da->get_type() == DestructuringAssignment::Type::ARRAY) {
                        for (const auto& target : da->get_targets()) {
                            if (!target) continue;
                            const std::string& name = target->get_name();
                            if (name.empty() || name[0] == '_') continue;
                            if (!seen_bindings.insert(name).second) {
                                add_error("Duplicate binding identifier '" + name + "' in destructuring parameter");
                                return nullptr;
                            }
                        }
                    } else {
                        for (const auto& pm : da->get_property_mappings()) {
                            if (pm.variable_name.empty() || pm.variable_name[0] == '_') continue;
                            if (!seen_bindings.insert(pm.variable_name).second) {
                                add_error("Duplicate binding identifier '" + pm.variable_name + "' in destructuring parameter");
                                return nullptr;
                            }
                        }
                    }
                }
            }
        }
    }
    if (body) {
        BlockStatement* block = static_cast<BlockStatement*>(body.get());
        if (block && !block->get_statements().empty()) {
            auto first_stmt = block->get_statements()[0].get();
            if (auto* expr_stmt = dynamic_cast<ExpressionStatement*>(first_stmt)) {
                if (auto* literal = dynamic_cast<StringLiteral*>(expr_stmt->get_expression())) {
                    if (literal->get_value() == "use strict") {
                        if (has_non_simple_params) {
                            add_error("SyntaxError: Illegal 'use strict' directive in function with non-simple parameter list");
                            return nullptr;
                        }
                        if (id) {
                            const std::string& fname = static_cast<Identifier*>(id.get())->get_name();
                            if (fname == "eval" || fname == "arguments") {
                                add_error("SyntaxError: '" + fname + "' cannot be used as function name in strict mode");
                                return nullptr;
                            }
                        }
                        // strict body: eval/arguments param names and duplicate params forbidden
                        for (const auto& p : params) {
                            if (!p->get_name()) continue;
                            const std::string& pn = p->get_name()->get_name();
                            if (pn == "eval" || pn == "arguments") {
                                add_error("SyntaxError: '" + pn + "' cannot be a parameter name in strict mode");
                                return nullptr;
                            }
                        }
                        for (size_t pi = 0; pi < params.size(); pi++) {
                            if (!params[pi]->get_name()) continue;
                            const std::string& pn = params[pi]->get_name()->get_name();
                            if (pn.empty() || pn[0] == '_') continue;
                            for (size_t pj = pi + 1; pj < params.size(); pj++) {
                                if (!params[pj]->get_name()) continue;
                                if (params[pj]->get_name()->get_name() == pn) {
                                    add_error("SyntaxError: Duplicate parameter name not allowed in strict mode");
                                    return nullptr;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    if (options_.strict_mode && body) {
        auto* block = static_cast<BlockStatement*>(body.get());
        std::string dup = check_params_body_lex_conflict(params, block);
        if (!dup.empty()) {
            add_error("SyntaxError: Identifier '" + dup + "' is already declared (parameter name shadows lexical declaration in strict mode)");
            return nullptr;
        }
    }

    Position end = get_current_position();
    auto fn_decl = std::make_unique<FunctionDeclaration>(
        std::move(id), std::move(params),
        std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body.release())),
        start, end, false, is_generator
    );
    fn_decl->set_source_text(get_source_slice(start.offset, last_meaningful_token().get_start().offset + 1));
    return fn_decl;
}

std::unique_ptr<ASTNode> Parser::parse_class_declaration() {
    Position start = get_current_position();
    
    if (!consume(TokenType::CLASS)) {
        add_error("Expected 'class'");
        return nullptr;
    }
    
    // await is valid as class name outside async/module/static-block contexts
    // yield is NEVER valid as class name (class body is always strict)
    bool is_await_name = (current_token().get_type() == TokenType::AWAIT &&
                          !options_.in_async_body && !options_.source_type_module &&
                          !options_.in_class_static_block);
    if (current_token().get_type() != TokenType::IDENTIFIER && !is_await_name) {
        add_error("Expected class name");
        return nullptr;
    }
    if (current_token().has_escaped_keyword()) {
        const std::string& cn = current_token().get_value();
        // Class context is always strict -- reject strict-mode reserved words with escapes
        static const std::unordered_set<std::string> class_name_forbidden = {
            "false","true","null","this","super",
            "break","case","catch","class","const","continue","debugger",
            "default","delete","do","else","export","extends","finally",
            "for","function","if","import","in","instanceof","new",
            "return","switch","throw","try","typeof","var","void","while","with","enum",
            "implements","interface","let","package","private","protected","public","static","yield"
        };
        if (class_name_forbidden.count(cn) ||
            (cn == "await" && options_.source_type_module)) {
            add_error("SyntaxError: '" + cn + "' cannot be used as class name via unicode escape sequences");
            return nullptr;
        }
    }

    std::unique_ptr<ASTNode> id;
    if (is_await_name) {
        std::string name = current_token().get_value();
        Position cs = current_token().get_start(), ce = current_token().get_end();
        advance();
        id = std::make_unique<Identifier>(name, cs, ce);
    } else {
        id = parse_identifier();
        if (!id) return nullptr;
    }

    bool saved_strict_pre = options_.strict_mode;
    options_.strict_mode = true;
    std::unique_ptr<ASTNode> superclass = nullptr;
    if (match(TokenType::EXTENDS)) {
        advance();

        size_t heritage_start_idx = current_token_index_;
        superclass = parse_assignment_expression();
        if (!superclass) {
            add_error("Expected superclass expression after 'extends'");
            return nullptr;
        }
        // An unparenthesized arrow function is not a LeftHandSideExpression -> SyntaxError.
        // If parenthesized (e.g. `(() => {})`), the arrow node starts after the outer `(`.
        {
            auto st = superclass->get_type();
            bool is_arrow = (st == ASTNode::Type::ARROW_FUNCTION_EXPRESSION);
            if (!is_arrow && st == ASTNode::Type::ASYNC_FUNCTION_EXPRESSION) {
                auto* af = static_cast<AsyncFunctionExpression*>(superclass.get());
                if (af->is_arrow()) is_arrow = true;
            }
            if (is_arrow && heritage_start_idx < tokens_.size()) {
                size_t arrow_offset = superclass->get_start().offset;
                size_t first_tok_offset = tokens_[heritage_start_idx].get_start().offset;
                if (arrow_offset == first_tok_offset) {
                    add_error("SyntaxError: Arrow function cannot be used as class heritage");
                    return nullptr;
                }
            }
        }
    }

    if (!match(TokenType::LEFT_BRACE)) {
        add_error("Expected '{' to start class body");
        return nullptr;
    }

    advance();

    bool saved_chh = options_.class_has_heritage;
    bool saved_class_strict = saved_strict_pre;
    options_.class_has_heritage = (superclass != nullptr);
    options_.strict_mode = true;
    options_.class_depth++;
    std::vector<std::unique_ptr<ASTNode>> statements;
    bool seen_constructor = false;

    // Pre-scan to collect declared private names before parsing body (for nested class access)
    {
        std::unordered_set<std::string> pre_declared;
        size_t depth = 1;
        TokenType prev_tt = TokenType::LEFT_BRACE;
        for (size_t i = current_token_index_; i < tokens_.size() && depth > 0; i++) {
            auto tt = tokens_[i].get_type();
            if (tt == TokenType::LEFT_BRACE) { prev_tt = tt; depth++; continue; }
            else if (tt == TokenType::RIGHT_BRACE) { depth--; if (depth == 0) break; prev_tt = tt; continue; }
            else if (tt == TokenType::HASH && depth == 1 &&
                     prev_tt != TokenType::DOT && prev_tt != TokenType::OPTIONAL_CHAINING &&
                     i + 1 < tokens_.size() &&
                     (tokens_[i+1].get_type() == TokenType::IDENTIFIER || is_keyword_token(tokens_[i+1].get_type())) &&
                     tokens_[i+1].get_start().offset == tokens_[i].get_start().offset + 1) {
                pre_declared.insert("#" + tokens_[i+1].get_value());
            }
            prev_tt = tt;
        }
        private_scope_stack_.push_back(std::move(pre_declared));
    }

    while (current_token().get_type() != TokenType::RIGHT_BRACE && !at_end()) {
        if (current_token().get_type() == TokenType::SEMICOLON) {
            advance();
            continue;
        }
        // Skip decorators on class elements
        if (current_token().get_type() == TokenType::AT) {
            skip_decorator_list();
            continue;
        }
        // static { } block
        if (current_token().get_type() == TokenType::STATIC) {
            Position sstart = get_current_position();
            size_t saved = current_token_index_;
            advance();
            if (current_token().get_type() == TokenType::LEFT_BRACE) {
                bool prev_static_block = options_.in_class_static_block;
                bool prev_generator = options_.in_generator_body;
                bool prev_async = options_.in_async_body;
                bool prev_strict = options_.strict_mode;
                bool prev_cm_sb = options_.in_class_method;
                int prev_loop_sb = options_.loop_depth;
                int prev_sw_sb = options_.switch_depth;
                auto prev_al_sb = options_.active_labels;
                auto prev_ll_sb = options_.loop_labels;
                options_.in_class_static_block = true;
                options_.in_class_method = true;
                options_.in_generator_body = false;
                options_.in_async_body = false;
                options_.strict_mode = true;
                options_.loop_depth = 0;
                options_.switch_depth = 0;
                options_.active_labels.clear();
                options_.loop_labels.clear();
                auto block = parse_block_statement(false);
                options_.in_class_static_block = prev_static_block;
                options_.in_class_method = prev_cm_sb;
                options_.in_generator_body = prev_generator;
                options_.in_async_body = prev_async;
                options_.strict_mode = prev_strict;
                options_.loop_depth = prev_loop_sb;
                options_.switch_depth = prev_sw_sb;
                options_.active_labels = std::move(prev_al_sb);
                options_.loop_labels = std::move(prev_ll_sb);
                if (block) {
                    statements.push_back(std::make_unique<ClassStaticBlock>(
                        std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(block.release())),
                        sstart, get_current_position()));
                }
                continue;
            }
            current_token_index_ = saved;
        }
        // #constructor is a reserved private name
        if (current_token().get_type() == TokenType::HASH) {
            size_t h_start = current_token().get_start().offset;
            size_t save_idx = current_token_index_;
            advance();
            if (current_token().get_type() == TokenType::IDENTIFIER &&
                current_token().get_start().offset == h_start + 1 &&
                current_token().get_value() == "constructor") {
                add_error("SyntaxError: '#constructor' is a reserved private name");
                return nullptr;
            }
            current_token_index_ = save_idx;
        }
        if (current_token().get_type() == TokenType::IDENTIFIER ||
            current_token().get_type() == TokenType::STATIC ||
            current_token().get_type() == TokenType::MULTIPLY ||
            current_token().get_type() == TokenType::LEFT_BRACKET ||
            current_token().get_type() == TokenType::HASH ||
            current_token().get_type() == TokenType::STRING ||
            current_token().get_type() == TokenType::NUMBER ||
            current_token().get_type() == TokenType::BIGINT_LITERAL ||
            is_reserved_word_as_property_name()) {
            auto method = parse_method_definition();
            if (method) {
                // Duplicate constructor check
                if (method->get_type() == ASTNode::Type::METHOD_DEFINITION) {
                    auto* md = static_cast<MethodDefinition*>(method.get());
                    if (md->get_kind() == MethodDefinition::CONSTRUCTOR && !md->is_static()) {
                        if (seen_constructor) {
                            add_error("SyntaxError: A class may only have one constructor");
                            return nullptr;
                        }
                        seen_constructor = true;
                    }
                }
                statements.push_back(std::move(method));
            } else {
                advance();
            }
        } else {
            advance();
        }
    }

    if (!match(TokenType::RIGHT_BRACE)) {
        add_error("Expected '}' to close class body");
        return nullptr;
    }

    // Check duplicate private names
    {
        struct PrivEntry { bool is_static; MethodDefinition::Kind kind; };
        std::unordered_map<std::string, PrivEntry> priv_seen;
        for (const auto& stmt : statements) {
            if (!stmt) continue;
            ASTNode* key_node = nullptr;
            bool is_static_m = false;
            MethodDefinition::Kind method_kind = MethodDefinition::METHOD;
            if (stmt->get_type() == ASTNode::Type::METHOD_DEFINITION) {
                auto* md = static_cast<MethodDefinition*>(stmt.get());
                key_node = md->get_key();
                is_static_m = md->is_static();
                method_kind = md->get_kind();
            } else if (stmt->get_type() == ASTNode::Type::CLASS_FIELD) {
                auto* cf = static_cast<ClassField*>(stmt.get());
                key_node = cf->get_key();
                is_static_m = cf->is_static();
            } else {
                continue;
            }
            if (!key_node || key_node->get_type() != ASTNode::Type::IDENTIFIER) continue;
            const std::string& kname = static_cast<Identifier*>(key_node)->get_name();
            if (kname.empty() || kname[0] != '#') continue;
            auto it = priv_seen.find(kname);
            if (it == priv_seen.end()) {
                priv_seen[kname] = {is_static_m, method_kind};
            } else {
                PrivEntry& prev = it->second;
                bool same_static = (prev.is_static == is_static_m);
                bool accessor_pair = same_static &&
                    ((prev.kind == MethodDefinition::GETTER && method_kind == MethodDefinition::SETTER) ||
                     (prev.kind == MethodDefinition::SETTER && method_kind == MethodDefinition::GETTER));
                if (!accessor_pair) {
                    add_error("SyntaxError: Private name '" + kname + "' has already been declared");
                    return nullptr;
                }
                priv_seen.erase(it);
            }
        }
    }

    // AllPrivateNamesValid: every #name used inside the class body must be declared
    // (including names from enclosing class scopes)
    {
        std::unordered_set<std::string> declared;
        for (const auto& stmt : statements) {
            if (!stmt) continue;
            ASTNode* key_node = nullptr;
            bool computed = false;
            if (stmt->get_type() == ASTNode::Type::METHOD_DEFINITION) {
                auto* md = static_cast<MethodDefinition*>(stmt.get());
                key_node = md->get_key(); computed = md->is_computed();
            } else if (stmt->get_type() == ASTNode::Type::CLASS_FIELD) {
                auto* cf = static_cast<ClassField*>(stmt.get());
                key_node = cf->get_key(); computed = cf->is_computed();
            }
            if (!computed && key_node && key_node->get_type() == ASTNode::Type::IDENTIFIER) {
                const auto& n = static_cast<Identifier*>(key_node)->get_name();
                if (!n.empty() && n[0] == '#') declared.insert(n);
            }
        }
        // Include private names from all enclosing scopes (stack already has this class's names from pre-scan)
        std::unordered_set<std::string> all_valid = declared;
        for (const auto& scope : private_scope_stack_)
            all_valid.insert(scope.begin(), scope.end());

        std::string bad_name;
        std::function<void(const ASTNode*)> chk = [&](const ASTNode* nd) {
            if (!nd || !bad_name.empty()) return;
            using T = ASTNode::Type;
            switch (nd->get_type()) {
                case T::CLASS_DECLARATION: return; // nested class has its own private scope
                case T::MEMBER_EXPRESSION: {
                    auto* me = static_cast<const MemberExpression*>(nd);
                    if (!me->is_computed()) {
                        auto* p = me->get_property();
                        if (p && p->get_type() == T::IDENTIFIER) {
                            const auto& nm = static_cast<const Identifier*>(p)->get_name();
                            if (!nm.empty() && nm[0] == '#' && !all_valid.count(nm)) { bad_name = nm; return; }
                        }
                    }
                    chk(me->get_object()); if (me->is_computed()) chk(me->get_property()); break;
                }
                case T::OPTIONAL_CHAINING_EXPRESSION: {
                    auto* oc = static_cast<const OptionalChainingExpression*>(nd);
                    if (!oc->is_computed()) {
                        auto* p = oc->get_property();
                        if (p && p->get_type() == T::IDENTIFIER) {
                            const auto& nm = static_cast<const Identifier*>(p)->get_name();
                            if (!nm.empty() && nm[0] == '#' && !all_valid.count(nm)) { bad_name = nm; return; }
                        }
                    }
                    chk(oc->get_object()); if (oc->is_computed()) chk(oc->get_property()); break;
                }
                case T::BINARY_EXPRESSION: {
                    auto* be = static_cast<const BinaryExpression*>(nd);
                    if (be->get_operator() == BinaryExpression::Operator::IN) {
                        auto* lft = be->get_left();
                        if (lft && lft->get_type() == T::IDENTIFIER) {
                            const auto& nm = static_cast<const Identifier*>(lft)->get_name();
                            if (!nm.empty() && nm[0] == '#' && !all_valid.count(nm)) { bad_name = nm; return; }
                        }
                    }
                    chk(be->get_left()); chk(be->get_right()); break;
                }
                case T::UNARY_EXPRESSION: chk(static_cast<const UnaryExpression*>(nd)->get_operand()); break;
                case T::ASSIGNMENT_EXPRESSION: { auto* ae = static_cast<const AssignmentExpression*>(nd); chk(ae->get_left()); chk(ae->get_right()); break; }
                case T::CONDITIONAL_EXPRESSION: { auto* ce = static_cast<const ConditionalExpression*>(nd); chk(ce->get_test()); chk(ce->get_consequent()); chk(ce->get_alternate()); break; }
                case T::CALL_EXPRESSION: { auto* ce = static_cast<const CallExpression*>(nd); chk(ce->get_callee()); for (const auto& a : ce->get_arguments()) chk(a.get()); break; }
                case T::NEW_EXPRESSION: { auto* ne = static_cast<const NewExpression*>(nd); chk(ne->get_constructor()); for (const auto& a : ne->get_arguments()) chk(a.get()); break; }
                case T::AWAIT_EXPRESSION: chk(static_cast<const AwaitExpression*>(nd)->get_argument()); break;
                case T::YIELD_EXPRESSION: chk(static_cast<const YieldExpression*>(nd)->get_argument()); break;
                case T::SPREAD_ELEMENT: chk(static_cast<const SpreadElement*>(nd)->get_argument()); break;
                case T::NULLISH_COALESCING_EXPRESSION: { auto* nc = static_cast<const NullishCoalescingExpression*>(nd); chk(nc->get_left()); chk(nc->get_right()); break; }
                case T::ARRAY_LITERAL: for (const auto& e : static_cast<const ArrayLiteral*>(nd)->get_elements()) chk(e.get()); break;
                case T::OBJECT_LITERAL: for (const auto& pr : static_cast<const ObjectLiteral*>(nd)->get_properties()) { if (pr->computed) chk(pr->key.get()); chk(pr->value.get()); } break;
                case T::FUNCTION_EXPRESSION: { auto* fe = static_cast<const FunctionExpression*>(nd); chk(fe->get_body()); for (const auto& p : fe->get_params()) if (p->has_default()) chk(p->get_default_value()); break; }
                case T::ARROW_FUNCTION_EXPRESSION: { auto* af = static_cast<const ArrowFunctionExpression*>(nd); chk(af->get_body()); for (const auto& p : af->get_params()) if (p->has_default()) chk(p->get_default_value()); break; }
                case T::ASYNC_FUNCTION_EXPRESSION: { auto* af = static_cast<const AsyncFunctionExpression*>(nd); chk(af->get_body()); for (const auto& p : af->get_params()) if (p->has_default()) chk(p->get_default_value()); break; }
                case T::FUNCTION_DECLARATION: { auto* fd = static_cast<const FunctionDeclaration*>(nd); chk(fd->get_body()); for (const auto& p : fd->get_params()) if (p->has_default()) chk(p->get_default_value()); break; }
                case T::EXPRESSION_STATEMENT: chk(static_cast<const ExpressionStatement*>(nd)->get_expression()); break;
                case T::BLOCK_STATEMENT: for (const auto& s : static_cast<const BlockStatement*>(nd)->get_statements()) chk(s.get()); break;
                case T::RETURN_STATEMENT: chk(static_cast<const ReturnStatement*>(nd)->get_argument()); break;
                case T::THROW_STATEMENT: chk(static_cast<const ThrowStatement*>(nd)->get_expression()); break;
                case T::IF_STATEMENT: { auto* is = static_cast<const IfStatement*>(nd); chk(is->get_test()); chk(is->get_consequent()); if (is->has_alternate()) chk(is->get_alternate()); break; }
                case T::FOR_STATEMENT: { auto* fs = static_cast<const ForStatement*>(nd); chk(fs->get_init()); chk(fs->get_test()); chk(fs->get_update()); chk(fs->get_body()); break; }
                case T::FOR_IN_STATEMENT: { auto* fi = static_cast<const ForInStatement*>(nd); chk(fi->get_left()); chk(fi->get_right()); chk(fi->get_body()); break; }
                case T::FOR_OF_STATEMENT: { auto* fo = static_cast<const ForOfStatement*>(nd); chk(fo->get_left()); chk(fo->get_right()); chk(fo->get_body()); break; }
                case T::WHILE_STATEMENT: { auto* ws = static_cast<const WhileStatement*>(nd); chk(ws->get_test()); chk(ws->get_body()); break; }
                case T::DO_WHILE_STATEMENT: { auto* dw = static_cast<const DoWhileStatement*>(nd); chk(dw->get_body()); chk(dw->get_test()); break; }
                case T::WITH_STATEMENT: { auto* wi = static_cast<const WithStatement*>(nd); chk(wi->get_object()); chk(wi->get_body()); break; }
                case T::LABELED_STATEMENT: chk(static_cast<const LabeledStatement*>(nd)->get_statement()); break;
                case T::TRY_STATEMENT: { auto* ts = static_cast<const TryStatement*>(nd); chk(ts->get_try_block()); chk(ts->get_catch_clause()); chk(ts->get_finally_block()); break; }
                case T::CATCH_CLAUSE: chk(static_cast<const CatchClause*>(nd)->get_body()); break;
                case T::SWITCH_STATEMENT: { auto* ss = static_cast<const SwitchStatement*>(nd); chk(ss->get_discriminant()); for (const auto& c : ss->get_cases()) chk(c.get()); break; }
                case T::CASE_CLAUSE: { auto* cc = static_cast<const CaseClause*>(nd); chk(cc->get_test()); for (const auto& s : cc->get_consequent()) chk(s.get()); break; }
                case T::VARIABLE_DECLARATION: for (const auto& d : static_cast<const VariableDeclaration*>(nd)->get_declarations()) chk(d->get_init()); break;
                case T::METHOD_DEFINITION: { auto* md = static_cast<const MethodDefinition*>(nd); if (md->is_computed()) chk(md->get_key()); chk(md->get_value()); break; }
                case T::CLASS_FIELD: { auto* cf = static_cast<const ClassField*>(nd); if (cf->is_computed()) chk(cf->get_key()); chk(cf->get_value()); break; }
                case T::CLASS_STATIC_BLOCK: chk(static_cast<const ClassStaticBlock*>(nd)->get_body()); break;
                case T::USING_DECLARATION: for (const auto& b : static_cast<const UsingDeclaration*>(nd)->get_bindings()) chk(b.initializer.get()); break;
                default: break;
            }
        };

        for (const auto& stmt : statements) {
            chk(stmt.get());
            if (!bad_name.empty()) {
                private_scope_stack_.pop_back();
                add_error("SyntaxError: Private name '" + bad_name + "' is not defined");
                return nullptr;
            }
        }
        private_scope_stack_.pop_back();
    }

    advance();

    options_.class_has_heritage = saved_chh;
    options_.strict_mode = saved_class_strict;
    options_.class_depth--;

    auto body = std::make_unique<BlockStatement>(std::move(statements), start, get_current_position());

    Position end = get_current_position();

    std::string class_src = get_source_slice(start.offset, last_meaningful_token().get_start().offset + 1);
    std::unique_ptr<ClassDeclaration> cls_decl;
    if (superclass) {
        cls_decl = std::make_unique<ClassDeclaration>(
            std::unique_ptr<Identifier>(static_cast<Identifier*>(id.release())),
            std::move(superclass),
            std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body.release())),
            start, end
        );
    } else {
        cls_decl = std::make_unique<ClassDeclaration>(
            std::unique_ptr<Identifier>(static_cast<Identifier*>(id.release())),
            std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body.release())),
            start, end
        );
    }
    cls_decl->set_source_text(class_src);
    return cls_decl;
}

std::unique_ptr<ASTNode> Parser::parse_class_expression() {
    Position start = get_current_position();

    if (!consume(TokenType::CLASS)) {
        add_error("Expected 'class'");
        return nullptr;
    }

    std::unique_ptr<ASTNode> id = nullptr;
    {
        bool await_name_ok = (current_token().get_type() == TokenType::AWAIT &&
                              !options_.in_async_body && !options_.source_type_module &&
                              !options_.in_class_static_block);
        bool yield_name_ok = false; // yield is never valid as class name (class body is always strict)
        if (current_token().get_type() == TokenType::IDENTIFIER || await_name_ok || yield_name_ok) {
            if (current_token().has_escaped_keyword()) {
                const std::string& cn = current_token().get_value();
                static const std::unordered_set<std::string> class_expr_name_forbidden = {
                    "false","true","null","this","super",
                    "break","case","catch","class","const","continue","debugger",
                    "default","delete","do","else","export","extends","finally",
                    "for","function","if","import","in","instanceof","new",
                    "return","switch","throw","try","typeof","var","void","while","with","enum",
                    "implements","interface","let","package","private","protected","public","static","yield"
                };
                if (class_expr_name_forbidden.count(cn) ||
                    (cn == "await" && options_.source_type_module)) {
                    add_error("SyntaxError: '" + cn + "' cannot be used as class name via unicode escape sequences");
                    return nullptr;
                }
            }
            if (await_name_ok || yield_name_ok) {
                std::string name = current_token().get_value();
                Position cs = current_token().get_start(), ce = current_token().get_end();
                advance();
                id = std::make_unique<Identifier>(name, cs, ce);
            } else {
                id = parse_identifier();
                if (!id) return nullptr;
            }
        }
    }

    bool saved_strict_pre2 = options_.strict_mode;
    options_.strict_mode = true;
    std::unique_ptr<ASTNode> superclass = nullptr;
    if (match(TokenType::EXTENDS)) {
        advance();

        size_t heritage_start_idx2 = current_token_index_;
        superclass = parse_assignment_expression();
        if (!superclass) {
            add_error("Expected superclass expression after 'extends'");
            return nullptr;
        }
        {
            auto st2 = superclass->get_type();
            bool is_arrow2 = (st2 == ASTNode::Type::ARROW_FUNCTION_EXPRESSION);
            if (!is_arrow2 && st2 == ASTNode::Type::ASYNC_FUNCTION_EXPRESSION) {
                auto* af2 = static_cast<AsyncFunctionExpression*>(superclass.get());
                if (af2->is_arrow()) is_arrow2 = true;
            }
            if (is_arrow2 && heritage_start_idx2 < tokens_.size()) {
                size_t arrow_offset2 = superclass->get_start().offset;
                size_t first_tok_offset2 = tokens_[heritage_start_idx2].get_start().offset;
                if (arrow_offset2 == first_tok_offset2) {
                    add_error("SyntaxError: Arrow function cannot be used as class heritage");
                    return nullptr;
                }
            }
        }
    }

    if (!match(TokenType::LEFT_BRACE)) {
        add_error("Expected '{' to start class body");
        return nullptr;
    }

    advance();

    bool saved_chh2 = options_.class_has_heritage;
    bool saved_class_strict2 = saved_strict_pre2;
    options_.class_has_heritage = (superclass != nullptr);
    options_.strict_mode = true;
    options_.class_depth++;
    std::vector<std::unique_ptr<ASTNode>> statements;
    bool seen_ctor2 = false;

    // Pre-scan to collect declared private names before parsing body (for nested class access)
    {
        std::unordered_set<std::string> pre_declared;
        size_t depth = 1;
        TokenType prev_tt = TokenType::LEFT_BRACE;
        for (size_t i = current_token_index_; i < tokens_.size() && depth > 0; i++) {
            auto tt = tokens_[i].get_type();
            if (tt == TokenType::LEFT_BRACE) { prev_tt = tt; depth++; continue; }
            else if (tt == TokenType::RIGHT_BRACE) { depth--; if (depth == 0) break; prev_tt = tt; continue; }
            else if (tt == TokenType::HASH && depth == 1 &&
                     prev_tt != TokenType::DOT && prev_tt != TokenType::OPTIONAL_CHAINING &&
                     i + 1 < tokens_.size() &&
                     (tokens_[i+1].get_type() == TokenType::IDENTIFIER || is_keyword_token(tokens_[i+1].get_type())) &&
                     tokens_[i+1].get_start().offset == tokens_[i].get_start().offset + 1) {
                pre_declared.insert("#" + tokens_[i+1].get_value());
            }
            prev_tt = tt;
        }
        private_scope_stack_.push_back(std::move(pre_declared));
    }

    while (current_token().get_type() != TokenType::RIGHT_BRACE && !at_end()) {
        if (current_token().get_type() == TokenType::SEMICOLON) {
            advance();
            continue;
        }
        // Skip decorators on class elements
        if (current_token().get_type() == TokenType::AT) {
            skip_decorator_list();
            continue;
        }
        // static { } block
        if (current_token().get_type() == TokenType::STATIC) {
            Position sstart2 = get_current_position();
            size_t saved2 = current_token_index_;
            advance();
            if (current_token().get_type() == TokenType::LEFT_BRACE) {
                bool prev_static_block2 = options_.in_class_static_block;
                bool prev_generator2 = options_.in_generator_body;
                bool prev_async2 = options_.in_async_body;
                bool prev_strict2 = options_.strict_mode;
                bool prev_cm_sb2 = options_.in_class_method;
                int prev_loop_sb2 = options_.loop_depth;
                int prev_sw_sb2 = options_.switch_depth;
                auto prev_al_sb2 = options_.active_labels;
                auto prev_ll_sb2 = options_.loop_labels;
                options_.in_class_static_block = true;
                options_.in_class_method = true;
                options_.in_generator_body = false;
                options_.in_async_body = false;
                options_.strict_mode = true;
                options_.loop_depth = 0;
                options_.switch_depth = 0;
                options_.active_labels.clear();
                options_.loop_labels.clear();
                auto sblock = parse_block_statement(false);
                options_.in_class_static_block = prev_static_block2;
                options_.in_class_method = prev_cm_sb2;
                options_.in_generator_body = prev_generator2;
                options_.in_async_body = prev_async2;
                options_.strict_mode = prev_strict2;
                options_.loop_depth = prev_loop_sb2;
                options_.switch_depth = prev_sw_sb2;
                options_.active_labels = std::move(prev_al_sb2);
                options_.loop_labels = std::move(prev_ll_sb2);
                if (sblock) {
                    statements.push_back(std::make_unique<ClassStaticBlock>(
                        std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(sblock.release())),
                        sstart2, get_current_position()));
                }
                continue;
            }
            current_token_index_ = saved2;
        }
        // #constructor is reserved
        if (current_token().get_type() == TokenType::HASH) {
            size_t h2 = current_token().get_start().offset;
            size_t si2 = current_token_index_;
            advance();
            if (current_token().get_type() == TokenType::IDENTIFIER &&
                current_token().get_start().offset == h2 + 1 &&
                current_token().get_value() == "constructor") {
                add_error("SyntaxError: '#constructor' is a reserved private name");
                return nullptr;
            }
            current_token_index_ = si2;
        }
        if (current_token().get_type() == TokenType::IDENTIFIER ||
            current_token().get_type() == TokenType::STATIC ||
            current_token().get_type() == TokenType::MULTIPLY ||
            current_token().get_type() == TokenType::LEFT_BRACKET ||
            current_token().get_type() == TokenType::HASH ||
            current_token().get_type() == TokenType::STRING ||
            current_token().get_type() == TokenType::NUMBER ||
            current_token().get_type() == TokenType::BIGINT_LITERAL ||
            is_reserved_word_as_property_name()) {
            auto method = parse_method_definition();
            if (method) {
                if (method->get_type() == ASTNode::Type::METHOD_DEFINITION) {
                    auto* md = static_cast<MethodDefinition*>(method.get());
                    if (md->get_kind() == MethodDefinition::CONSTRUCTOR && !md->is_static()) {
                        if (seen_ctor2) {
                            add_error("SyntaxError: A class may only have one constructor");
                            return nullptr;
                        }
                        seen_ctor2 = true;
                    }
                }
                statements.push_back(std::move(method));
            } else {
                advance();
            }
        } else {
            advance();
        }
    }

    if (!match(TokenType::RIGHT_BRACE)) {
        add_error("Expected '}' to close class body");
        return nullptr;
    }

    // Check duplicate private names
    {
        struct PrivEntry2 { bool is_static; MethodDefinition::Kind kind; };
        std::unordered_map<std::string, PrivEntry2> priv_seen2;
        for (const auto& stmt : statements) {
            if (!stmt) continue;
            ASTNode* key_node = nullptr;
            bool is_static_m = false;
            MethodDefinition::Kind method_kind = MethodDefinition::METHOD;
            if (stmt->get_type() == ASTNode::Type::METHOD_DEFINITION) {
                auto* md = static_cast<MethodDefinition*>(stmt.get());
                key_node = md->get_key();
                is_static_m = md->is_static();
                method_kind = md->get_kind();
            } else if (stmt->get_type() == ASTNode::Type::CLASS_FIELD) {
                auto* cf = static_cast<ClassField*>(stmt.get());
                key_node = cf->get_key();
                is_static_m = cf->is_static();
            } else {
                continue;
            }
            if (!key_node || key_node->get_type() != ASTNode::Type::IDENTIFIER) continue;
            const std::string& kname = static_cast<Identifier*>(key_node)->get_name();
            if (kname.empty() || kname[0] != '#') continue;
            auto it = priv_seen2.find(kname);
            if (it == priv_seen2.end()) {
                priv_seen2[kname] = {is_static_m, method_kind};
            } else {
                PrivEntry2& prev = it->second;
                bool same_static = (prev.is_static == is_static_m);
                bool accessor_pair = same_static &&
                    ((prev.kind == MethodDefinition::GETTER && method_kind == MethodDefinition::SETTER) ||
                     (prev.kind == MethodDefinition::SETTER && method_kind == MethodDefinition::GETTER));
                if (!accessor_pair) {
                    add_error("SyntaxError: Private name '" + kname + "' has already been declared");
                    return nullptr;
                }
                priv_seen2.erase(it);
            }
        }
    }

    // AllPrivateNamesValid check 
    {
        std::unordered_set<std::string> declared;
        for (const auto& stmt : statements) {
            if (!stmt) continue;
            ASTNode* key_node = nullptr; bool computed = false;
            if (stmt->get_type() == ASTNode::Type::METHOD_DEFINITION) { auto* md = static_cast<MethodDefinition*>(stmt.get()); key_node = md->get_key(); computed = md->is_computed(); }
            else if (stmt->get_type() == ASTNode::Type::CLASS_FIELD) { auto* cf = static_cast<ClassField*>(stmt.get()); key_node = cf->get_key(); computed = cf->is_computed(); }
            if (!computed && key_node && key_node->get_type() == ASTNode::Type::IDENTIFIER) {
                const auto& n = static_cast<Identifier*>(key_node)->get_name();
                if (!n.empty() && n[0] == '#') declared.insert(n);
            }
        }
        // Include names from all enclosing scopes (stack already has this class's names from pre-scan)
        std::unordered_set<std::string> all_valid = declared;
        for (const auto& scope : private_scope_stack_)
            all_valid.insert(scope.begin(), scope.end());
        std::string bad_name;
        std::function<void(const ASTNode*)> chk = [&](const ASTNode* nd) {
            if (!nd || !bad_name.empty()) return;
            using T = ASTNode::Type;
            switch (nd->get_type()) {
                case T::CLASS_DECLARATION: return;
                case T::MEMBER_EXPRESSION: { auto* me = static_cast<const MemberExpression*>(nd); if (!me->is_computed()) { auto* p = me->get_property(); if (p && p->get_type() == T::IDENTIFIER) { const auto& nm = static_cast<const Identifier*>(p)->get_name(); if (!nm.empty() && nm[0] == '#' && !all_valid.count(nm)) { bad_name = nm; return; } } } chk(me->get_object()); if (me->is_computed()) chk(me->get_property()); break; }
                case T::OPTIONAL_CHAINING_EXPRESSION: { auto* oc = static_cast<const OptionalChainingExpression*>(nd); if (!oc->is_computed()) { auto* p = oc->get_property(); if (p && p->get_type() == T::IDENTIFIER) { const auto& nm = static_cast<const Identifier*>(p)->get_name(); if (!nm.empty() && nm[0] == '#' && !all_valid.count(nm)) { bad_name = nm; return; } } } chk(oc->get_object()); if (oc->is_computed()) chk(oc->get_property()); break; }
                case T::BINARY_EXPRESSION: { auto* be = static_cast<const BinaryExpression*>(nd); if (be->get_operator() == BinaryExpression::Operator::IN) { auto* lft = be->get_left(); if (lft && lft->get_type() == T::IDENTIFIER) { const auto& nm = static_cast<const Identifier*>(lft)->get_name(); if (!nm.empty() && nm[0] == '#' && !all_valid.count(nm)) { bad_name = nm; return; } } } chk(be->get_left()); chk(be->get_right()); break; }
                case T::UNARY_EXPRESSION: chk(static_cast<const UnaryExpression*>(nd)->get_operand()); break;
                case T::ASSIGNMENT_EXPRESSION: { auto* ae = static_cast<const AssignmentExpression*>(nd); chk(ae->get_left()); chk(ae->get_right()); break; }
                case T::CONDITIONAL_EXPRESSION: { auto* ce = static_cast<const ConditionalExpression*>(nd); chk(ce->get_test()); chk(ce->get_consequent()); chk(ce->get_alternate()); break; }
                case T::CALL_EXPRESSION: { auto* ce = static_cast<const CallExpression*>(nd); chk(ce->get_callee()); for (const auto& a : ce->get_arguments()) chk(a.get()); break; }
                case T::NEW_EXPRESSION: { auto* ne = static_cast<const NewExpression*>(nd); chk(ne->get_constructor()); for (const auto& a : ne->get_arguments()) chk(a.get()); break; }
                case T::AWAIT_EXPRESSION: chk(static_cast<const AwaitExpression*>(nd)->get_argument()); break;
                case T::YIELD_EXPRESSION: chk(static_cast<const YieldExpression*>(nd)->get_argument()); break;
                case T::SPREAD_ELEMENT: chk(static_cast<const SpreadElement*>(nd)->get_argument()); break;
                case T::NULLISH_COALESCING_EXPRESSION: { auto* nc = static_cast<const NullishCoalescingExpression*>(nd); chk(nc->get_left()); chk(nc->get_right()); break; }
                case T::ARRAY_LITERAL: for (const auto& e : static_cast<const ArrayLiteral*>(nd)->get_elements()) chk(e.get()); break;
                case T::OBJECT_LITERAL: for (const auto& pr : static_cast<const ObjectLiteral*>(nd)->get_properties()) { if (pr->computed) chk(pr->key.get()); chk(pr->value.get()); } break;
                case T::FUNCTION_EXPRESSION: { auto* fe = static_cast<const FunctionExpression*>(nd); chk(fe->get_body()); for (const auto& p : fe->get_params()) if (p->has_default()) chk(p->get_default_value()); break; }
                case T::ARROW_FUNCTION_EXPRESSION: { auto* af = static_cast<const ArrowFunctionExpression*>(nd); chk(af->get_body()); for (const auto& p : af->get_params()) if (p->has_default()) chk(p->get_default_value()); break; }
                case T::ASYNC_FUNCTION_EXPRESSION: { auto* af = static_cast<const AsyncFunctionExpression*>(nd); chk(af->get_body()); for (const auto& p : af->get_params()) if (p->has_default()) chk(p->get_default_value()); break; }
                case T::FUNCTION_DECLARATION: { auto* fd = static_cast<const FunctionDeclaration*>(nd); chk(fd->get_body()); for (const auto& p : fd->get_params()) if (p->has_default()) chk(p->get_default_value()); break; }
                case T::EXPRESSION_STATEMENT: chk(static_cast<const ExpressionStatement*>(nd)->get_expression()); break;
                case T::BLOCK_STATEMENT: for (const auto& s : static_cast<const BlockStatement*>(nd)->get_statements()) chk(s.get()); break;
                case T::RETURN_STATEMENT: chk(static_cast<const ReturnStatement*>(nd)->get_argument()); break;
                case T::THROW_STATEMENT: chk(static_cast<const ThrowStatement*>(nd)->get_expression()); break;
                case T::IF_STATEMENT: { auto* is = static_cast<const IfStatement*>(nd); chk(is->get_test()); chk(is->get_consequent()); if (is->has_alternate()) chk(is->get_alternate()); break; }
                case T::FOR_STATEMENT: { auto* fs = static_cast<const ForStatement*>(nd); chk(fs->get_init()); chk(fs->get_test()); chk(fs->get_update()); chk(fs->get_body()); break; }
                case T::FOR_IN_STATEMENT: { auto* fi = static_cast<const ForInStatement*>(nd); chk(fi->get_left()); chk(fi->get_right()); chk(fi->get_body()); break; }
                case T::FOR_OF_STATEMENT: { auto* fo = static_cast<const ForOfStatement*>(nd); chk(fo->get_left()); chk(fo->get_right()); chk(fo->get_body()); break; }
                case T::WHILE_STATEMENT: { auto* ws = static_cast<const WhileStatement*>(nd); chk(ws->get_test()); chk(ws->get_body()); break; }
                case T::DO_WHILE_STATEMENT: { auto* dw = static_cast<const DoWhileStatement*>(nd); chk(dw->get_body()); chk(dw->get_test()); break; }
                case T::WITH_STATEMENT: { auto* wi = static_cast<const WithStatement*>(nd); chk(wi->get_object()); chk(wi->get_body()); break; }
                case T::LABELED_STATEMENT: chk(static_cast<const LabeledStatement*>(nd)->get_statement()); break;
                case T::TRY_STATEMENT: { auto* ts = static_cast<const TryStatement*>(nd); chk(ts->get_try_block()); chk(ts->get_catch_clause()); chk(ts->get_finally_block()); break; }
                case T::CATCH_CLAUSE: chk(static_cast<const CatchClause*>(nd)->get_body()); break;
                case T::SWITCH_STATEMENT: { auto* ss = static_cast<const SwitchStatement*>(nd); chk(ss->get_discriminant()); for (const auto& c : ss->get_cases()) chk(c.get()); break; }
                case T::CASE_CLAUSE: { auto* cc = static_cast<const CaseClause*>(nd); chk(cc->get_test()); for (const auto& s : cc->get_consequent()) chk(s.get()); break; }
                case T::VARIABLE_DECLARATION: for (const auto& d : static_cast<const VariableDeclaration*>(nd)->get_declarations()) chk(d->get_init()); break;
                case T::METHOD_DEFINITION: { auto* md = static_cast<const MethodDefinition*>(nd); if (md->is_computed()) chk(md->get_key()); chk(md->get_value()); break; }
                case T::CLASS_FIELD: { auto* cf = static_cast<const ClassField*>(nd); if (cf->is_computed()) chk(cf->get_key()); chk(cf->get_value()); break; }
                case T::CLASS_STATIC_BLOCK: chk(static_cast<const ClassStaticBlock*>(nd)->get_body()); break;
                case T::USING_DECLARATION: for (const auto& b : static_cast<const UsingDeclaration*>(nd)->get_bindings()) chk(b.initializer.get()); break;
                default: break;
            }
        };
        for (const auto& stmt : statements) {
            chk(stmt.get());
            if (!bad_name.empty()) { private_scope_stack_.pop_back(); add_error("SyntaxError: Private name '" + bad_name + "' is not defined"); return nullptr; }
        }
        private_scope_stack_.pop_back();
    }

    advance();

    options_.class_has_heritage = saved_chh2;
    options_.strict_mode = saved_class_strict2;
    options_.class_depth--;

    auto body = std::make_unique<BlockStatement>(std::move(statements), start, get_current_position());

    Position end = get_current_position();

    std::string cls_expr_src = get_source_slice(start.offset, last_meaningful_token().get_start().offset + 1);
    std::unique_ptr<ClassDeclaration> cls_expr;
    if (id) {
        if (superclass) {
            cls_expr = std::make_unique<ClassDeclaration>(
                std::unique_ptr<Identifier>(static_cast<Identifier*>(id.release())),
                std::move(superclass),
                std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body.release())),
                start, end
            );
        } else {
            cls_expr = std::make_unique<ClassDeclaration>(
                std::unique_ptr<Identifier>(static_cast<Identifier*>(id.release())),
                std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body.release())),
                start, end
            );
        }
    } else {
        auto anonymous_id = std::make_unique<Identifier>("", start, start);
        if (superclass) {
            cls_expr = std::make_unique<ClassDeclaration>(
                std::move(anonymous_id),
                std::move(superclass),
                std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body.release())),
                start, end
            );
        } else {
            cls_expr = std::make_unique<ClassDeclaration>(
                std::move(anonymous_id),
                std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body.release())),
                start, end
            );
        }
    }
    cls_expr->set_source_text(cls_expr_src);
    cls_expr->set_is_expression(true);
    return cls_expr;
}

std::unique_ptr<ASTNode> Parser::parse_method_definition() {
    Position start = get_current_position();
    // toString's source text must exclude the `static` modifier (NativeFunction syntax
    // starts at the method name) -- src_start tracks where the name actually begins.
    Position src_start = start;

    bool is_static = false;
    if (current_token().get_value() == "static" && current_token().get_type() != TokenType::STATIC &&
        !current_token().has_escaped_keyword()) {
        is_static = true;
        advance();
        src_start = get_current_position();
    } else if (current_token().get_type() == TokenType::STATIC) {
        // Peek: if followed by =, ;, }, or newline, this is a FIELD named 'static', not the modifier
        TokenType nxt = peek_token().get_type();
        bool static_is_field = (nxt == TokenType::ASSIGN || nxt == TokenType::SEMICOLON ||
                                nxt == TokenType::RIGHT_BRACE || nxt == TokenType::EOF_TOKEN ||
                                nxt == TokenType::NEWLINE);
        if (!static_is_field) {
            // Also check if next token is on a different line (ASI)
            size_t static_line = current_token().get_end().line;
            if (peek_token().get_start().line > static_line) static_is_field = true;
        }
        if (static_is_field) {
            // 'static' is the field name, not the modifier — fall through with is_static=false
        } else {
            is_static = true;
            advance();
            src_start = get_current_position();
        }
    }

    bool is_async = false;
    if (current_token().get_type() == TokenType::ASYNC) {
        is_async = true;
        advance();
    } else if (current_token().get_type() == TokenType::IDENTIFIER &&
               current_token().get_value() == "async" &&
               current_token().has_escaped_keyword()) {
        size_t async_end_line = current_token().get_end().line;
        TokenType next = peek_token().get_type();
        if (next == TokenType::MULTIPLY || next == TokenType::IDENTIFIER ||
            next == TokenType::LEFT_BRACKET || next == TokenType::HASH ||
            next == TokenType::STRING || next == TokenType::NUMBER) {
            add_error("SyntaxError: `async` cannot contain unicode escape sequences");
            return nullptr;
        }
    }

    bool is_generator = false;
    if (current_token().get_type() == TokenType::MULTIPLY) {
        is_generator = true;
        advance();
    }

    MethodDefinition::Kind method_kind = MethodDefinition::METHOD;
    if (current_token().get_type() == TokenType::IDENTIFIER) {
        std::string token_value = current_token().get_value();
        if (token_value == "get" || token_value == "set") {
            // Only treat as getter/setter if next token is a valid property name (not '(' or '=' or ';')
            TokenType next = peek_token().get_type();
            bool is_accessor = (next == TokenType::IDENTIFIER || next == TokenType::HASH ||
                                next == TokenType::STRING || next == TokenType::NUMBER ||
                                next == TokenType::LEFT_BRACKET || is_keyword_token(next));
            if (is_accessor) {
                method_kind = (token_value == "get") ? MethodDefinition::GETTER : MethodDefinition::SETTER;
                advance();
            }
        }
    }

    std::unique_ptr<ASTNode> key = nullptr;
    bool computed = false;
    bool is_private = false;

    if (current_token().get_type() == TokenType::HASH) {
        is_private = true;
        size_t hash_start = current_token().get_start().offset;
        advance();

        // Private names allow any IdentifierName (including reserved words like yield/await)
        if (current_token().get_type() != TokenType::IDENTIFIER && !is_keyword_token(current_token().get_type())) {
            add_error("Expected identifier after '#' for private member");
            return nullptr;
        }
        if (current_token().get_start().offset != hash_start + 1) {
            add_error("SyntaxError: Whitespace is not allowed between '#' and identifier");
            return nullptr;
        }

        std::string private_name = "#" + current_token().get_value();
        if (private_name == "#constructor") {
            add_error("SyntaxError: '#constructor' is a reserved private name");
            return nullptr;
        }
        Position start = current_token().get_start();
        Position end = current_token().get_end();
        advance();
        key = std::make_unique<Identifier>(private_name, start, end);
    } else if (current_token().get_type() == TokenType::IDENTIFIER) {
        if (current_token().has_escaped_keyword()) {
            std::string name = current_token().get_value();
            Position s = current_token().get_start();
            Position e = current_token().get_end();
            advance();
            key = std::make_unique<Identifier>(name, s, e);
        } else {
            key = parse_identifier();
        }
    } else if (is_reserved_word_as_property_name()) {
        std::string name = current_token().get_value();
        Position start = current_token().get_start();
        Position end = current_token().get_end();
        advance();
        key = std::make_unique<Identifier>(name, start, end);
    } else if (current_token().get_type() == TokenType::STRING) {
        key = parse_string_literal();
    } else if (current_token().get_type() == TokenType::NUMBER) {
        key = parse_number_literal();
    } else if (current_token().get_type() == TokenType::BIGINT_LITERAL) {
        key = parse_bigint_literal();
    } else if (current_token().get_type() == TokenType::LEFT_BRACKET) {
        computed = true;
        advance();

        // Computed property keys always allow 'in' regardless of outer for-loop context
        bool saved_no_in = no_in_mode_;
        no_in_mode_ = false;
        key = parse_assignment_expression();
        no_in_mode_ = saved_no_in;

        if (!key) {
            add_error("Failed to parse computed property expression");
            return nullptr;
        }
        if (!consume(TokenType::RIGHT_BRACKET)) {
            add_error("Expected ']' after computed property");
            return nullptr;
        }
    } else {
        add_error("Expected method name or computed property");
        return nullptr;
    }

    if (!key) return nullptr;

    // Handle `accessor FieldName` syntax (auto-accessor fields proposal)
    // When key is identifier "accessor" and next non-ws token on same line is a valid field name, treat as accessor field
    if (!computed && key->get_type() == ASTNode::Type::IDENTIFIER &&
        static_cast<Identifier*>(key.get())->get_name() == "accessor" &&
        !is_generator && !is_async && method_kind == MethodDefinition::METHOD) {
        TokenType nxt = current_token().get_type();
        size_t accessor_end_line = key->get_end().line;
        bool on_same_line = (current_token().get_start().line == accessor_end_line);
        bool valid_name = on_same_line && (nxt == TokenType::IDENTIFIER || nxt == TokenType::HASH ||
                          nxt == TokenType::STRING || nxt == TokenType::NUMBER ||
                          nxt == TokenType::LEFT_BRACKET || is_keyword_token(nxt));
        if (valid_name) {
            // Re-parse key as the actual accessor field name
            if (nxt == TokenType::HASH) {
                is_private = true;
                size_t hash_start = current_token().get_start().offset;
                advance();
                if (!match(TokenType::IDENTIFIER)) { add_error("Expected identifier after '#'"); return nullptr; }
                if (current_token().get_start().offset != hash_start + 1) {
                    add_error("SyntaxError: Whitespace not allowed between '#' and identifier"); return nullptr;
                }
                key = std::make_unique<Identifier>("#" + current_token().get_value(), current_token().get_start(), current_token().get_end());
                advance();
            } else if (nxt == TokenType::LEFT_BRACKET) {
                computed = true; advance();
                key = parse_assignment_expression();
                if (!key || !consume(TokenType::RIGHT_BRACKET)) { add_error("Expected ']'"); return nullptr; }
            } else if (nxt == TokenType::IDENTIFIER) {
                key = parse_identifier();
            } else {
                key = std::make_unique<Identifier>(current_token().get_value(), current_token().get_start(), current_token().get_end());
                advance();
            }
        }
    }

    if (current_token().get_type() == TokenType::ASSIGN ||
        current_token().get_type() == TokenType::SEMICOLON ||
        current_token().get_type() == TokenType::RIGHT_BRACE ||
        (!is_generator && !is_async && method_kind == MethodDefinition::METHOD &&
         current_token().get_type() != TokenType::LEFT_PAREN)) {

        std::unique_ptr<ASTNode> init = nullptr;

        if (current_token().get_type() == TokenType::ASSIGN) {
            advance();
            bool saved_cfi = options_.in_class_field_init;
            bool saved_cm_fi = options_.in_class_method;
            options_.in_class_field_init = true;
            options_.in_class_method = true;
            init = parse_assignment_expression();
            options_.in_class_field_init = saved_cfi;
            options_.in_class_method = saved_cm_fi;
            if (!init) {
                add_error("Expected field initializer after '='");
                return nullptr;
            }
        }

        if (current_token().get_type() == TokenType::SEMICOLON) {
            advance();
        } else {
            // ASI: no semicolon only valid if line terminator before next token
            TokenType next = current_token().get_type();
            if (next != TokenType::RIGHT_BRACE && next != TokenType::EOF_TOKEN) {
                // Scan backward for a newline token before the current token
                bool has_newline = false;
                for (int bi = (int)current_token_index_ - 1; bi >= 0; bi--) {
                    TokenType bt = tokens_[bi].get_type();
                    if (bt == TokenType::NEWLINE) { has_newline = true; break; }
                    if (bt != TokenType::WHITESPACE && bt != TokenType::COMMENT) break;
                }
                if (!has_newline) {
                    add_error("SyntaxError: Missing semicolon between class field declarations");
                    return nullptr;
                }
            }
        }

        // Class field restrictions (covers both Identifier and String literal keys)
        if (!computed && key) {
            std::string fname;
            if (key->get_type() == ASTNode::Type::IDENTIFIER)
                fname = static_cast<Identifier*>(key.get())->get_name();
            else if (key->get_type() == ASTNode::Type::STRING_LITERAL)
                fname = static_cast<StringLiteral*>(key.get())->get_value();
            if (!fname.empty()) {
                if (!is_static && fname == "constructor") {
                    add_error("SyntaxError: Class field cannot be named 'constructor'");
                    return nullptr;
                }
                if (is_static && (fname == "prototype" || fname == "constructor")) {
                    add_error("SyntaxError: Static class field cannot be named 'prototype' or 'constructor'");
                    return nullptr;
                }
            }
        }

        return std::make_unique<ClassField>(std::move(key), std::move(init), is_static, computed, start, get_current_position());
    }

    // Method name validation
    if (!computed && key) {
        std::string key_name;
        if (key->get_type() == ASTNode::Type::IDENTIFIER)
            key_name = static_cast<Identifier*>(key.get())->get_name();
        else if (key->get_type() == ASTNode::Type::STRING_LITERAL)
            key_name = static_cast<StringLiteral*>(key.get())->get_value();

        if (!is_static && key_name == "constructor") {
            if (is_generator || is_async ||
                method_kind == MethodDefinition::GETTER || method_kind == MethodDefinition::SETTER) {
                add_error("SyntaxError: Class constructor may not be a generator, async method, getter, or setter");
                return nullptr;
            }
        }
        if (is_static && key_name == "prototype") {
            add_error("SyntaxError: Class may not have a static property named 'prototype'");
            return nullptr;
        }
    }

    MethodDefinition::Kind kind = method_kind;
    if (!computed && key && key->get_type() == ASTNode::Type::IDENTIFIER) {
        auto* key_id = static_cast<Identifier*>(key.get());
        if (key_id->get_name() == "constructor" && !is_static &&
            !is_generator && !is_async &&
            method_kind != MethodDefinition::GETTER && method_kind != MethodDefinition::SETTER) {
            kind = MethodDefinition::CONSTRUCTOR;
        }
    }

    if (current_token().get_type() != TokenType::LEFT_PAREN) {
        add_error("Expected '(' for method parameters");
        return nullptr;
    }
    
    advance();
    
    std::vector<std::unique_ptr<Parameter>> params;
    bool method_has_non_simple_params = false;
    bool saved_arrow_params_m = options_.in_arrow_params;
    bool saved_sb_params = options_.in_class_static_block;
    bool saved_cm_params = options_.in_class_method;
    bool saved_async_params_m = options_.in_async_body;
    options_.in_arrow_params = true;
    options_.in_class_static_block = false;
    options_.in_class_method = true;
    if (is_async) options_.in_async_body = true;
    while (current_token().get_type() != TokenType::RIGHT_PAREN && !at_end()) {
        Position param_start = get_current_position();
        bool is_rest = false;

        if (match(TokenType::ELLIPSIS)) {
            is_rest = true;
            method_has_non_simple_params = true;
            advance();
        }

        std::unique_ptr<Identifier> param_name = nullptr;

        if (current_token().get_type() == TokenType::LEFT_BRACKET ||
            current_token().get_type() == TokenType::LEFT_BRACE) {
            method_has_non_simple_params = true;
            auto destructuring = parse_destructuring_pattern();
            if (!destructuring) {
                add_error("Invalid destructuring pattern in arrow parameters");
                return nullptr;
            }
            static int arrow_destr_counter = 0;
            std::string synthetic_name = "__adestr_" + std::to_string(arrow_destr_counter++);
            Position param_pos = get_current_position();
            param_name = std::make_unique<Identifier>(synthetic_name, param_pos, param_pos);

            std::unique_ptr<ASTNode> default_value = nullptr;
            if (!is_rest && match(TokenType::ASSIGN)) {
                advance();
                default_value = parse_assignment_expression();
                if (!default_value) {
                    add_error("Invalid default parameter value");
                    return nullptr;
                }
            }

            Position param_end = get_current_position();
            auto param = std::make_unique<Parameter>(std::move(param_name), std::move(default_value), is_rest, param_start, param_end);
            param->set_destructuring_pattern(std::move(destructuring));
            params.push_back(std::move(param));

            if (is_rest) {
                if (!match(TokenType::RIGHT_PAREN)) {
                    add_error("Rest parameter must be last formal parameter");
                    return nullptr;
                }
                break;
            }

            if (match(TokenType::COMMA)) {
                advance();
            } else if (!match(TokenType::RIGHT_PAREN)) {
                add_error("Expected ',' or ')' in parameter list");
                return nullptr;
            }
            continue;
        } else if (current_token().get_type() == TokenType::IDENTIFIER) {
            const std::string& apname = current_token().get_value();
            if (options_.strict_mode && (apname == "eval" || apname == "arguments")) {
                add_error("SyntaxError: '" + apname + "' cannot be a parameter name in strict mode");
                return nullptr;
            }
            param_name = std::make_unique<Identifier>(apname,
                                                      current_token().get_start(), current_token().get_end());
            advance();
        } else if ((!is_async && !options_.source_type_module &&
                    current_token().get_type() == TokenType::AWAIT) ||
                   (!is_generator && !options_.strict_mode &&
                    current_token().get_type() == TokenType::YIELD) ||
                   ((current_token().get_type() == TokenType::LET ||
                     current_token().get_type() == TokenType::STATIC) &&
                    !options_.strict_mode)) {
            param_name = std::make_unique<Identifier>(current_token().get_value(),
                                                      current_token().get_start(), current_token().get_end());
            advance();
        } else {
            add_error("Expected parameter name or destructuring pattern");
            return nullptr;
        }

        std::unique_ptr<ASTNode> default_value = nullptr;
        if (!is_rest && match(TokenType::ASSIGN)) {
            method_has_non_simple_params = true;
            advance();
            default_value = parse_assignment_expression();
            if (!default_value) {
                add_error("Invalid default parameter value");
                return nullptr;
            }
        } else if (is_rest && match(TokenType::ASSIGN)) {
            add_error("Rest parameter cannot have default value");
            return nullptr;
        }

        Position param_end = get_current_position();
        auto param = std::make_unique<Parameter>(std::move(param_name), std::move(default_value), is_rest, param_start, param_end);
        params.push_back(std::move(param));
        
        if (is_rest) {
            if (current_token().get_type() != TokenType::RIGHT_PAREN) {
                add_error("Rest parameter must be last formal parameter");
                return nullptr;
            }
            break;
        }
        
        if (current_token().get_type() == TokenType::COMMA) {
            advance();
        } else if (current_token().get_type() != TokenType::RIGHT_PAREN) {
            add_error("Expected ',' or ')' in parameter list");
            return nullptr;
        }
    }
    
    options_.in_arrow_params = saved_arrow_params_m;
    options_.in_class_static_block = saved_sb_params;
    options_.in_class_method = saved_cm_params;
    options_.in_async_body = saved_async_params_m;

    if (!match(TokenType::RIGHT_PAREN)) {
        add_error("Expected ')' after parameters");
        return nullptr;
    }
    advance();

    // Getter: no params. Setter: exactly one simple param.
    if (method_kind == MethodDefinition::GETTER && !params.empty()) {
        add_error("SyntaxError: Getter must have no formal parameters");
        return nullptr;
    }
    if (method_kind == MethodDefinition::SETTER) {
        if (params.size() != 1) {
            add_error("SyntaxError: Setter must have exactly one formal parameter");
            return nullptr;
        }
        if (!params.empty() && params[0]->is_rest()) {
            add_error("SyntaxError: Setter parameter must not be a rest parameter");
            return nullptr;
        }
    }

    if (current_token().get_type() != TokenType::LEFT_BRACE) {
        add_error("Expected '{' for method body");
        return nullptr;
    }

    bool saved_a = options_.in_async_body;
    bool saved_g = options_.in_generator_body;
    bool saved_sb = options_.in_class_static_block;
    if (is_async) options_.in_async_body = true;
    options_.in_generator_body = is_generator;
    options_.in_class_static_block = false;
    bool saved_cm = options_.in_class_method;
    bool saved_ic = options_.in_constructor;
    options_.in_class_method = true;
    options_.in_constructor = (kind == MethodDefinition::CONSTRUCTOR);
    options_.function_depth++;
    options_.non_arrow_function_depth++;
    auto body = parse_block_statement(true);
    options_.function_depth--;
    options_.non_arrow_function_depth--;
    options_.in_class_method = saved_cm;
    options_.in_constructor = saved_ic;
    options_.in_async_body = saved_a;
    options_.in_generator_body = saved_g;
    options_.in_class_static_block = saved_sb;
    if (!body) {
        add_error("Expected method body");
        return nullptr;
    }

    // Class methods always strict; duplicate params forbidden when non-simple
    if (method_has_non_simple_params || options_.strict_mode || is_async || is_generator) {
        for (size_t pi = 0; pi < params.size(); pi++) {
            if (!params[pi]->get_name()) continue;
            const std::string& pn = params[pi]->get_name()->get_name();
            if (pn.empty() || pn[0] == '_') continue;
            for (size_t pj = pi + 1; pj < params.size(); pj++) {
                if (!params[pj]->get_name()) continue;
                if (params[pj]->get_name()->get_name() == pn) {
                    add_error("SyntaxError: Duplicate parameter name not allowed");
                    return nullptr;
                }
            }
        }
    }

    if (method_has_non_simple_params && body) {
        auto* block = static_cast<BlockStatement*>(body.get());
        if (block && !block->get_statements().empty()) {
            auto* first = block->get_statements()[0].get();
            if (auto* es = dynamic_cast<ExpressionStatement*>(first)) {
                if (auto* sl = dynamic_cast<StringLiteral*>(es->get_expression())) {
                    if (sl->get_value() == "use strict") {
                        add_error("SyntaxError: Illegal 'use strict' directive in function with non-simple parameter list");
                        return nullptr;
                    }
                }
            }
        }
    }

    // Methods are always strict: params must not shadow lexical declarations in body.
    if (body) {
        auto* block = static_cast<BlockStatement*>(body.get());
        std::string dup = check_params_body_lex_conflict(params, block);
        if (!dup.empty()) {
            add_error("SyntaxError: Identifier '" + dup + "' is already declared (parameter name shadows lexical declaration)");
            return nullptr;
        }
    }

    std::string method_src = get_source_slice(src_start.offset, last_meaningful_token().get_start().offset + 1);

    auto function_expr = std::make_unique<FunctionExpression>(
        nullptr,
        std::move(params),
        std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body.release())),
        start, get_current_position(), is_generator, is_async
    );
    function_expr->set_source_text(method_src);

    Position end = get_current_position();
    auto method = std::make_unique<MethodDefinition>(
        std::move(key),
        std::move(function_expr),
        kind, is_static, computed, start, end
    );
    method->set_source_text(method_src);
    return method;
}

std::unique_ptr<ASTNode> Parser::parse_function_expression() {
    Position start = get_current_position();

    if (!consume(TokenType::FUNCTION)) {
        add_error("Expected 'function'");
        return nullptr;
    }

    bool is_generator = false;
    if (match(TokenType::MULTIPLY)) {
        is_generator = true;
        advance();
    }

    std::unique_ptr<Identifier> id = nullptr;
    if (current_token().get_type() == TokenType::IDENTIFIER) {
        const std::string& fe_name = current_token().get_value();
        if (options_.strict_mode && (fe_name == "eval" || fe_name == "arguments")) {
            add_error("SyntaxError: '" + fe_name + "' cannot be used as function name in strict mode");
            return nullptr;
        }
        id = std::make_unique<Identifier>(fe_name,
                                        current_token().get_start(), current_token().get_end());
        advance();
    } else if (current_token().get_type() == TokenType::YIELD && !options_.strict_mode && !is_generator) {
        // yield is a valid function expression name in sloppy non-generator context
        id = std::make_unique<Identifier>("yield", current_token().get_start(), current_token().get_end());
        advance();
    } else if (current_token().get_type() == TokenType::AWAIT && !options_.in_async_body && !options_.source_type_module) {
        // await is a valid function expression name outside async context
        id = std::make_unique<Identifier>("await", current_token().get_start(), current_token().get_end());
        advance();
    }

    if (!consume(TokenType::LEFT_PAREN)) {
        add_error("Expected '(' after 'function'");
        return nullptr;
    }

    std::vector<std::unique_ptr<Parameter>> params;
    bool has_non_simple_params = false;

    // FormalParameters[+Yield] for generator expressions: parse bare `yield` in
    // parameter default values as YieldExpression so the Contains-YieldExpression
    // early error below can detect it (spec 15.5.1).
    bool saved_gen_for_params_fe = options_.in_generator_body;
    bool saved_csb_params_fe = options_.in_class_static_block;
    options_.in_generator_body = is_generator;
    options_.in_class_static_block = false;

    while (!match(TokenType::RIGHT_PAREN) && !at_end()) {
        Position param_start = get_current_position();
        bool is_rest = false;

        if (match(TokenType::ELLIPSIS)) {
            is_rest = true;
            has_non_simple_params = true;
            advance();
        }

        std::unique_ptr<Identifier> param_name = nullptr;

        if (current_token().get_type() == TokenType::LEFT_BRACKET ||
            current_token().get_type() == TokenType::LEFT_BRACE) {
            has_non_simple_params = true;
            auto destructuring = parse_destructuring_pattern();
            if (!destructuring) {
                add_error("Invalid destructuring pattern in function parameters");
                options_.in_generator_body = saved_gen_for_params_fe;
                return nullptr;
            }
            static int destr_param_counter = 0;
            std::string synthetic_name = "__destr_" + std::to_string(destr_param_counter++);
            Position param_pos = get_current_position();
            param_name = std::make_unique<Identifier>(synthetic_name, param_pos, param_pos);

            std::unique_ptr<ASTNode> default_value = nullptr;
            if (!is_rest && match(TokenType::ASSIGN)) {
                advance();
                default_value = parse_assignment_expression();
                if (!default_value) {
                    add_error("Invalid default parameter value");
                    options_.in_generator_body = saved_gen_for_params_fe;
                    return nullptr;
                }
            }

            Position param_end = get_current_position();
            auto param = std::make_unique<Parameter>(std::move(param_name), std::move(default_value), is_rest, param_start, param_end);
            param->set_destructuring_pattern(std::move(destructuring));
            params.push_back(std::move(param));

            if (is_rest) {
                if (!match(TokenType::RIGHT_PAREN)) {
                    add_error("Rest parameter must be last formal parameter");
                    options_.in_generator_body = saved_gen_for_params_fe;
                    return nullptr;
                }
                break;
            }

            if (match(TokenType::COMMA)) {
                advance();
            } else if (!match(TokenType::RIGHT_PAREN)) {
                add_error("Expected ',' or ')' in parameter list");
                options_.in_generator_body = saved_gen_for_params_fe;
                return nullptr;
            }
            continue;
        } else if (current_token().get_type() == TokenType::IDENTIFIER) {
            // ES5: eval and arguments cannot be used as parameter names in strict mode
            if (options_.strict_mode) {
                const std::string& pname = current_token().get_value();
                if (pname == "eval" || pname == "arguments") {
                    add_error("'" + pname + "' cannot be used as a parameter name in strict mode");
                    options_.in_generator_body = saved_gen_for_params_fe;
                    return nullptr;
                }
            }
            param_name = std::make_unique<Identifier>(current_token().get_value(),
                                                      current_token().get_start(), current_token().get_end());
            advance();
        } else if ((current_token().get_type() == TokenType::AWAIT &&
                    !options_.source_type_module) ||
                   (current_token().get_type() == TokenType::YIELD &&
                    !is_generator && !options_.strict_mode) ||
                   ((current_token().get_type() == TokenType::LET ||
                     current_token().get_type() == TokenType::STATIC ||
                     current_token().get_type() == TokenType::UNDEFINED) &&
                    !options_.strict_mode)) {
            param_name = std::make_unique<Identifier>(current_token().get_value(),
                                                      current_token().get_start(), current_token().get_end());
            advance();
        } else {
            add_error("Expected parameter name or destructuring pattern");
            options_.in_generator_body = saved_gen_for_params_fe;
            return nullptr;
        }

        std::unique_ptr<ASTNode> default_value = nullptr;
        if (!is_rest && match(TokenType::ASSIGN)) {
            has_non_simple_params = true;
            advance();
            default_value = parse_assignment_expression();
            if (!default_value) {
                add_error("Invalid default parameter value");
                options_.in_generator_body = saved_gen_for_params_fe;
                return nullptr;
            }
        } else if (is_rest && match(TokenType::ASSIGN)) {
            add_error("Rest parameter cannot have default value");
            options_.in_generator_body = saved_gen_for_params_fe;
            return nullptr;
        }

        Position param_end = get_current_position();
        auto param = std::make_unique<Parameter>(std::move(param_name), std::move(default_value), is_rest, param_start, param_end);
        params.push_back(std::move(param));

        if (is_rest) {
            if (!match(TokenType::RIGHT_PAREN)) {
                add_error("Rest parameter must be last formal parameter");
                options_.in_generator_body = saved_gen_for_params_fe;
                return nullptr;
            }
            break;
        }

        if (match(TokenType::COMMA)) {
            advance();
        } else if (!match(TokenType::RIGHT_PAREN)) {
            add_error("Expected ',' or ')' in parameter list");
            options_.in_generator_body = saved_gen_for_params_fe;
            return nullptr;
        }
    }

    options_.in_generator_body = saved_gen_for_params_fe;
    options_.in_class_static_block = saved_csb_params_fe;

    if (!consume(TokenType::RIGHT_PAREN)) {
        add_error("Expected ')' after parameters");
        return nullptr;
    }

    if (is_generator) {
        std::string forbidden = find_forbidden_expr_in_params(params, true, false);
        if (!forbidden.empty()) {
            add_error("SyntaxError: " + forbidden + " expression not allowed in formal parameters of generator function");
            return nullptr;
        }
    }

    if (!is_generator && options_.source_type_module) {
        std::string forbidden = find_forbidden_expr_in_params(params, false, true);
        if (!forbidden.empty()) {
            add_error("SyntaxError: await expression not allowed in formal parameters of function in module context");
            return nullptr;
        }
    }

    int saved_loop_fn = options_.loop_depth;
    int saved_sw_fn = options_.switch_depth;
    auto saved_al_fn = options_.active_labels;
    auto saved_ll_fn = options_.loop_labels;
    options_.loop_depth = 0;
    options_.switch_depth = 0;
    options_.active_labels.clear();
    options_.loop_labels.clear();
    bool saved_gen_ctx = options_.in_generator_body;
    bool saved_async_fd = options_.in_async_body;
    bool saved_csb_fd = options_.in_class_static_block;
    bool saved_cfi2 = options_.in_class_field_init;
    bool saved_cm2 = options_.in_class_method;
    bool saved_ic_fd = options_.in_constructor;
    options_.in_generator_body = is_generator;
    options_.in_async_body = false;
    options_.in_class_static_block = false;
    options_.in_class_field_init = false;
    options_.in_class_method = false;
    options_.in_constructor = false;
    options_.function_depth++;
    options_.non_arrow_function_depth++;
    auto body = parse_block_statement(true);
    options_.function_depth--;
    options_.non_arrow_function_depth--;
    options_.in_generator_body = saved_gen_ctx;
    options_.in_async_body = saved_async_fd;
    options_.in_class_static_block = saved_csb_fd;
    options_.in_class_field_init = saved_cfi2;
    options_.in_class_method = saved_cm2;
    options_.in_constructor = saved_ic_fd;
    options_.loop_depth = saved_loop_fn;
    options_.switch_depth = saved_sw_fn;
    options_.active_labels = std::move(saved_al_fn);
    options_.loop_labels = std::move(saved_ll_fn);
    if (!body) {
        add_error("Expected function body");
        return nullptr;
    }

    // Duplicate param names: SyntaxError in strict mode OR when params are non-simple
    if (options_.strict_mode || has_non_simple_params) {
        for (size_t pi = 0; pi < params.size(); pi++) {
            if (!params[pi]->get_name()) continue;
            const std::string& pn = params[pi]->get_name()->get_name();
            if (pn.empty() || pn[0] == '_') continue;
            for (size_t pj = pi + 1; pj < params.size(); pj++) {
                if (!params[pj]->get_name()) continue;
                if (params[pj]->get_name()->get_name() == pn) {
                    add_error("SyntaxError: Duplicate parameter name not allowed");
                    return nullptr;
                }
            }
        }
    }


    // ES6: Duplicate binding identifiers inside destructuring params always SyntaxError
    {
        std::unordered_set<std::string> seen_bindings;
        for (const auto& param : params) {
            if (param->has_destructuring()) {
                ASTNode* pattern = param->get_destructuring_pattern();
                if (pattern && pattern->get_type() == ASTNode::Type::DESTRUCTURING_ASSIGNMENT) {
                    DestructuringAssignment* da = static_cast<DestructuringAssignment*>(pattern);
                    if (da->get_type() == DestructuringAssignment::Type::ARRAY) {
                        for (const auto& target : da->get_targets()) {
                            if (!target) continue;
                            const std::string& name = target->get_name();
                            if (name.empty() || name[0] == '_') continue;
                            if (!seen_bindings.insert(name).second) {
                                add_error("Duplicate binding identifier '" + name + "' in destructuring parameter");
                                return nullptr;
                            }
                        }
                    } else {
                        for (const auto& pm : da->get_property_mappings()) {
                            if (pm.variable_name.empty() || pm.variable_name[0] == '_') continue;
                            if (!seen_bindings.insert(pm.variable_name).second) {
                                add_error("Duplicate binding identifier '" + pm.variable_name + "' in destructuring parameter");
                                return nullptr;
                            }
                        }
                    }
                }
            }
        }
    }
    if (body) {
        BlockStatement* block = static_cast<BlockStatement*>(body.get());
        if (block && !block->get_statements().empty()) {
            auto* first_stmt = block->get_statements()[0].get();
            if (auto* expr_stmt = dynamic_cast<ExpressionStatement*>(first_stmt)) {
                if (auto* literal = dynamic_cast<StringLiteral*>(expr_stmt->get_expression())) {
                    if (literal->get_value() == "use strict") {
                        if (has_non_simple_params) {
                            add_error("SyntaxError: Illegal 'use strict' directive in function with non-simple parameter list");
                            return nullptr;
                        }
                        if (id) {
                            const std::string& fname = static_cast<Identifier*>(id.get())->get_name();
                            if (fname == "eval" || fname == "arguments") {
                                add_error("SyntaxError: '" + fname + "' cannot be used as function name in strict mode");
                                return nullptr;
                            }
                        }
                        for (const auto& p : params) {
                            if (!p->get_name()) continue;
                            const std::string& pn = p->get_name()->get_name();
                            if (pn == "eval" || pn == "arguments") {
                                add_error("SyntaxError: '" + pn + "' cannot be a parameter name in strict mode");
                                return nullptr;
                            }
                        }
                        for (size_t pi = 0; pi < params.size(); pi++) {
                            if (!params[pi]->get_name()) continue;
                            const std::string& pn = params[pi]->get_name()->get_name();
                            if (pn.empty() || pn[0] == '_') continue;
                            for (size_t pj = pi + 1; pj < params.size(); pj++) {
                                if (!params[pj]->get_name()) continue;
                                if (params[pj]->get_name()->get_name() == pn) {
                                    add_error("SyntaxError: Duplicate parameter name not allowed in strict mode");
                                    return nullptr;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    if (options_.strict_mode && body) {
        auto* block = static_cast<BlockStatement*>(body.get());
        std::string dup = check_params_body_lex_conflict(params, block);
        if (!dup.empty()) {
            add_error("SyntaxError: Identifier '" + dup + "' is already declared (parameter name shadows lexical declaration in strict mode)");
            return nullptr;
        }
    }

    Position end = get_current_position();
    auto fn_expr = std::make_unique<FunctionExpression>(
        std::move(id), std::move(params),
        std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body.release())),
        start, end, is_generator
    );
    fn_expr->set_source_text(get_source_slice(start.offset, last_meaningful_token().get_start().offset + 1));
    return fn_expr;
}

std::unique_ptr<ASTNode> Parser::parse_async_function_expression() {
    Position start = get_current_position();

    // Save async token end-line and end pos BEFORE consuming it (advance() skips newlines)
    size_t async_end_line = current_token().get_end().line;
    Position async_end = current_token().get_end();

    if (!consume(TokenType::ASYNC)) {
        add_error("Expected 'async'");
        return nullptr;
    }

    // No line terminator allowed between 'async' and 'function' (ES2017)
    // If there IS a line terminator, 'async' is just an identifier expression, not async function
    if (match(TokenType::FUNCTION) && current_token().get_start().line != async_end_line) {
        return std::make_unique<Identifier>("async", start, async_end);
    }

    if (match(TokenType::FUNCTION)) {
        advance();
    } else if (match(TokenType::LEFT_PAREN)) {
        // No line terminator allowed between 'async' and '(' for async arrow
        if (current_token().get_start().line != async_end_line) {
            add_error("SyntaxError: Unexpected token: line break between 'async' and arrow parameters");
            return nullptr;
        }
        return parse_async_arrow_function(start);
    } else if (current_token().get_start().line == async_end_line &&
               (match(TokenType::IDENTIFIER) ||
                // Contextual keywords (of, from, static, ...) used as async arrow single param
                (is_keyword_token(current_token().get_type()) &&
                 peek_token().get_type() == TokenType::ARROW))) {
        return parse_async_arrow_function_single_param(start);
    } else {
        // Not an async function/arrow — `async` is being used as an identifier
        return std::make_unique<Identifier>("async", start, async_end);
    }

    bool is_generator = false;
    if (current_token().get_type() == TokenType::MULTIPLY) {
        advance();
        is_generator = true;
    }

    std::unique_ptr<Identifier> id = nullptr;
    if (current_token().get_type() == TokenType::IDENTIFIER) {
        const std::string& aname = current_token().get_value();
        if (options_.strict_mode && (aname == "eval" || aname == "arguments")) {
            add_error("SyntaxError: '" + aname + "' cannot be used as function name in strict mode");
            return nullptr;
        }
        id = std::make_unique<Identifier>(aname, current_token().get_start(), current_token().get_end());
        advance();
    }

    if (!consume(TokenType::LEFT_PAREN)) {
        add_error("Expected '(' after async function name");
        return nullptr;
    }

    std::vector<std::unique_ptr<Parameter>> params;
    bool has_non_simple_params = false;

    // FormalParameters[+Yield, +Await] (async generator) / [~Yield, +Await] (async
    // function): parse bare `yield` in parameter default values as YieldExpression
    // when this is an async generator, so the Contains-YieldExpression early error
    // below can detect it. `await` must also be treated as AwaitExpression in async
    // function parameters (spec: ContainsAwait early error for FormalParameters).
    bool saved_gen_for_params_afe = options_.in_generator_body;
    bool saved_async_for_params_afe = options_.in_async_body;
    options_.in_generator_body = is_generator;
    options_.in_async_body = true;

    while (!match(TokenType::RIGHT_PAREN) && !at_end()) {
        Position param_start = current_token().get_start();
        bool is_rest = false;

        if (match(TokenType::ELLIPSIS)) {
            is_rest = true;
            has_non_simple_params = true;
            advance();
        }

        std::unique_ptr<Identifier> param_name = nullptr;

        if (current_token().get_type() == TokenType::LEFT_BRACKET ||
            current_token().get_type() == TokenType::LEFT_BRACE) {
            has_non_simple_params = true;
            auto destructuring = parse_destructuring_pattern();
            if (!destructuring) {
                add_error("Invalid destructuring pattern in function parameters");
                options_.in_generator_body = saved_gen_for_params_afe;
                return nullptr;
            }
            static int destr_param_counter = 0;
            std::string synthetic_name = "__destr_" + std::to_string(destr_param_counter++);
            Position param_pos = get_current_position();
            param_name = std::make_unique<Identifier>(synthetic_name, param_pos, param_pos);

            std::unique_ptr<ASTNode> default_value = nullptr;
            if (!is_rest && match(TokenType::ASSIGN)) {
                advance();
                default_value = parse_assignment_expression();
                if (!default_value) {
                    add_error("Invalid default parameter value");
                    options_.in_generator_body = saved_gen_for_params_afe;
                    return nullptr;
                }
            }

            Position param_end = get_current_position();
            auto param = std::make_unique<Parameter>(std::move(param_name), std::move(default_value), is_rest, param_start, param_end);
            param->set_destructuring_pattern(std::move(destructuring));
            params.push_back(std::move(param));

            if (is_rest) {
                if (!match(TokenType::RIGHT_PAREN)) {
                    add_error("Rest parameter must be last formal parameter");
                    options_.in_generator_body = saved_gen_for_params_afe;
                    return nullptr;
                }
                break;
            }

            if (match(TokenType::COMMA)) {
                advance();
            } else if (!match(TokenType::RIGHT_PAREN)) {
                add_error("Expected ',' or ')' in parameter list");
                options_.in_generator_body = saved_gen_for_params_afe;
                return nullptr;
            }
            continue;
        } else if (current_token().get_type() == TokenType::IDENTIFIER) {
            {
                const std::string& pn2 = current_token().get_value();
                if (options_.strict_mode && (pn2 == "eval" || pn2 == "arguments")) {
                    add_error("SyntaxError: '" + pn2 + "' cannot be a parameter name in strict mode");
                    options_.in_generator_body = saved_gen_for_params_afe;
                    return nullptr;
                }
                param_name = std::make_unique<Identifier>(pn2,
                                                          current_token().get_start(), current_token().get_end());
            }
            advance();
        } else if ((!is_generator && !options_.strict_mode &&
                    current_token().get_type() == TokenType::YIELD) ||
                   ((current_token().get_type() == TokenType::LET ||
                     current_token().get_type() == TokenType::STATIC) &&
                    !options_.strict_mode)) {
            param_name = std::make_unique<Identifier>(current_token().get_value(),
                                                      current_token().get_start(), current_token().get_end());
            advance();
        } else {
            add_error("Expected parameter name or destructuring pattern");
            options_.in_generator_body = saved_gen_for_params_afe;
            return nullptr;
        }

        std::unique_ptr<ASTNode> default_value = nullptr;
        if (!is_rest && match(TokenType::ASSIGN)) {
            has_non_simple_params = true;
            advance();
            default_value = parse_assignment_expression();
            if (!default_value) {
                add_error("Invalid default parameter value");
                options_.in_generator_body = saved_gen_for_params_afe;
                return nullptr;
            }
        } else if (is_rest && match(TokenType::ASSIGN)) {
            add_error("Rest parameter cannot have default value");
            options_.in_generator_body = saved_gen_for_params_afe;
            return nullptr;
        }

        Position param_end = get_current_position();
        auto param = std::make_unique<Parameter>(std::move(param_name), std::move(default_value), is_rest, param_start, param_end);
        params.push_back(std::move(param));

        if (is_rest) {
            if (!match(TokenType::RIGHT_PAREN)) {
                add_error("Rest parameter must be last formal parameter");
                options_.in_generator_body = saved_gen_for_params_afe;
                return nullptr;
            }
            break;
        }

        if (match(TokenType::COMMA)) {
            advance();
        } else if (!match(TokenType::RIGHT_PAREN)) {
            add_error("Expected ',' or ')' in parameter list");
            options_.in_generator_body = saved_gen_for_params_afe;
            return nullptr;
        }
    }

    options_.in_generator_body = saved_gen_for_params_afe;
    options_.in_async_body = saved_async_for_params_afe;

    if (!consume(TokenType::RIGHT_PAREN)) {
        add_error("Expected ')' after parameters");
        return nullptr;
    }

    {
        std::string forbidden = find_forbidden_expr_in_params(params, is_generator, true);
        if (!forbidden.empty()) {
            add_error("SyntaxError: " + forbidden + " expression not allowed in formal parameters of async function");
            return nullptr;
        }
    }

    // Async functions always use UniqueFormaParameters (or at least strict)
    if (options_.strict_mode || has_non_simple_params || is_generator) {
        for (size_t pi = 0; pi < params.size(); pi++) {
            if (!params[pi]->get_name()) continue;
            const std::string& pn = params[pi]->get_name()->get_name();
            if (pn.empty() || pn[0] == '_') continue;
            for (size_t pj = pi + 1; pj < params.size(); pj++) {
                if (!params[pj]->get_name()) continue;
                if (params[pj]->get_name()->get_name() == pn) {
                    add_error("SyntaxError: Duplicate parameter name not allowed");
                    return nullptr;
                }
            }
        }
    }

    bool saved_async = options_.in_async_body;
    bool saved_gen = options_.in_generator_body;
    bool saved_cfi_ae = options_.in_class_field_init;
    bool saved_sb_ae = options_.in_class_static_block;
    options_.in_async_body = true;
    options_.in_generator_body = is_generator;
    options_.in_class_field_init = false;
    options_.in_class_static_block = false;
    bool saved_cm_ae = options_.in_class_method;
    options_.in_class_method = false;
    options_.in_constructor = false;
    options_.function_depth++;
    options_.non_arrow_function_depth++;
    auto body = parse_block_statement(true);
    options_.function_depth--;
    options_.non_arrow_function_depth--;
    options_.in_async_body = saved_async;
    options_.in_generator_body = saved_gen;
    options_.in_class_field_init = saved_cfi_ae;
    options_.in_class_static_block = saved_sb_ae;
    options_.in_class_method = saved_cm_ae;
    if (!body) {
        add_error("Expected async function body");
        return nullptr;
    }

    if (has_non_simple_params && body) {
        auto* blk = static_cast<BlockStatement*>(body.get());
        if (blk && !blk->get_statements().empty()) {
            auto* fs = blk->get_statements()[0].get();
            if (auto* es = dynamic_cast<ExpressionStatement*>(fs)) {
                if (auto* sl = dynamic_cast<StringLiteral*>(es->get_expression())) {
                    if (sl->get_value() == "use strict") {
                        add_error("SyntaxError: Illegal 'use strict' directive in function with non-simple parameter list");
                        return nullptr;
                    }
                }
            }
        }
    }

    if (body) {
        auto* block = static_cast<BlockStatement*>(body.get());
        std::string dup = check_params_body_lex_conflict(params, block);
        if (!dup.empty()) {
            add_error("SyntaxError: Identifier '" + dup + "' is already declared (parameter name shadows lexical declaration)");
            return nullptr;
        }
    }

    Position end = get_current_position();
    std::string src_text = get_source_slice(start.offset, last_meaningful_token().get_start().offset + 1);
    if (is_generator) {
        auto gen_expr = std::make_unique<FunctionExpression>(
            std::move(id), std::move(params),
            std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body.release())),
            start, end, true, true
        );
        gen_expr->set_source_text(src_text);
        return gen_expr;
    }
    auto async_expr = std::make_unique<AsyncFunctionExpression>(
        std::move(id), std::move(params),
        std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body.release())),
        start, end
    );
    async_expr->set_source_text(src_text);
    return async_expr;
}

std::unique_ptr<ASTNode> Parser::parse_async_function_declaration() {
    Position start = get_current_position();

    // Save async token end-line BEFORE consuming it
    size_t async_end_line = current_token().get_end().line;

    if (!consume(TokenType::ASYNC)) {
        add_error("Expected 'async'");
        return nullptr;
    }

    // ES2017: No line terminator allowed between 'async' and 'function'
    if (match(TokenType::FUNCTION) && current_token().get_start().line != async_end_line) {
        add_error("Unexpected token: line break between 'async' and 'function' is not allowed");
        return nullptr;
    }

    if (!consume(TokenType::FUNCTION)) {
        add_error("Expected 'function' after 'async'");
        return nullptr;
    }

    bool is_generator = false;
    if (current_token().get_type() == TokenType::MULTIPLY) {
        advance();
        is_generator = true;
    }

    {
        TokenType ct = current_token().get_type();
        bool name_ok = ct == TokenType::IDENTIFIER;
        if (!name_ok && ct == TokenType::YIELD && !options_.in_generator_body && !options_.strict_mode && !is_generator)
            name_ok = true;
        if (!name_ok && ct == TokenType::AWAIT && !options_.in_async_body && !options_.source_type_module && !options_.in_class_static_block)
            name_ok = true;
        if (!name_ok && ct == TokenType::LET && !options_.strict_mode)
            name_ok = true;
        if (!name_ok) {
            add_error("Expected function name after 'async function'");
            return nullptr;
        }
    }

    std::string af_name = current_token().get_value();
    auto id = std::make_unique<Identifier>(af_name,
                                        current_token().get_start(), current_token().get_end());
    advance();

    if (options_.strict_mode && (af_name == "eval" || af_name == "arguments")) {
        add_error("SyntaxError: '" + af_name + "' cannot be used as function name in strict mode");
        return nullptr;
    }

    if (!consume(TokenType::LEFT_PAREN)) {
        add_error("Expected '(' after async function name");
        return nullptr;
    }

    std::vector<std::unique_ptr<Parameter>> params;
    bool has_non_simple_params = false;

    // FormalParameters[+Yield, +Await] (async generator) / [~Yield, +Await] (async
    // function): parse bare `yield` in parameter default values as YieldExpression
    // when this is an async generator, so the Contains-YieldExpression early error
    // below can detect it. `await` must also be treated as AwaitExpression in async
    // function parameters (spec: ContainsAwait early error for FormalParameters).
    bool saved_gen_for_params_afd = options_.in_generator_body;
    bool saved_async_for_params_afd = options_.in_async_body;
    options_.in_generator_body = is_generator;
    options_.in_async_body = true;

    while (!match(TokenType::RIGHT_PAREN) && !at_end()) {
        Position param_start = current_token().get_start();
        bool is_rest = false;

        if (match(TokenType::ELLIPSIS)) {
            is_rest = true;
            has_non_simple_params = true;
            advance();
        }

        std::unique_ptr<Identifier> param_name = nullptr;

        if (current_token().get_type() == TokenType::LEFT_BRACKET ||
            current_token().get_type() == TokenType::LEFT_BRACE) {
            has_non_simple_params = true;
            auto destructuring = parse_destructuring_pattern();
            if (!destructuring) {
                add_error("Invalid destructuring pattern in function parameters");
                options_.in_generator_body = saved_gen_for_params_afd;
                return nullptr;
            }
            static int destr_param_counter = 0;
            std::string synthetic_name = "__destr_" + std::to_string(destr_param_counter++);
            Position param_pos = get_current_position();
            param_name = std::make_unique<Identifier>(synthetic_name, param_pos, param_pos);

            std::unique_ptr<ASTNode> default_value = nullptr;
            if (!is_rest && match(TokenType::ASSIGN)) {
                advance();
                default_value = parse_assignment_expression();
                if (!default_value) {
                    add_error("Invalid default parameter value");
                    options_.in_generator_body = saved_gen_for_params_afd;
                    return nullptr;
                }
            }

            Position param_end = get_current_position();
            auto param = std::make_unique<Parameter>(std::move(param_name), std::move(default_value), is_rest, param_start, param_end);
            param->set_destructuring_pattern(std::move(destructuring));
            params.push_back(std::move(param));

            if (is_rest) {
                if (!match(TokenType::RIGHT_PAREN)) {
                    add_error("Rest parameter must be last formal parameter");
                    options_.in_generator_body = saved_gen_for_params_afd;
                    return nullptr;
                }
                break;
            }

            if (match(TokenType::COMMA)) {
                advance();
            } else if (!match(TokenType::RIGHT_PAREN)) {
                add_error("Expected ',' or ')' in parameter list");
                options_.in_generator_body = saved_gen_for_params_afd;
                return nullptr;
            }
            continue;
        } else if (current_token().get_type() == TokenType::IDENTIFIER) {
            {
                const std::string& pn2 = current_token().get_value();
                if (options_.strict_mode && (pn2 == "eval" || pn2 == "arguments")) {
                    add_error("SyntaxError: '" + pn2 + "' cannot be a parameter name in strict mode");
                    options_.in_generator_body = saved_gen_for_params_afd;
                    return nullptr;
                }
                param_name = std::make_unique<Identifier>(pn2,
                                                          current_token().get_start(), current_token().get_end());
            }
            advance();
        } else if ((!is_generator && !options_.strict_mode &&
                    current_token().get_type() == TokenType::YIELD) ||
                   ((current_token().get_type() == TokenType::LET ||
                     current_token().get_type() == TokenType::STATIC) &&
                    !options_.strict_mode)) {
            param_name = std::make_unique<Identifier>(current_token().get_value(),
                                                      current_token().get_start(), current_token().get_end());
            advance();
        } else {
            add_error("Expected parameter name or destructuring pattern");
            options_.in_generator_body = saved_gen_for_params_afd;
            return nullptr;
        }

        std::unique_ptr<ASTNode> default_value = nullptr;
        if (!is_rest && match(TokenType::ASSIGN)) {
            has_non_simple_params = true;
            advance();
            default_value = parse_assignment_expression();
            if (!default_value) {
                add_error("Invalid default parameter value");
                options_.in_generator_body = saved_gen_for_params_afd;
                return nullptr;
            }
        } else if (is_rest && match(TokenType::ASSIGN)) {
            add_error("Rest parameter cannot have default value");
            options_.in_generator_body = saved_gen_for_params_afd;
            return nullptr;
        }

        Position param_end = get_current_position();
        auto param = std::make_unique<Parameter>(std::move(param_name), std::move(default_value), is_rest, param_start, param_end);
        params.push_back(std::move(param));

        if (is_rest) {
            if (!match(TokenType::RIGHT_PAREN)) {
                add_error("Rest parameter must be last formal parameter");
                options_.in_generator_body = saved_gen_for_params_afd;
                return nullptr;
            }
            break;
        }

        if (match(TokenType::COMMA)) {
            advance();
        } else if (!match(TokenType::RIGHT_PAREN)) {
            add_error("Expected ',' or ')' in parameter list");
            options_.in_generator_body = saved_gen_for_params_afd;
            return nullptr;
        }
    }

    options_.in_generator_body = saved_gen_for_params_afd;
    options_.in_async_body = saved_async_for_params_afd;

    if (!consume(TokenType::RIGHT_PAREN)) {
        add_error("Expected ')' after parameters");
        return nullptr;
    }

    {
        std::string forbidden = find_forbidden_expr_in_params(params, is_generator, true);
        if (!forbidden.empty()) {
            add_error("SyntaxError: " + forbidden + " expression not allowed in formal parameters of async function");
            return nullptr;
        }
    }

    // Duplicate params always forbidden in async functions (and strict mode)
    {
        std::unordered_set<std::string> seen_params;
        for (const auto& p : params) {
            if (p->get_name() && !p->get_name()->get_name().empty()) {
                const std::string& pn = p->get_name()->get_name();
                if (!seen_params.insert(pn).second) {
                    add_error("SyntaxError: Duplicate parameter name '" + pn + "' not allowed");
                    return nullptr;
                }
                if (options_.strict_mode && (pn == "eval" || pn == "arguments")) {
                    add_error("SyntaxError: '" + pn + "' cannot be parameter name in strict mode");
                    return nullptr;
                }
            }
        }
    }

    bool saved_async2 = options_.in_async_body;
    bool saved_gen2 = options_.in_generator_body;
    bool saved_cfi_ad = options_.in_class_field_init;
    options_.in_async_body = true;
    options_.in_generator_body = is_generator;
    options_.in_class_field_init = false;
    bool saved_cm_ad = options_.in_class_method;
    options_.in_class_method = false;
    options_.in_constructor = false;
    options_.function_depth++;
    options_.non_arrow_function_depth++;
    auto body = parse_block_statement(true);
    options_.function_depth--;
    options_.non_arrow_function_depth--;
    options_.in_async_body = saved_async2;
    options_.in_generator_body = saved_gen2;
    options_.in_class_field_init = saved_cfi_ad;
    options_.in_class_method = saved_cm_ad;
    if (!body) {
        add_error("Expected async function body");
        return nullptr;
    }

    if (has_non_simple_params && body) {
        auto* blk = static_cast<BlockStatement*>(body.get());
        if (blk && !blk->get_statements().empty()) {
            auto* fs = blk->get_statements()[0].get();
            if (auto* es = dynamic_cast<ExpressionStatement*>(fs)) {
                if (auto* sl = dynamic_cast<StringLiteral*>(es->get_expression())) {
                    if (sl->get_value() == "use strict") {
                        add_error("SyntaxError: Illegal 'use strict' directive in function with non-simple parameter list");
                        return nullptr;
                    }
                }
            }
        }
    }

    if (body) {
        auto* block = static_cast<BlockStatement*>(body.get());
        std::string dup = check_params_body_lex_conflict(params, block);
        if (!dup.empty()) {
            add_error("SyntaxError: Identifier '" + dup + "' is already declared (parameter name shadows lexical declaration)");
            return nullptr;
        }
    }

    Position end = get_current_position();
    auto async_fn_decl = std::make_unique<FunctionDeclaration>(
        std::move(id), std::move(params),
        std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body.release())),
        start, end, true, is_generator
    );
    async_fn_decl->set_source_text(get_source_slice(start.offset, last_meaningful_token().get_start().offset + 1));
    return async_fn_decl;
}

std::unique_ptr<ASTNode> Parser::parse_arrow_function() {
    Position start = get_current_position();
    std::vector<std::unique_ptr<Parameter>> params;
    bool has_non_simple_params = false;
    
    if (match(TokenType::IDENTIFIER) ||
        // Non-strict, non-generator: yield/await usable as plain identifier param
        (match(TokenType::YIELD) && !options_.in_generator_body && !options_.strict_mode)) {
        Position param_start = get_current_position();
        std::string pname = (current_token().get_type() == TokenType::YIELD) ? "yield" : current_token().get_value();
        if (options_.strict_mode && (pname == "eval" || pname == "arguments")) {
            add_error("SyntaxError: '" + pname + "' cannot be a parameter name in strict mode");
            return nullptr;
        }
        auto param_name = std::make_unique<Identifier>(pname,
                                                      current_token().get_start(), current_token().get_end());
        advance();

        Position param_end = get_current_position();
        auto param = std::make_unique<Parameter>(std::move(param_name), nullptr, false, param_start, param_end);
        params.push_back(std::move(param));
    } else if (match(TokenType::LEFT_PAREN)) {
        advance();

        bool saved_arrow_params = options_.in_arrow_params;
        options_.in_arrow_params = true;

        while (!match(TokenType::RIGHT_PAREN) && !at_end()) {
            if (current_token().get_type() == TokenType::LEFT_BRACE ||
                current_token().get_type() == TokenType::LEFT_BRACKET) {
                has_non_simple_params = true;
                Position param_start = get_current_position();
                auto destructuring = parse_destructuring_pattern();
                if (!destructuring) {
                    add_error("Invalid destructuring pattern in arrow parameters");
                    return nullptr;
                }
                static int arrow2_destr_counter = 0;
                std::string synthetic_name = "__arrow_destr_" + std::to_string(arrow2_destr_counter++);
                Position param_pos = get_current_position();
                auto param_name = std::make_unique<Identifier>(synthetic_name, param_pos, param_pos);

                std::unique_ptr<ASTNode> default_value = nullptr;
                if (match(TokenType::ASSIGN)) {
                    advance();
                    default_value = parse_assignment_expression();
                }

                Position param_end = get_current_position();
                auto param = std::make_unique<Parameter>(std::move(param_name), std::move(default_value), false, param_start, param_end);
                param->set_destructuring_pattern(std::move(destructuring));
                params.push_back(std::move(param));

                if (match(TokenType::COMMA)) {
                    advance();
                }
                continue;
            } else if (current_token().get_type() == TokenType::ELLIPSIS) {
                has_non_simple_params = true;
                advance();
                // rest param name follows
                if (current_token().get_type() == TokenType::IDENTIFIER) {
                    Position rp_start = get_current_position();
                    auto rp_name = std::make_unique<Identifier>(current_token().get_value(),
                        current_token().get_start(), current_token().get_end());
                    advance();
                    auto rp = std::make_unique<Parameter>(std::move(rp_name), nullptr, true, rp_start, get_current_position());
                    params.push_back(std::move(rp));
                } else if (current_token().get_type() == TokenType::LEFT_BRACKET ||
                           current_token().get_type() == TokenType::LEFT_BRACE) {
                    Position rp_start = get_current_position();
                    auto destructuring = parse_destructuring_pattern();
                    if (!destructuring) {
                        add_error("Invalid destructuring rest pattern in arrow function parameters");
                        return nullptr;
                    }
                    static int arrow_rest_destr_counter = 0;
                    std::string synthetic_name = "__destr_" + std::to_string(arrow_rest_destr_counter++);
                    auto rp_name = std::make_unique<Identifier>(synthetic_name, rp_start, rp_start);
                    auto rp = std::make_unique<Parameter>(std::move(rp_name), nullptr, true, rp_start, get_current_position());
                    rp->set_destructuring_pattern(std::move(destructuring));
                    params.push_back(std::move(rp));
                }
                break;
            } else if (current_token().get_type() == TokenType::IDENTIFIER) {
                if (options_.strict_mode) {
                    const std::string& pn = current_token().get_value();
                    if (pn == "eval" || pn == "arguments") {
                        add_error("SyntaxError: '" + pn + "' cannot be used as parameter name in strict mode");
                        return nullptr;
                    }
                    if (pn == "yield") {
                        add_error("SyntaxError: 'yield' cannot be used as parameter name in strict mode");
                        return nullptr;
                    }
                }
            } else if (current_token().get_type() == TokenType::YIELD && options_.strict_mode) {
                add_error("SyntaxError: 'yield' cannot be used as parameter name in strict mode");
                return nullptr;
            } else if (current_token().get_type() == TokenType::AWAIT) {
                if (options_.in_async_body || options_.source_type_module) {
                    add_error("SyntaxError: 'await' cannot be used as parameter name in async context");
                    return nullptr;
                }
                // outside async context: await is a valid identifier param
            } else {
                advance();
                continue;
            }

            Position param_start = get_current_position();
            auto param_name = std::make_unique<Identifier>(current_token().get_value(),
                                                          current_token().get_start(), current_token().get_end());
            advance();

            std::unique_ptr<ASTNode> default_value = nullptr;
            if (match(TokenType::ASSIGN)) {
                has_non_simple_params = true;
                advance();
                default_value = parse_assignment_expression();
                if (!default_value) {
                    add_error("Invalid default parameter value");
                    return nullptr;
                }
            }
            
            Position param_end = get_current_position();
            auto param = std::make_unique<Parameter>(std::move(param_name), std::move(default_value), false, param_start, param_end);
            params.push_back(std::move(param));
            
            if (match(TokenType::COMMA)) {
                advance();
            } else if (!match(TokenType::RIGHT_PAREN)) {
                add_error("Expected ',' or ')' in arrow function parameter list");
                return nullptr;
            }
        }
        
        options_.in_arrow_params = saved_arrow_params;

        if (!consume(TokenType::RIGHT_PAREN)) {
            add_error("Expected ')' after arrow function parameters");
            return nullptr;
        }
    }
    
    // Arrow functions always use UniqueFormalParameters — duplicates are always SyntaxError
    {
        std::unordered_set<std::string> seen_params;
        auto check_name = [&](const std::string& name) -> bool {
            if (name.empty() || name[0] == '_') return true;
            if (!seen_params.insert(name).second) {
                add_error("SyntaxError: Duplicate parameter name '" + name + "' in arrow function");
                return false;
            }
            return true;
        };
        for (const auto& p : params) {
            if (p->get_name()) {
                if (!check_name(p->get_name()->get_name())) return nullptr;
            }
            if (p->has_destructuring()) {
                ASTNode* pat = p->get_destructuring_pattern();
                if (pat && pat->get_type() == ASTNode::Type::DESTRUCTURING_ASSIGNMENT) {
                    auto* da = static_cast<DestructuringAssignment*>(pat);
                    // For both ARRAY and OBJECT, get_targets() contains the binding variable names
                    for (const auto& tgt : da->get_targets()) {
                        if (!tgt) continue;
                        const std::string& tname = tgt->get_name();
                        if (tname.empty() || tname[0] == '_' || tname[0] == '.') continue;
                        if (!check_name(tname)) return nullptr;
                    }
                }
            }
        }
    }

    // Arrow functions: await/yield expressions in formals are always SyntaxError
    {
        std::string forbidden = find_forbidden_expr_in_params(params,
            options_.in_generator_body, options_.in_async_body);
        if (!forbidden.empty()) {
            add_error("SyntaxError: " + forbidden + " expression not allowed in arrow function parameters");
            return nullptr;
        }
    }

    if (!consume(TokenType::ARROW)) {
        add_error("Expected '=>' in arrow function");
        return nullptr;
    }

    std::unique_ptr<ASTNode> body;
    bool saved_sb_arrow = options_.in_class_static_block;
    options_.in_class_static_block = false;
    if (match(TokenType::LEFT_BRACE)) {
        options_.function_depth++;
        body = parse_block_statement(true);
        options_.function_depth--;
    } else {
        body = parse_assignment_expression();
    }
    options_.in_class_static_block = saved_sb_arrow;

    if (!body) {
        add_error("Expected arrow function body");
        return nullptr;
    }

    if (has_non_simple_params && body) {
        if (auto block = dynamic_cast<BlockStatement*>(body.get())) {
            if (!block->get_statements().empty()) {
                auto first_stmt = block->get_statements()[0].get();
                if (auto expr_stmt = dynamic_cast<ExpressionStatement*>(first_stmt)) {
                    if (auto literal = dynamic_cast<StringLiteral*>(expr_stmt->get_expression())) {
                        std::string value = literal->get_value();
                        if (value == "use strict") {
                            add_error("Illegal 'use strict' directive in function with non-simple parameter list");
                            return nullptr;
                        }
                    }
                }
            }
        }
    }
    
    Position end = get_current_position();
    auto arrow_expr = std::make_unique<ArrowFunctionExpression>(
        std::move(params), std::move(body), false, start, end
    );
    {
        const Token& last = previous_token();
        size_t src_end = (last.get_start().offset == last.get_end().offset)
            ? last.get_start().offset + 1
            : last.get_end().offset;
        arrow_expr->set_source_text(get_source_slice(start.offset, src_end));
    }
    return arrow_expr;
}

bool Parser::try_parse_arrow_function_params() {
    if (!match(TokenType::LEFT_PAREN)) {
        return false;
    }
    
    size_t paren_count = 1;
    size_t lookahead = 1;
    
    while (lookahead < tokens_.size() && paren_count > 0) {
        TokenType type = peek_token(lookahead).get_type();
        if (type == TokenType::LEFT_PAREN) {
            paren_count++;
        } else if (type == TokenType::RIGHT_PAREN) {
            paren_count--;
        }
        lookahead++;
    }
    
    if (lookahead < tokens_.size()) {
        return peek_token(lookahead).get_type() == TokenType::ARROW;
    }
    
    return false;
}

std::unique_ptr<ASTNode> Parser::parse_yield_expression() {
    Position start = get_current_position();
    
    // Save yield token's line BEFORE consuming (advance skips whitespace)
    size_t yield_end_line = current_token().get_end().line;

    if (!consume(TokenType::YIELD)) {
        add_error("Expected 'yield'");
        return nullptr;
    }

    // [no LineTerminator here] before * or argument
    bool same_line = (!at_end() && current_token().get_start().line == yield_end_line);

    // yield [LT] * is SyntaxError: * on new line cannot be delegate
    if (!same_line && match(TokenType::MULTIPLY) && options_.in_generator_body) {
        add_error("SyntaxError: Newline is not allowed before '*' in yield expression");
        return nullptr;
    }

    bool is_delegate = false;
    if (same_line && match(TokenType::MULTIPLY)) {
        is_delegate = true;
        advance();
        // Spec: no [LT here] restriction between * and the expression -- the expression
        // may appear on a new line after the *.
    }

    std::unique_ptr<ASTNode> argument = nullptr;
    if (is_delegate) {
        // yield * always requires an argument (on any line after the *)
        if (!at_end()) {
            TokenType nt = current_token().get_type();
            if (nt != TokenType::SEMICOLON && nt != TokenType::RIGHT_BRACE &&
                nt != TokenType::RIGHT_PAREN && nt != TokenType::RIGHT_BRACKET &&
                nt != TokenType::COMMA && nt != TokenType::EOF_TOKEN &&
                nt != TokenType::COLON) {
                argument = parse_assignment_expression();
            }
        }
    } else if (same_line && !at_end()) {
        TokenType nt = current_token().get_type();
        if (nt != TokenType::SEMICOLON && nt != TokenType::RIGHT_BRACE &&
            nt != TokenType::RIGHT_PAREN && nt != TokenType::RIGHT_BRACKET &&
            nt != TokenType::COMMA && nt != TokenType::EOF_TOKEN &&
            nt != TokenType::COLON) {
            argument = parse_assignment_expression();
        }
    }
    
    Position end = get_current_position();
    return std::make_unique<YieldExpression>(std::move(argument), is_delegate, start, end);
}

std::unique_ptr<ASTNode> Parser::parse_import_expression() {
    Position start = get_current_position();

    if (!consume(TokenType::IMPORT)) {
        add_error("Expected 'import'");
        return nullptr;
    }

    // import.X -- handle meta-properties and reject unsupported proposals
    if (match(TokenType::DOT)) {
        advance(); // consume .
        if (current_token().get_type() == TokenType::IDENTIFIER) {
            const std::string& prop = current_token().get_value();
            if (prop == "meta") {
                if (current_token().has_escaped_keyword()) {
                    add_error("SyntaxError: 'meta' in import.meta cannot use escape sequences");
                    return nullptr;
                }
                if (!options_.source_type_module) {
                    add_error("SyntaxError: import.meta is only valid in module code");
                    return nullptr;
                }
                Position end = current_token().get_end();
                advance();
                return std::make_unique<MetaProperty>("import", "meta", start, end);
            }
            if (prop == "source") {
                advance(); // consume 'source'
                // import.source must be immediately called: import.source(AssignmentExpression)
                if (!match(TokenType::LEFT_PAREN)) {
                    add_error("SyntaxError: import.source must be called as import.source(specifier)");
                    return nullptr;
                }
                advance(); // consume (
                if (match(TokenType::RIGHT_PAREN)) {
                    add_error("SyntaxError: import.source() requires a specifier argument");
                    return nullptr;
                }
                if (match(TokenType::ELLIPSIS)) {
                    add_error("SyntaxError: import.source() does not allow spread arguments");
                    return nullptr;
                }
                auto specifier = parse_assignment_expression();
                if (!specifier) {
                    add_error("Expected module specifier in import.source()");
                    return nullptr;
                }
                if (match(TokenType::COMMA)) {
                    advance();
                    if (!match(TokenType::RIGHT_PAREN)) {
                        parse_assignment_expression();
                    }
                    if (match(TokenType::COMMA)) advance();
                }
                if (!consume(TokenType::RIGHT_PAREN)) {
                    add_error("Expected ')' after import.source specifier");
                    return nullptr;
                }
                Position end = get_current_position();
                // Use a distinct sentinel name so the runtime handler can reject with SyntaxError
                // per spec GetModuleSource (SourceTextModuleRecord always throws SyntaxError).
                auto import_id = std::make_unique<Identifier>("__import_source__", start, start);
                std::vector<std::unique_ptr<ASTNode>> args;
                args.push_back(std::move(specifier));
                return std::make_unique<CallExpression>(std::move(import_id), std::move(args), start, end);
            }
            if (prop == "defer") {
                advance(); // consume 'defer'
                if (!consume(TokenType::LEFT_PAREN)) {
                    add_error("SyntaxError: import.defer requires arguments");
                    return nullptr;
                }
                std::vector<std::unique_ptr<ASTNode>> args;
                if (match(TokenType::RIGHT_PAREN)) {
                    add_error("SyntaxError: import.defer() requires a specifier argument");
                    return nullptr;
                }
                auto arg = parse_assignment_expression();
                if (arg) args.push_back(std::move(arg));
                if (match(TokenType::COMMA)) advance();
                if (!consume(TokenType::RIGHT_PAREN)) {
                    add_error("Expected ')' after import.defer argument");
                    return nullptr;
                }
                Position end = get_current_position();
                auto import_id = std::make_unique<Identifier>("import", start, start);
                return std::make_unique<CallExpression>(std::move(import_id), std::move(args), start, end);
            }
            // import.UNKNOWN etc. are SyntaxErrors
            add_error("SyntaxError: Unknown import meta-property '" + prop + "'");
            return nullptr;
        }
        add_error("SyntaxError: Invalid import meta-property");
        return nullptr;
    }

    if (!consume(TokenType::LEFT_PAREN)) {
        add_error("SyntaxError: Expected '(' after 'import'");
        return nullptr;
    }

    auto specifier = parse_assignment_expression();
    if (!specifier) {
        add_error("Expected module specifier in import()");
        return nullptr;
    }

    // Optional second argument: import attributes -- parse and ignore
    // Must reset no_in_mode_ because 'in' is valid inside call args (e.g. import(x, a in b))
    if (match(TokenType::COMMA)) {
        advance();
        if (!match(TokenType::RIGHT_PAREN)) {
            bool saved_no_in_imp = no_in_mode_;
            no_in_mode_ = false;
            parse_assignment_expression(); // consume and discard options argument
            no_in_mode_ = saved_no_in_imp;
        }
        if (match(TokenType::COMMA)) advance();
    }

    if (!consume(TokenType::RIGHT_PAREN)) {
        add_error("Expected ')' after import specifier");
        return nullptr;
    }

    Position end = get_current_position();

    auto import_id = std::make_unique<Identifier>("import", start, get_current_position());
    std::vector<std::unique_ptr<ASTNode>> args;
    args.push_back(std::move(specifier));

    return std::make_unique<CallExpression>(std::move(import_id), std::move(args), start, end);
}

std::unique_ptr<ASTNode> Parser::parse_return_statement() {
    Position start = get_current_position();

    if (!consume(TokenType::RETURN)) {
        add_error("Expected 'return'");
        return nullptr;
    }

    if (options_.in_class_static_block) {
        add_error("SyntaxError: Illegal return statement inside class static block");
        return nullptr;
    }
    if (options_.function_depth == 0 && !options_.allow_return_outside_function) {
        add_error("SyntaxError: Illegal return statement");
        return nullptr;
    }
    
    std::unique_ptr<ASTNode> argument = nullptr;
    
    if (!match(TokenType::SEMICOLON) && !at_end() && 
        current_token().get_start().line == start.line) {
        argument = parse_expression();
        if (!argument) {
            add_error("Invalid return expression");
            return nullptr;
        }
    }
    
    consume_if_match(TokenType::SEMICOLON);
    
    Position end = get_current_position();
    return std::make_unique<ReturnStatement>(std::move(argument), start, end);
}

std::unique_ptr<ASTNode> Parser::parse_break_statement() {
    Position start = get_current_position();

    if (!consume(TokenType::BREAK)) {
        add_error("Expected 'break'");
        return nullptr;
    }

    // break without label requires enclosing loop or switch
    bool has_label = (current_token().get_type() == TokenType::IDENTIFIER &&
                      current_token().get_start().line == start.line);
    if (!has_label && options_.loop_depth == 0 && options_.switch_depth == 0) {
        add_error("SyntaxError: Illegal break statement");
        return nullptr;
    }

    std::string label;
    if (current_token().get_type() == TokenType::IDENTIFIER &&
        current_token().get_start().line == start.line) {
        label = current_token().get_value();
        advance();
    }

    if (!label.empty() && !options_.active_labels.count(label)) {
        add_error("SyntaxError: Undefined label '" + label + "'");
        return nullptr;
    }

    consume_if_match(TokenType::SEMICOLON);

    Position end = get_current_position();
    return std::make_unique<BreakStatement>(start, end, label);
}

std::unique_ptr<ASTNode> Parser::parse_continue_statement() {
    Position start = get_current_position();

    if (!consume(TokenType::CONTINUE)) {
        add_error("Expected 'continue'");
        return nullptr;
    }

    bool has_label = (current_token().get_type() == TokenType::IDENTIFIER &&
                      current_token().get_start().line == start.line);
    if (!has_label && options_.loop_depth == 0) {
        add_error("SyntaxError: Illegal continue statement");
        return nullptr;
    }

    std::string label;
    if (current_token().get_type() == TokenType::IDENTIFIER &&
        current_token().get_start().line == start.line) {
        label = current_token().get_value();
        advance();
    }

    if (!label.empty() && !options_.loop_labels.count(label)) {
        add_error("SyntaxError: Undefined or non-loop label '" + label + "'");
        return nullptr;
    }

    consume_if_match(TokenType::SEMICOLON);

    Position end = get_current_position();
    return std::make_unique<ContinueStatement>(start, end, label);
}


namespace ParserFactory {

std::unique_ptr<Parser> create_expression_parser(const std::string& source) {
    Lexer lexer(source);
    TokenSequence tokens = lexer.tokenize();
    return std::make_unique<Parser>(std::move(tokens));
}

std::unique_ptr<Parser> create_statement_parser(const std::string& source) {
    Lexer lexer(source);
    TokenSequence tokens = lexer.tokenize();
    return std::make_unique<Parser>(std::move(tokens));
}

std::unique_ptr<Parser> create_module_parser(const std::string& source) {
    Lexer::LexerOptions lex_opts;
    lex_opts.source_type_module = true;
    Lexer lexer(source, lex_opts);
    TokenSequence tokens = lexer.tokenize();
    Parser::ParseOptions options;
    options.source_type_module = true;
    options.strict_mode = true;
    return std::make_unique<Parser>(std::move(tokens), options);
}

}

std::unique_ptr<ASTNode> Parser::parse_object_literal() {
    Position start = get_current_position();
    // Nested property-value parses may set last_expr_was_parenthesized_; reset on exit
    // so the object literal itself is never considered "parenthesized" by its callers.
    struct ResetParenFlag {
        bool& flag;
        ~ResetParenFlag() { flag = false; }
    } reset_guard{last_expr_was_parenthesized_};

    if (!consume(TokenType::LEFT_BRACE)) {
        add_error("Expected '{'");
        return nullptr;
    }

    // 'in' is always allowed inside {...} regardless of outer no_in_mode_
    bool saved_no_in_ol = no_in_mode_;
    no_in_mode_ = false;
    
    std::vector<std::unique_ptr<ObjectLiteral::Property>> properties;
    
    if (match(TokenType::RIGHT_BRACE)) {
        advance();
        no_in_mode_ = saved_no_in_ol;
        Position end = get_current_position();
        return std::make_unique<ObjectLiteral>(std::move(properties), start, end);
    }
    
    do {
        if (match(TokenType::ELLIPSIS)) {
            auto spread = parse_spread_element();
            if (!spread) {
                add_error("Invalid spread element in object literal");
                return nullptr;
            }
            properties.push_back(std::make_unique<ObjectLiteral::Property>(
                nullptr, std::move(spread), false, false
            ));
            
            if (match(TokenType::COMMA)) {
                advance();
                continue;
            } else {
                break;
            }
        }
        
        Position prop_start = get_current_position();
        ObjectLiteral::PropertyType property_type = ObjectLiteral::PropertyType::Value;
        bool is_async = false;

        if (match(TokenType::IDENTIFIER)) {
            if (current_token().get_value() == "get") {
                if (current_token().has_escaped_keyword()) {
                    // get etc. -- escaped get keyword is SyntaxError
                    size_t saved_pos = current_token_index_;
                    advance();
                    bool is_getter_syntax = !match(TokenType::LEFT_PAREN) &&
                        (match(TokenType::LEFT_BRACKET) || match(TokenType::IDENTIFIER) ||
                         match(TokenType::STRING) || match(TokenType::NUMBER) || is_keyword_token(current_token().get_type())) &&
                        !match(TokenType::COLON);
                    current_token_index_ = saved_pos;
                    if (is_getter_syntax) {
                        add_error("SyntaxError: `get` cannot contain unicode escape sequences");
                        return nullptr;
                    }
                }
                size_t saved_pos = current_token_index_;
                advance();

                bool is_method_shorthand = match(TokenType::LEFT_PAREN);
                bool is_getter_syntax = !is_method_shorthand && (match(TokenType::LEFT_BRACKET) || match(TokenType::IDENTIFIER) || match(TokenType::STRING) || match(TokenType::NUMBER) || is_keyword_token(current_token().get_type()));
                bool is_normal_property = match(TokenType::COLON);

                current_token_index_ = saved_pos;

                if (is_getter_syntax && !is_normal_property) {
                    property_type = ObjectLiteral::PropertyType::Getter;
                    advance();
                }
            } else if (current_token().get_value() == "set") {
                if (current_token().has_escaped_keyword()) {
                    size_t saved_pos = current_token_index_;
                    advance();
                    bool is_setter_syntax = !match(TokenType::LEFT_PAREN) &&
                        (match(TokenType::LEFT_BRACKET) || match(TokenType::IDENTIFIER) ||
                         match(TokenType::STRING) || match(TokenType::NUMBER) || is_keyword_token(current_token().get_type())) &&
                        !match(TokenType::COLON);
                    current_token_index_ = saved_pos;
                    if (is_setter_syntax) {
                        add_error("SyntaxError: `set` cannot contain unicode escape sequences");
                        return nullptr;
                    }
                }
                size_t saved_pos = current_token_index_;
                advance();

                bool is_method_shorthand = match(TokenType::LEFT_PAREN);
                bool is_setter_syntax = !is_method_shorthand && (match(TokenType::LEFT_BRACKET) || match(TokenType::IDENTIFIER) || match(TokenType::STRING) || match(TokenType::NUMBER) || is_keyword_token(current_token().get_type()));
                bool is_normal_property = match(TokenType::COLON);

                current_token_index_ = saved_pos;

                if (is_setter_syntax && !is_normal_property) {
                    property_type = ObjectLiteral::PropertyType::Setter;
                    advance();
                }
            } else if (current_token().get_value() == "async") {
                if (current_token().has_escaped_keyword()) {
                    add_error("SyntaxError: `async` cannot contain unicode escape sequences");
                    return nullptr;
                }
                size_t async_line = current_token().get_end().line;
                // Peek at next significant token to check for line terminator
                size_t peek_idx = current_token_index_ + 1;
                while (peek_idx < tokens_.size() &&
                       (tokens_[peek_idx].get_type() == TokenType::NEWLINE ||
                        tokens_[peek_idx].get_type() == TokenType::WHITESPACE ||
                        tokens_[peek_idx].get_type() == TokenType::COMMENT)) {
                    peek_idx++;
                }
                bool has_lt = (peek_idx < tokens_.size() &&
                               tokens_[peek_idx].get_start().line > async_line);
                // Check if there's a method after (not just a value property)
                bool next_is_method = (peek_idx + 1 < tokens_.size() &&
                    (tokens_[peek_idx].get_type() == TokenType::IDENTIFIER ||
                     tokens_[peek_idx].get_type() == TokenType::LEFT_BRACKET ||
                     is_keyword_token(tokens_[peek_idx].get_type())) &&
                    tokens_[peek_idx + 1].get_type() == TokenType::LEFT_PAREN);
                if (has_lt && next_is_method) {
                    add_error("SyntaxError: Line terminator not allowed between async and method name");
                    return nullptr;
                }
                is_async = true;
                advance();
            }
        } else if (match(TokenType::ASYNC)) {
            size_t async_line = current_token().get_end().line;
            size_t peek_idx = current_token_index_ + 1;
            while (peek_idx < tokens_.size() &&
                   (tokens_[peek_idx].get_type() == TokenType::NEWLINE ||
                    tokens_[peek_idx].get_type() == TokenType::WHITESPACE ||
                    tokens_[peek_idx].get_type() == TokenType::COMMENT)) {
                peek_idx++;
            }
            bool async_has_lt = (peek_idx < tokens_.size() &&
                                 tokens_[peek_idx].get_start().line > async_line);
            bool async_next_is_method = (peek_idx + 1 < tokens_.size() &&
                (tokens_[peek_idx].get_type() == TokenType::IDENTIFIER ||
                 tokens_[peek_idx].get_type() == TokenType::LEFT_BRACKET ||
                 tokens_[peek_idx].get_type() == TokenType::STRING ||
                 tokens_[peek_idx].get_type() == TokenType::NUMBER ||
                 tokens_[peek_idx].get_type() == TokenType::MULTIPLY ||
                 is_keyword_token(tokens_[peek_idx].get_type())));
            if (async_has_lt && async_next_is_method) {
                add_error("SyntaxError: Line terminator not allowed between 'async' and method name");
                return nullptr;
            }
            is_async = true;
            advance();
        }

        bool is_generator = false;
        if (match(TokenType::MULTIPLY)) {
            is_generator = true;
            advance();
        }

        std::unique_ptr<ASTNode> key;
        bool computed = false;
        
        if (match(TokenType::LEFT_BRACKET)) {
            advance();
            computed = true;
            key = parse_assignment_expression();
            if (!key) {
                add_error("Expected expression for computed property key");
                return nullptr;
            }
            if (!consume(TokenType::RIGHT_BRACKET)) {
                add_error("Expected ']' after computed property key");
                return nullptr;
            }
        } else if (match(TokenType::IDENTIFIER)) {
            if (current_token().has_escaped_keyword()) {
                std::string kname = current_token().get_value();
                Position ks = current_token().get_start();
                Position ke = current_token().get_end();
                advance();
                key = std::make_unique<Identifier>(kname, ks, ke);
            } else {
                key = parse_identifier();
            }
        } else if (match(TokenType::STRING)) {
            key = parse_string_literal();
        } else if (match(TokenType::NUMBER)) {
            key = parse_number_literal();
        } else if (match(TokenType::BIGINT_LITERAL)) {
            key = parse_bigint_literal();
        } else if (is_keyword_token(current_token().get_type())) {
            key = std::make_unique<Identifier>(current_token().get_value(),
                                               current_token().get_start(), current_token().get_end());
            advance();
        } else {
            add_error("Expected property key");
            return nullptr;
        }

        if (property_type == ObjectLiteral::PropertyType::Getter || property_type == ObjectLiteral::PropertyType::Setter) {
            if (!match(TokenType::LEFT_PAREN)) {
                add_error(property_type == ObjectLiteral::PropertyType::Getter ?
                          "Expected '(' after getter property key" :
                          "Expected '(' after setter property key");
                return nullptr;
            }
        }

        if (match(TokenType::LEFT_PAREN)) {
            advance();

            std::vector<std::unique_ptr<Parameter>> params;
            bool obj_non_simple = false;
            bool saved_oap = options_.in_arrow_params;
            bool saved_async_obj_params = options_.in_async_body;
            bool saved_csb_obj_params = options_.in_class_static_block;
            bool saved_cm_obj_params = options_.in_class_method;
            options_.in_arrow_params = true;
            options_.in_class_static_block = false;
            options_.in_class_method = true; // super is valid in method param defaults
            if (is_async) options_.in_async_body = true;
            if (!match(TokenType::RIGHT_PAREN)) {
                do {
                    Position param_start = current_token().get_start();
                    bool is_rest = false;

                    if (match(TokenType::ELLIPSIS)) {
                        is_rest = true;
                        obj_non_simple = true;
                        advance();
                    }

                    std::unique_ptr<Identifier> param_name = nullptr;

                    if (current_token().get_type() == TokenType::LEFT_BRACKET ||
                        current_token().get_type() == TokenType::LEFT_BRACE) {
                        obj_non_simple = true;
                        auto destructuring = parse_destructuring_pattern();
                        if (!destructuring) {
                            add_error("Invalid destructuring pattern in parameters");
                            return nullptr;
                        }
                        static int async_destr_counter = 0;
                        std::string synthetic_name = "__asyncdestr_" + std::to_string(async_destr_counter++);
                        Position param_pos = get_current_position();
                        param_name = std::make_unique<Identifier>(synthetic_name, param_pos, param_pos);

                        std::unique_ptr<ASTNode> default_value = nullptr;
                        if (is_rest && match(TokenType::ASSIGN)) {
                            add_error("SyntaxError: Rest parameter cannot have a default value");
                            return nullptr;
                        } else if (!is_rest && match(TokenType::ASSIGN)) {
                            advance();
                            default_value = parse_assignment_expression();
                            if (!default_value) {
                                add_error("Expected expression after '=' in parameter default");
                                return nullptr;
                            }
                        }

                        Position param_end = get_current_position();
                        auto param = std::make_unique<Parameter>(std::move(param_name), std::move(default_value), is_rest, param_start, param_end);
                        param->set_destructuring_pattern(std::move(destructuring));
                        params.push_back(std::move(param));

                        if (is_rest) {
                            if (match(TokenType::COMMA)) {
                                add_error("SyntaxError: Rest parameter must be last formal parameter");
                                return nullptr;
                            }
                            break;
                        }
                        if (match(TokenType::COMMA)) {
                            advance();
                            if (match(TokenType::RIGHT_PAREN)) break; // trailing comma
                        } else {
                            break;
                        }
                        continue;
                    } else if (current_token().get_type() == TokenType::IDENTIFIER) {
                        param_name = std::make_unique<Identifier>(
                            current_token().get_value(),
                            current_token().get_start(),
                            current_token().get_end()
                        );
                        advance();
                    } else if ((!is_async && !options_.source_type_module &&
                                current_token().get_type() == TokenType::AWAIT) ||
                               (!is_generator && !options_.strict_mode &&
                                current_token().get_type() == TokenType::YIELD) ||
                               ((current_token().get_type() == TokenType::LET ||
                                 current_token().get_type() == TokenType::STATIC) &&
                                !options_.strict_mode)) {
                        param_name = std::make_unique<Identifier>(current_token().get_value(),
                                                                  current_token().get_start(), current_token().get_end());
                        advance();
                    } else {
                        add_error("Expected parameter name or destructuring pattern");
                        return nullptr;
                    }

                    std::unique_ptr<ASTNode> default_value = nullptr;
                    if (!is_rest && match(TokenType::ASSIGN)) {
                        obj_non_simple = true;
                        advance();
                        default_value = parse_assignment_expression();
                        if (!default_value) {
                            add_error("Expected expression after '=' in parameter default");
                            return nullptr;
                        }
                    } else if (is_rest && match(TokenType::ASSIGN)) {
                        add_error("SyntaxError: Rest parameter cannot have a default value");
                        return nullptr;
                    }

                    Position param_end = get_current_position();
                    auto param = std::make_unique<Parameter>(std::move(param_name), std::move(default_value), is_rest, param_start, param_end);
                    params.push_back(std::move(param));

                    if (is_rest) {
                        if (match(TokenType::COMMA)) {
                            add_error("SyntaxError: Rest parameter must be last formal parameter");
                            return nullptr;
                        }
                        break;
                    }
                    if (match(TokenType::COMMA)) {
                        advance();
                        if (match(TokenType::RIGHT_PAREN)) break; // trailing comma
                    } else {
                        break;
                    }
                } while (!at_end());
            }

            options_.in_arrow_params = saved_oap;
            options_.in_async_body = saved_async_obj_params;
            options_.in_class_static_block = saved_csb_obj_params;
            options_.in_class_method = saved_cm_obj_params;

            if (!consume(TokenType::RIGHT_PAREN)) {
                add_error("Expected ')' after parameters");
                return nullptr;
            }

            // Getter: no params. Setter: exactly one simple param.
            if (property_type == ObjectLiteral::PropertyType::Getter && !params.empty()) {
                add_error("SyntaxError: Getter must have no formal parameters");
                return nullptr;
            }
            if (property_type == ObjectLiteral::PropertyType::Setter) {
                if (params.size() != 1) {
                    add_error("SyntaxError: Setter must have exactly one formal parameter");
                    return nullptr;
                }
                // Setters may have default values and destructuring, but not rest
                if (params[0]->is_rest()) {
                    add_error("SyntaxError: Setter parameter may not be a rest parameter");
                    return nullptr;
                }
            }

            // All methods use UniqueFormalParameters (per spec MethodDefinition grammar)
            for (size_t pi = 0; pi < params.size(); pi++) {
                if (!params[pi]->get_name()) continue;
                const std::string& pn = params[pi]->get_name()->get_name();
                if (pn.empty() || pn[0] == '_') continue;
                // eval/arguments forbidden in strict mode only (non-strict allows them as param names)
                if (options_.strict_mode) {
                    if (pn == "eval" || pn == "arguments") {
                        add_error("SyntaxError: '" + pn + "' cannot be a parameter name in strict mode");
                        return nullptr;
                    }
                }
                for (size_t pj = pi + 1; pj < params.size(); pj++) {
                    if (!params[pj]->get_name()) continue;
                    if (params[pj]->get_name()->get_name() == pn) {
                        add_error("SyntaxError: Duplicate parameter name not allowed in method");
                        return nullptr;
                    }
                }
            }

            if (!match(TokenType::LEFT_BRACE)) {
                add_error("Expected '{' for method body");
                return nullptr;
            }

            bool saved_ab = options_.in_async_body;
            bool saved_gb = options_.in_generator_body;
            bool saved_sb_ol = options_.in_class_static_block;
            bool saved_cm_ol = options_.in_class_method;
            bool saved_ic_ol = options_.in_constructor;
            if (is_async) options_.in_async_body = true;
            options_.in_generator_body = is_generator;
            options_.in_class_static_block = false;
            options_.in_class_method = true;
            options_.in_constructor = false;
            options_.function_depth++;
            options_.non_arrow_function_depth++;
            auto body = parse_block_statement(true);
            options_.function_depth--;
            options_.non_arrow_function_depth--;
            options_.in_async_body = saved_ab;
            options_.in_generator_body = saved_gb;
            options_.in_class_static_block = saved_sb_ol;
            options_.in_class_method = saved_cm_ol;
            options_.in_constructor = saved_ic_ol;
            if (!body) {
                add_error("Expected method body");
                return nullptr;
            }
            if (obj_non_simple) {
                auto* blk = static_cast<BlockStatement*>(body.get());
                if (blk && !blk->get_statements().empty()) {
                    auto* fs = blk->get_statements()[0].get();
                    if (auto* es = dynamic_cast<ExpressionStatement*>(fs)) {
                        if (auto* sl = dynamic_cast<StringLiteral*>(es->get_expression())) {
                            if (sl->get_value() == "use strict") {
                                add_error("SyntaxError: Illegal 'use strict' directive in function with non-simple parameter list");
                                return nullptr;
                            }
                        }
                    }
                }
            }
            if (!options_.strict_mode) {
                auto* blk = static_cast<BlockStatement*>(body.get());
                bool body_strict = blk && !blk->get_statements().empty() && [&] {
                    auto* es = dynamic_cast<ExpressionStatement*>(blk->get_statements()[0].get());
                    if (es) {
                        auto* sl = dynamic_cast<StringLiteral*>(es->get_expression());
                        return sl && sl->get_value() == "use strict";
                    }
                    return false;
                }();
                if (body_strict) {
                    for (const auto& p : params) {
                        if (!p->get_name()) continue;
                        const std::string& pn = p->get_name()->get_name();
                        if (pn == "eval" || pn == "arguments") {
                            add_error("SyntaxError: '" + pn + "' cannot be a parameter name in strict mode");
                            return nullptr;
                        }
                    }
                }
            }
            {
                auto* blk = static_cast<BlockStatement*>(body.get());
                std::string dup = check_params_body_lex_conflict(params, blk);
                if (!dup.empty()) {
                    add_error("SyntaxError: Identifier '" + dup + "' is already declared (parameter name shadows lexical declaration)");
                    return nullptr;
                }
            }

            std::unique_ptr<ASTNode> method_value;
            if (is_async && is_generator) {
                auto block_body = std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body.release()));
                method_value = std::make_unique<FunctionExpression>(
                    nullptr,
                    std::move(params),
                    std::move(block_body),
                    key->get_start(),
                    get_current_position(),
                    true,  // is_generator
                    true   // is_async
                );
            } else if (is_async) {
                auto block_body = std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body.release()));
                method_value = std::make_unique<AsyncFunctionExpression>(
                    nullptr,
                    std::move(params),
                    std::move(block_body),
                    key->get_start(),
                    get_current_position()
                );
            } else if (is_generator) {
                auto block_body = std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body.release()));
                method_value = std::make_unique<FunctionExpression>(
                    nullptr,
                    std::move(params),
                    std::move(block_body),
                    key->get_start(),
                    get_current_position(),
                    true // is_generator
                );
            } else {
                auto block_body = std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body.release()));
                method_value = std::make_unique<FunctionExpression>(
                    nullptr,
                    std::move(params),
                    std::move(block_body),
                    key->get_start(),
                    get_current_position()
                );
            }
            
            {
                std::string method_src = get_source_slice(prop_start.offset, last_meaningful_token().get_start().offset + 1);
                if (!method_src.empty()) {
                    if (method_value->get_type() == ASTNode::Type::FUNCTION_EXPRESSION) {
                        static_cast<FunctionExpression*>(method_value.get())->set_source_text(method_src);
                    }
                }
            }

            ObjectLiteral::PropertyType final_type = property_type;
            if (final_type == ObjectLiteral::PropertyType::Value) {
                final_type = ObjectLiteral::PropertyType::Method;
            }
            auto property = std::make_unique<ObjectLiteral::Property>(
                std::move(key), std::move(method_value), computed, final_type
            );
            properties.push_back(std::move(property));
        } else {
            if (match(TokenType::COMMA) || match(TokenType::RIGHT_BRACE)) {
                if (!key) {
                    add_error("Invalid shorthand property");
                    return nullptr;
                }
                // Generator/async prefix requires a method body
                if (is_generator) {
                    add_error("SyntaxError: Generator method requires a function body");
                    return nullptr;
                }
                if (is_async) {
                    add_error("SyntaxError: Async method requires a function body");
                    return nullptr;
                }
                // Computed keys cannot be shorthands: ({[x]}) is a SyntaxError
                if (computed) {
                    add_error("SyntaxError: Computed property name cannot be shorthand");
                    return nullptr;
                }

                if (auto* identifier_key = dynamic_cast<Identifier*>(key.get())) {
                    const std::string& shn = identifier_key->get_name();
                    // Reserved words are never valid as shorthand identifier references
                    static const std::unordered_set<std::string> always_reserved = {
                        "false","true","null","this","super","enum",
                        "break","case","catch","class","const","continue","debugger",
                        "default","delete","do","else","export","extends","finally",
                        "for","function","if","import","in","instanceof","new",
                        "return","switch","throw","try","typeof","var","void","while","with"
                    };
                    // Unicode escape sequences cannot be used for reserved words in identifiers
                    if (identifier_key->has_escaped_keyword() && (
                        always_reserved.count(shn) ||
                        (options_.strict_mode && (shn == "implements" || shn == "interface" ||
                         shn == "let" || shn == "package" || shn == "private" ||
                         shn == "protected" || shn == "public" || shn == "static" || shn == "yield")))) {
                        add_error("SyntaxError: Unicode escape sequences are not allowed in this reserved word");
                        return nullptr;
                    }
                    if (always_reserved.count(shn)) {
                        add_error("SyntaxError: '" + shn + "' cannot be used as an identifier reference");
                        return nullptr;
                    }
                    if (options_.in_class_static_block &&
                        (shn == "await" || shn == "arguments")) {
                        add_error("SyntaxError: '" + shn + "' cannot be used as identifier in class static block");
                        return nullptr;
                    }
                    if (options_.strict_mode) {
                        static const std::unordered_set<std::string> strict_future = {
                            "implements","interface","let","package","private",
                            "protected","public","static","yield"
                        };
                        if (strict_future.count(shn)) {
                            add_error("SyntaxError: '" + shn + "' is a reserved word in strict mode");
                            return nullptr;
                        }
                        if (shn == "eval" || shn == "arguments") {
                            add_error("SyntaxError: '" + shn + "' cannot be used as identifier in strict mode");
                            return nullptr;
                        }
                    }
                    auto value = std::make_unique<Identifier>(
                        shn,
                        identifier_key->get_start(),
                        identifier_key->get_end()
                    );

                    auto property = std::make_unique<ObjectLiteral::Property>(
                        std::move(key), std::move(value), computed, false
                    );
                    property->shorthand = true;
                    properties.push_back(std::move(property));
                } else {
                    add_error("Shorthand properties can only be used with identifier keys");
                    return nullptr;
                }
            } else if (match(TokenType::ASSIGN) && dynamic_cast<Identifier*>(key.get())) {
                // CoverInitializedName: {a = expr} - valid in destructuring assignment patterns
                advance();
                auto default_expr = parse_assignment_expression();
                if (!default_expr) {
                    add_error("Expected expression after '=' in shorthand default");
                    return nullptr;
                }
                auto* id_key = static_cast<Identifier*>(key.get());
                auto left_id = std::make_unique<Identifier>(id_key->get_name(), id_key->get_start(), id_key->get_end());
                Position assign_start = id_key->get_start();
                Position assign_end = default_expr->get_end();
                auto assign_val = std::make_unique<AssignmentExpression>(
                    std::move(left_id), AssignmentExpression::Operator::ASSIGN,
                    std::move(default_expr), assign_start, assign_end
                );
                auto property = std::make_unique<ObjectLiteral::Property>(
                    std::move(key), std::move(assign_val), computed, false
                );
                property->shorthand = true;
                properties.push_back(std::move(property));
            } else {
                if (!consume(TokenType::COLON)) {
                    add_error("Expected ':' after property key");
                    return nullptr;
                }

                bool saved_iae2 = options_.in_array_element;
                options_.in_array_element = true;
                auto value = parse_assignment_expression();
                options_.in_array_element = saved_iae2;
                if (!value) {
                    add_error("Expected property value");
                    return nullptr;
                }

                auto property = std::make_unique<ObjectLiteral::Property>(
                    std::move(key), std::move(value), computed, false
                );
                properties.push_back(std::move(property));
            }
        }
        
        if (match(TokenType::COMMA)) {
            advance();
            if (match(TokenType::RIGHT_BRACE)) {
                break;
            }
        } else {
            break;
        }
        
    } while (!at_end() && !match(TokenType::RIGHT_BRACE));
    
    if (!consume(TokenType::RIGHT_BRACE)) {
        add_error("Expected '}' to close object literal");
        return nullptr;
    }

    // Annex B early error: duplicate __proto__ in object initializer.
    // Applies only to non-shorthand, non-computed PropertyName:AssignmentExpression form.
    // Does NOT apply to destructuring assignment patterns (next token is '=').
    bool is_destructuring_target = match(TokenType::ASSIGN);
    if (!is_destructuring_target) {
        int proto_count = 0;
        for (const auto& prop : properties) {
            if (prop->shorthand) continue; // shorthand __proto__ is allowed
            if (!prop->computed && prop->type == ObjectLiteral::PropertyType::Value
                && prop->key && prop->key->get_type() == ASTNode::Type::IDENTIFIER) {
                auto* id = static_cast<Identifier*>(prop->key.get());
                if (id->get_name() == "__proto__") proto_count++;
            } else if (!prop->computed && prop->type == ObjectLiteral::PropertyType::Value
                && prop->key && prop->key->get_type() == ASTNode::Type::STRING_LITERAL) {
                auto* sl = static_cast<StringLiteral*>(prop->key.get());
                if (sl->get_value() == "__proto__") proto_count++;
            }
        }
        if (proto_count > 1) {
            add_error("Duplicate __proto__ fields are not allowed in object literals");
            return nullptr;
        }
    }

    no_in_mode_ = saved_no_in_ol;
    Position end = get_current_position();
    return std::make_unique<ObjectLiteral>(std::move(properties), start, end);
}

std::unique_ptr<ASTNode> Parser::parse_array_literal() {
    Position start = get_current_position();

    if (!consume(TokenType::LEFT_BRACKET)) {
        add_error("Expected '['");
        return nullptr;
    }

    // 'in' is always allowed inside [...] regardless of outer no_in_mode_
    bool saved_no_in_al = no_in_mode_;
    no_in_mode_ = false;

    std::vector<std::unique_ptr<ASTNode>> elements;
    
    if (match(TokenType::RIGHT_BRACKET)) {
        advance();
        no_in_mode_ = saved_no_in_al;
        Position end = get_current_position();
        return std::make_unique<ArrayLiteral>(std::move(elements), start, end);
    }

    do {
        if (match(TokenType::COMMA)) {
            elements.push_back(std::make_unique<UndefinedLiteral>(get_current_position(), get_current_position()));
        } else {
            if (match(TokenType::ELLIPSIS)) {
                auto spread = parse_spread_element();
                if (!spread) {
                    add_error("Invalid spread element");
                    no_in_mode_ = saved_no_in_al;
                    return nullptr;
                }
                elements.push_back(std::move(spread));
            } else {
                bool saved_iae = options_.in_array_element;
                options_.in_array_element = true;
                auto element = parse_assignment_expression();
                options_.in_array_element = saved_iae;
                if (!element) {
                    add_error("Expected array element");
                    no_in_mode_ = saved_no_in_al;
                    return nullptr;
                }
                elements.push_back(std::move(element));
            }
        }

        if (match(TokenType::COMMA)) {
            advance();
            if (match(TokenType::RIGHT_BRACKET)) {
                // trailing comma after spread -> add sentinel for destructuring check
                if (!elements.empty() && elements.back() &&
                    elements.back()->get_type() == ASTNode::Type::SPREAD_ELEMENT) {
                    Position p = get_current_position();
                    elements.push_back(std::make_unique<UndefinedLiteral>(p, p));
                }
                break;
            }
        } else {
            break;
        }

    } while (!at_end() && !match(TokenType::RIGHT_BRACKET));

    if (!consume(TokenType::RIGHT_BRACKET)) {
        add_error("Expected ']' to close array literal");
        no_in_mode_ = saved_no_in_al;
        return nullptr;
    }

    no_in_mode_ = saved_no_in_al;
    Position end = get_current_position();
    return std::make_unique<ArrayLiteral>(std::move(elements), start, end);
}


std::unique_ptr<ASTNode> Parser::parse_try_statement() {
    Position start = current_token().get_start();
    advance();
    
    auto try_block = parse_block_statement();
    if (!try_block) {
        add_error("Expected block statement after 'try'");
        return nullptr;
    }
    
    std::unique_ptr<ASTNode> catch_clause = nullptr;
    std::unique_ptr<ASTNode> finally_block = nullptr;
    
    if (match(TokenType::CATCH)) {
        catch_clause = parse_catch_clause();
        if (!catch_clause) {
            add_error("Invalid catch clause");
            return nullptr;
        }
    }
    
    if (match(TokenType::FINALLY)) {
        advance();
        finally_block = parse_block_statement();
        if (!finally_block) {
            add_error("Expected block statement after 'finally'");
            return nullptr;
        }
    }
    
    if (!catch_clause && !finally_block) {
        add_error("Missing catch or finally after try");
        return nullptr;
    }
    
    Position end = get_current_position();
    return std::make_unique<TryStatement>(
        std::move(try_block),
        std::move(catch_clause),
        std::move(finally_block),
        start, end
    );
}

std::unique_ptr<ASTNode> Parser::parse_catch_clause() {
    Position start = current_token().get_start();
    advance();

    std::string parameter_name = "";

    // Optional catch binding: catch can have no parameter (ES2019+)
    std::unique_ptr<ASTNode> catch_destr_pattern;

    if (match(TokenType::LEFT_PAREN)) {
        advance(); // consume '('

        if (match(TokenType::LEFT_BRACKET) || match(TokenType::LEFT_BRACE)) {
            // Destructuring in catch: use full pattern parser
            catch_destr_pattern = parse_destructuring_pattern();  // assigned to outer scope var
            if (!catch_destr_pattern) {
                add_error("Invalid destructuring pattern in catch clause");
                return nullptr;
            }
            // Check for duplicate bound names in destructuring catch parameter
            if (catch_destr_pattern->get_type() == ASTNode::Type::DESTRUCTURING_ASSIGNMENT) {
                auto* da = static_cast<DestructuringAssignment*>(catch_destr_pattern.get());
                std::unordered_set<std::string> seen_names;
                for (const auto& t : da->get_targets()) {
                    if (!t) continue;
                    const std::string& nm = t->get_name();
                    if (nm.empty() || nm[0] == '_') continue;
                    if (!seen_names.insert(nm).second) {
                        add_error("SyntaxError: Duplicate binding '" + nm + "' in catch parameter");
                        return nullptr;
                    }
                }
            }
            parameter_name = "__destr_pattern__";
        } else if (match(TokenType::IDENTIFIER) ||
                   (match(TokenType::AWAIT) && !options_.in_async_body && !options_.source_type_module && !options_.in_class_static_block) ||
                   (match(TokenType::YIELD) && !options_.in_generator_body && !options_.strict_mode)) {
            parameter_name = current_token().get_value();

            // ES5: eval and arguments cannot be used as catch parameter in strict mode
            if (options_.strict_mode && (parameter_name == "eval" || parameter_name == "arguments")) {
                add_error("'" + parameter_name + "' cannot be used as a catch parameter in strict mode");
                return nullptr;
            }

            advance();
        } else {
            add_error("Expected identifier or destructuring pattern in catch clause");
            return nullptr;
        }

        if (!consume(TokenType::RIGHT_PAREN)) {
            add_error("Expected ')' after catch parameter");
            return nullptr;
        }
    }

    auto body = parse_block_statement();
    if (!body) {
        add_error("Expected block statement in catch clause");
        return nullptr;
    }

    // Check catch param names don't appear in LexicallyDeclaredNames of catch body
    if (!parameter_name.empty() && parameter_name != "__destr_pattern__" &&
        body->get_type() == ASTNode::Type::BLOCK_STATEMENT) {
        auto* blk = static_cast<BlockStatement*>(body.get());
        for (const auto& stmt : blk->get_statements()) {
            if (!stmt) continue;
            auto st = stmt->get_type();
            if (st == ASTNode::Type::VARIABLE_DECLARATION) {
                auto* vd = static_cast<VariableDeclaration*>(stmt.get());
                if (vd->get_kind() != VariableDeclarator::Kind::VAR) {
                    for (const auto& d : vd->get_declarations()) {
                        if (d->get_id() && d->get_id()->get_name() == parameter_name) {
                            add_error("SyntaxError: Identifier '" + parameter_name + "' is already declared in catch");
                            return nullptr;
                        }
                    }
                }
            } else if (st == ASTNode::Type::FUNCTION_DECLARATION) {
                auto* fn = static_cast<FunctionDeclaration*>(stmt.get());
                if (fn->get_id() && fn->get_id()->get_name() == parameter_name) {
                    add_error("SyntaxError: Identifier '" + parameter_name + "' is already declared in catch");
                    return nullptr;
                }
            }
        }
    }

    Position end = get_current_position();
    auto catch_node = std::make_unique<CatchClause>(parameter_name, std::move(body), start, end);
    if (catch_destr_pattern) {
        catch_node->set_destructuring_pattern(std::move(catch_destr_pattern));
    }
    return catch_node;
}

std::unique_ptr<ASTNode> Parser::parse_throw_statement() {
    Position start = current_token().get_start();
    int throw_line = current_token().get_start().line;
    advance();

    if (current_token().get_start().line > throw_line) {
        add_error("SyntaxError: Illegal newline after throw");
        return nullptr;
    }

    auto expression = parse_expression();
    if (!expression) {
        add_error("Expected expression after 'throw'");
        return nullptr;
    }
    
    consume_if_match(TokenType::SEMICOLON);
    
    Position end = get_current_position();
    return std::make_unique<ThrowStatement>(std::move(expression), start, end);
}

std::unique_ptr<ASTNode> Parser::parse_switch_statement() {
    Position start = current_token().get_start();
    advance();
    
    if (!consume(TokenType::LEFT_PAREN)) {
        add_error("Expected '(' after 'switch'");
        return nullptr;
    }
    
    auto discriminant = parse_expression();
    if (!discriminant) {
        add_error("Expected expression in switch statement");
        return nullptr;
    }
    
    if (!consume(TokenType::RIGHT_PAREN)) {
        add_error("Expected ')' after switch expression");
        return nullptr;
    }
    
    if (!consume(TokenType::LEFT_BRACE)) {
        add_error("Expected '{' after switch expression");
        return nullptr;
    }

    options_.switch_depth++;
    std::vector<std::unique_ptr<ASTNode>> cases;
    bool seen_default = false;

    while (!match(TokenType::RIGHT_BRACE) && !at_end()) {
        if (match(TokenType::CASE)) {
            Position case_start = current_token().get_start();
            advance();
            
            auto test = parse_expression();
            if (!test) {
                add_error("Expected expression after 'case'");
                return nullptr;
            }
            
            if (!consume(TokenType::COLON)) {
                add_error("Expected ':' after case expression");
                return nullptr;
            }
            
            std::vector<std::unique_ptr<ASTNode>> consequent;
            options_.in_switch_case_list = true;
            while (!match(TokenType::CASE) && !match(TokenType::DEFAULT) &&
                   !match(TokenType::RIGHT_BRACE) && !at_end()) {
                auto stmt = parse_statement();
                if (stmt) {
                    consequent.push_back(std::move(stmt));
                } else {
                    break;
                }
            }
            options_.in_switch_case_list = false;

            Position case_end = get_current_position();
            cases.push_back(std::make_unique<CaseClause>(
                std::move(test), std::move(consequent), case_start, case_end
            ));

        } else if (match(TokenType::DEFAULT)) {
            if (seen_default) {
                add_error("SyntaxError: Duplicate 'default' clause in switch statement");
                return nullptr;
            }
            seen_default = true;
            Position default_start = current_token().get_start();
            advance();

            if (!consume(TokenType::COLON)) {
                add_error("Expected ':' after 'default'");
                return nullptr;
            }

            std::vector<std::unique_ptr<ASTNode>> consequent;
            options_.in_switch_case_list = true;
            while (!match(TokenType::CASE) && !match(TokenType::DEFAULT) &&
                   !match(TokenType::RIGHT_BRACE) && !at_end()) {
                auto stmt = parse_statement();
                if (stmt) {
                    consequent.push_back(std::move(stmt));
                } else {
                    break;
                }
            }
            options_.in_switch_case_list = false;
            
            Position default_end = get_current_position();
            cases.push_back(std::make_unique<CaseClause>(
                nullptr, std::move(consequent), default_start, default_end
            ));
            
        } else {
            add_error("Expected 'case' or 'default' in switch body");
            skip_to(TokenType::RIGHT_BRACE);
            break;
        }
    }
    
    if (!consume(TokenType::RIGHT_BRACE)) {
        add_error("Expected '}' to close switch statement");
        return nullptr;
    }

    // SwitchStatement early error: LexicallyDeclaredNames across all cases must be unique
    {
        std::vector<std::unique_ptr<ASTNode>> all_stmts;
        for (const auto& c : cases) {
            auto* cc = static_cast<CaseClause*>(c.get());
            for (const auto& s : cc->get_consequent())
                all_stmts.push_back(s ? s->clone() : nullptr);
        }
        std::string dup = find_block_lexical_duplicate(all_stmts);
        if (!dup.empty()) {
            add_error("Identifier '" + dup + "' has already been declared");
            return nullptr;
        }
    }

    options_.switch_depth--;
    Position end = get_current_position();
    return std::make_unique<SwitchStatement>(
        std::move(discriminant), std::move(cases), start, end
    );
}


std::unique_ptr<ASTNode> Parser::parse_import_statement() {
    Position start = current_token().get_start();
    // import declarations are only valid at module top level
    if (options_.function_depth > 0 || options_.loop_depth > 0) {
        add_error("SyntaxError: Import declarations may not appear in function or loop bodies");
        advance();
        return nullptr;
    }
    advance();

    // Parse optional 'with { ... }' import attributes clause; validate no duplicate keys
    auto skip_import_with = [&]() {
        if (!match(TokenType::WITH)) return;
        advance();
        if (!match(TokenType::LEFT_BRACE)) return;
        advance();
        std::unordered_set<std::string> seen_keys;
        while (!at_end() && !match(TokenType::RIGHT_BRACE)) {
            // key is identifier or string literal
            std::string key;
            if (match(TokenType::IDENTIFIER) || is_reserved_word_as_property_name()) {
                key = current_token().get_value();
                advance();
            } else if (match(TokenType::STRING)) {
                key = current_token().get_value();
                advance();
            } else {
                advance();
                continue;
            }
            if (!seen_keys.insert(key).second) {
                add_error("SyntaxError: Duplicate import attribute key '" + key + "'");
                return;
            }
            // skip ':' and value
            if (match(TokenType::COLON)) {
                advance();
                if (!at_end() && !match(TokenType::RIGHT_BRACE) && !match(TokenType::COMMA))
                    advance();
            }
            if (match(TokenType::COMMA)) advance();
        }
        if (match(TokenType::RIGHT_BRACE)) advance();
    };

    // Side-effect import: import "module"
    if (match(TokenType::STRING)) {
        std::string module_source = current_token().get_value();
        advance();
        skip_import_with();
        Position end = get_current_position();
        std::vector<std::unique_ptr<ImportSpecifier>> specifiers;
        return std::make_unique<ImportStatement>(std::move(specifiers), module_source, start, end);
    }

    if (match(TokenType::MULTIPLY)) {
        advance();
        
        if (current_token().get_type() != TokenType::IDENTIFIER || current_token().get_value() != "as") {
            add_error("Expected 'as' after '*' in import statement");
            return nullptr;
        }
        if (current_token().has_escaped_keyword()) {
            add_error("SyntaxError: 'as' cannot use unicode escape sequences in import statement");
            return nullptr;
        }
        advance();
        
        if (current_token().get_type() != TokenType::IDENTIFIER) {
            add_error("Expected identifier after 'as'");
            return nullptr;
        }
        std::string namespace_alias = current_token().get_value();
        advance();
        
        if (current_token().get_type() != TokenType::FROM) {
            add_error("Expected 'from' in import statement");
            return nullptr;
        }
        advance();
        
        if (current_token().get_type() != TokenType::STRING) {
            add_error("Expected string literal after 'from'");
            return nullptr;
        }
        std::string module_source = current_token().get_value();
        advance(); skip_import_with();
        Position end = get_current_position();
        return std::make_unique<ImportStatement>(namespace_alias, module_source, start, end);
    }

    if (match(TokenType::LEFT_BRACE)) {
        advance();
        
        std::vector<std::unique_ptr<ImportSpecifier>> specifiers;
        
        while (!match(TokenType::RIGHT_BRACE) && !at_end()) {
            auto specifier = parse_import_specifier();
            if (specifier) {
                specifiers.push_back(std::move(specifier));
            }
            
            if (match(TokenType::COMMA)) {
                advance();
            } else if (!match(TokenType::RIGHT_BRACE)) {
                add_error("Expected ',' or '}' in import specifiers");
                break;
            }
        }
        
        if (!match(TokenType::RIGHT_BRACE)) {
            add_error("Expected '}' after import specifiers");
            return nullptr;
        }
        advance();

        // Check for duplicate local binding names in import specifiers
        for (size_t si = 0; si < specifiers.size(); si++)
            for (size_t sj = si + 1; sj < specifiers.size(); sj++)
                if (specifiers[si]->get_local_name() == specifiers[sj]->get_local_name()) {
                    add_error("SyntaxError: Duplicate import binding '" + specifiers[si]->get_local_name() + "'");
                    return nullptr;
                }

        if (current_token().get_type() != TokenType::FROM) {
            add_error("Expected 'from' in import statement");
            return nullptr;
        }
        advance();

        if (current_token().get_type() != TokenType::STRING) {
            add_error("Expected string literal after 'from'");
            return nullptr;
        }
        std::string module_source = current_token().get_value();
        advance(); skip_import_with();
        Position end = get_current_position();
        return std::make_unique<ImportStatement>(std::move(specifiers), module_source, start, end);
    }

    if (match(TokenType::IDENTIFIER)) {
        // import defer * as ns from "module"
        if (current_token().get_value() == "defer") {
            size_t saved_idx = current_token_index_;
            advance(); // try consuming 'defer'
            if (match(TokenType::MULTIPLY)) {
                advance();
                if (current_token().get_type() != TokenType::IDENTIFIER || current_token().get_value() != "as") {
                    add_error("Expected 'as' after '*' in import defer statement");
                    return nullptr;
                }
                advance();
                if (current_token().get_type() != TokenType::IDENTIFIER) {
                    add_error("Expected identifier after 'as' in import defer statement");
                    return nullptr;
                }
                std::string namespace_alias = current_token().get_value();
                advance();
                if (current_token().get_type() != TokenType::FROM) {
                    add_error("Expected 'from' in import defer statement");
                    return nullptr;
                }
                advance();
                if (current_token().get_type() != TokenType::STRING) {
                    add_error("Expected string literal after 'from' in import defer statement");
                    return nullptr;
                }
                std::string module_source = current_token().get_value();
                advance(); skip_import_with();
                Position end = get_current_position();
                return std::make_unique<ImportStatement>(namespace_alias, module_source, start, end, true);
            }
            // Not import defer *, treat 'defer' as a default import alias
            current_token_index_ = saved_idx;
        }

        std::string default_alias = current_token().get_value();
        advance();

        if (match(TokenType::COMMA)) {
            advance();

            // ImportedDefaultBinding , NameSpaceImport
            if (match(TokenType::MULTIPLY)) {
                advance();
                if (current_token().get_type() != TokenType::IDENTIFIER || current_token().get_value() != "as") {
                    add_error("Expected 'as' after '*' in namespace import");
                    return nullptr;
                }
                advance();
                if (current_token().get_type() != TokenType::IDENTIFIER) {
                    add_error("Expected identifier after '* as' in namespace import");
                    return nullptr;
                }
                std::string namespace_alias = current_token().get_value();
                advance();
                if (current_token().get_type() != TokenType::FROM) {
                    add_error("Expected 'from' after namespace import");
                    return nullptr;
                }
                advance();
                if (current_token().get_type() != TokenType::STRING) {
                    add_error("Expected string literal after 'from'");
                    return nullptr;
                }
                std::string module_source = current_token().get_value();
                advance(); skip_import_with();
                Position end = get_current_position();
                // ImportStatement with both default alias and namespace alias
                return std::make_unique<ImportStatement>(default_alias, namespace_alias, module_source, start, end);
            }

            // ImportedDefaultBinding , NamedImports
            if (!match(TokenType::LEFT_BRACE)) {
                add_error("Expected '{' or '*' after ',' in mixed import statement");
                return nullptr;
            }
            advance();

            std::vector<std::unique_ptr<ImportSpecifier>> specifiers;
            while (!match(TokenType::RIGHT_BRACE) && !at_end()) {
                auto specifier = parse_import_specifier();
                if (specifier) {
                    specifiers.push_back(std::move(specifier));
                }

                if (match(TokenType::COMMA)) {
                    advance();
                } else if (!match(TokenType::RIGHT_BRACE)) {
                    add_error("Expected ',' or '}' in import specifiers");
                    break;
                }
            }

            if (!match(TokenType::RIGHT_BRACE)) {
                add_error("Expected '}' after import specifiers");
                return nullptr;
            }
            advance();

            if (current_token().get_type() != TokenType::FROM) {
                add_error("Expected 'from' in mixed import statement");
                return nullptr;
            }
            advance();

            if (current_token().get_type() != TokenType::STRING) {
                add_error("Expected string literal after 'from'");
                return nullptr;
            }
            std::string module_source = current_token().get_value();
            advance(); skip_import_with();
            Position end = get_current_position();
            return std::make_unique<ImportStatement>(default_alias, std::move(specifiers), module_source, start, end);
        }

        if (current_token().get_type() != TokenType::FROM) {
            add_error("Expected 'from' in import statement");
            return nullptr;
        }
        advance();

        if (current_token().get_type() != TokenType::STRING) {
            add_error("Expected string literal after 'from'");
            return nullptr;
        }
        std::string module_source = current_token().get_value();
        advance(); skip_import_with();
        Position end = get_current_position();
        return std::make_unique<ImportStatement>(default_alias, module_source, true, start, end);
    }
    
    add_error("Invalid import statement syntax");
    return nullptr;
}

std::unique_ptr<ASTNode> Parser::parse_export_statement() {
    Position start = current_token().get_start();

    if (options_.function_depth > 0) {
        add_error("SyntaxError: 'export' not allowed inside function body");
        advance();
        return nullptr;
    }

    auto skip_export_with = [&]() {
        if (!match(TokenType::WITH)) return;
        advance();
        if (!match(TokenType::LEFT_BRACE)) return;
        advance();
        std::unordered_set<std::string> seen_keys;
        while (!at_end() && !match(TokenType::RIGHT_BRACE)) {
            std::string key;
            if (match(TokenType::IDENTIFIER) || is_reserved_word_as_property_name()) {
                key = current_token().get_value(); advance();
            } else if (match(TokenType::STRING)) {
                key = current_token().get_value(); advance();
            } else { advance(); continue; }
            if (!seen_keys.insert(key).second) {
                add_error("SyntaxError: Duplicate import attribute key '" + key + "'");
                return;
            }
            if (match(TokenType::COLON)) {
                advance();
                if (!at_end() && !match(TokenType::RIGHT_BRACE) && !match(TokenType::COMMA)) advance();
            }
            if (match(TokenType::COMMA)) advance();
        }
        if (match(TokenType::RIGHT_BRACE)) advance();
    };

    advance();
    
    if (match(TokenType::DEFAULT)) {
        advance();

        // export default function/function*/async function/class -> HoistableDeclaration/ClassDeclaration.
        // Per spec: only AssignmentExpression is allowed when lookahead is NOT {function, async [no LT] function, class}.
        // After a decl body, any `(` is a SyntaxError (can't invoke a declaration).
        bool is_decl_form = match(TokenType::FUNCTION) || match(TokenType::CLASS);
        if (!is_decl_form && match(TokenType::IDENTIFIER) && current_token().get_value() == "async") {
            size_t saved_idx = current_token_index_;
            size_t async_line = current_token().get_end().line;
            advance();
            if (match(TokenType::FUNCTION) && current_token().get_start().line == async_line) {
                is_decl_form = true;
            }
            current_token_index_ = saved_idx;
        }

        std::unique_ptr<ASTNode> default_export;
        if (is_decl_form) {
            // Parse only the declaration node (not call expressions after it)
            if (match(TokenType::CLASS)) {
                default_export = parse_class_expression();
            } else {
                default_export = parse_function_expression();
            }
            if (!default_export) {
                add_error("Expected declaration after 'export default'");
                return nullptr;
            }
            // `export default function fn(){}` is a HoistableDeclaration, not a NamedEvaluation expression --
            // fn's own-name binding must stay the normal mutable module-scope one.
            if (default_export->get_type() == ASTNode::Type::FUNCTION_EXPRESSION) {
                static_cast<FunctionExpression*>(default_export.get())->set_decl_form(true);
            } else if (default_export->get_type() == ASTNode::Type::ASYNC_FUNCTION_EXPRESSION) {
                static_cast<AsyncFunctionExpression*>(default_export.get())->set_decl_form(true);
            }
            if (match(TokenType::LEFT_PAREN)) {
                add_error("SyntaxError: Anonymous function declaration cannot be immediately invoked");
                return nullptr;
            }
        } else {
            default_export = parse_assignment_expression();
            if (!default_export) {
                add_error("Expected expression after 'export default'");
                return nullptr;
            }
        }

        // Consume optional semicolon (ASI applies here)
        if (!consume_if_match(TokenType::SEMICOLON)) {
            TokenType cur = current_token().get_type();
            if (cur != TokenType::RIGHT_BRACE && cur != TokenType::EOF_TOKEN) {
                bool has_nl = false;
                bool prev_is_brace = false;
                size_t i = current_token_index_;
                while (i > 0) {
                    i--;
                    TokenType t = tokens_[i].get_type();
                    if (t == TokenType::NEWLINE) { has_nl = true; break; }
                    if (t != TokenType::WHITESPACE && t != TokenType::COMMENT) {
                        prev_is_brace = (t == TokenType::RIGHT_BRACE);
                        break;
                    }
                }
                // Class/function declarations end with '}' -- no ASI needed
                if (!has_nl && !prev_is_brace) {
                    add_error("SyntaxError: Unexpected token after export default expression");
                    return nullptr;
                }
            }
        }

        Position end = get_current_position();
        return std::make_unique<ExportStatement>(std::move(default_export), true, start, end);
    }
    
    // export * from 'module'  OR  export * as name from 'module'
    if (match(TokenType::MULTIPLY)) {
        advance();
        std::string exported_name = "*";
        if (match(TokenType::IDENTIFIER) && current_token().get_value() == "as") {
            advance();
            // ES2022: export name can be a StringLiteral or IdentifierName
            bool star_as_is_string = match(TokenType::STRING);
            if (!star_as_is_string && !match(TokenType::IDENTIFIER) && !is_reserved_word_as_property_name()) {
                add_error("Expected identifier after 'as' in export * as");
                return nullptr;
            }
            exported_name = current_token().get_value();
            if (star_as_is_string) {
                for (size_t i = 0; i + 2 < exported_name.size(); i++) {
                    unsigned char b0 = exported_name[i], b1 = exported_name[i+1], b2 = exported_name[i+2];
                    if (b0 == 0xED && b1 >= 0xA0 && b1 <= 0xBF && b2 >= 0x80 && b2 <= 0xBF) {
                        add_error("SyntaxError: Module export name contains lone surrogate");
                        return nullptr;
                    }
                }
            }
            advance();
        }
        if (current_token().get_type() != TokenType::FROM) {
            add_error("Expected 'from' in export * statement");
            return nullptr;
        }
        advance();
        if (!match(TokenType::STRING)) {
            add_error("Expected string literal after 'from' in export *");
            return nullptr;
        }
        std::string source_module = current_token().get_value();
        advance(); skip_export_with();
        if (!consume_if_match(TokenType::SEMICOLON)) {
            TokenType cur = current_token().get_type();
            if (cur != TokenType::RIGHT_BRACE && cur != TokenType::EOF_TOKEN) {
                bool has_nl = false;
                size_t i = current_token_index_;
                while (i > 0) {
                    i--;
                    TokenType t = tokens_[i].get_type();
                    if (t == TokenType::NEWLINE) { has_nl = true; break; }
                    if (t != TokenType::WHITESPACE && t != TokenType::COMMENT) break;
                }
                if (!has_nl) {
                    add_error("SyntaxError: Unexpected token after export statement");
                    return nullptr;
                }
            }
        }
        Position end = get_current_position();
        std::vector<std::unique_ptr<ExportSpecifier>> specifiers;
        specifiers.push_back(std::make_unique<ExportSpecifier>("*", exported_name, start, end));
        return std::make_unique<ExportStatement>(std::move(specifiers), source_module, start, end);
    }

    if (match(TokenType::LEFT_BRACE)) {
        advance();

        std::vector<std::unique_ptr<ExportSpecifier>> specifiers;
        std::vector<bool> spec_local_is_string;

        while (!match(TokenType::RIGHT_BRACE) && !at_end()) {
            bool local_is_str = match(TokenType::STRING);
            auto specifier = parse_export_specifier();
            if (specifier) {
                specifiers.push_back(std::move(specifier));
                spec_local_is_string.push_back(local_is_str);
            }

            if (match(TokenType::COMMA)) {
                advance();
            } else if (!match(TokenType::RIGHT_BRACE)) {
                add_error("Expected ',' or '}' in export specifiers");
                break;
            }
        }

        if (!match(TokenType::RIGHT_BRACE)) {
            add_error("Expected '}' after export specifiers");
            return nullptr;
        }
        advance();

        if (match(TokenType::FROM)) {
            advance();
            
            if (!match(TokenType::STRING)) {
                add_error("Expected string literal after 'from'");
                return nullptr;
            }
            std::string source_module = current_token().get_value();
            advance(); skip_export_with();
            if (!consume_if_match(TokenType::SEMICOLON)) {
                TokenType cur = current_token().get_type();
                if (cur != TokenType::RIGHT_BRACE && cur != TokenType::EOF_TOKEN) {
                    bool has_nl = false;
                    size_t i = current_token_index_;
                    while (i > 0) {
                        i--;
                        TokenType t = tokens_[i].get_type();
                        if (t == TokenType::NEWLINE) { has_nl = true; break; }
                        if (t != TokenType::WHITESPACE && t != TokenType::COMMENT) break;
                    }
                    if (!has_nl) {
                        add_error("SyntaxError: Unexpected token after export statement");
                        return nullptr;
                    }
                }
            }

            Position end = get_current_position();
            return std::make_unique<ExportStatement>(std::move(specifiers), source_module, start, end);
        }
        
        // export NamedExports without 'from': string local names are not allowed
        for (size_t i = 0; i < spec_local_is_string.size(); i++) {
            if (spec_local_is_string[i]) {
                add_error("SyntaxError: String literal cannot be used as local binding name in export specifier without 'from'");
                return nullptr;
            }
        }

        // export NamedExports; -- must be followed by semicolon or line terminator
        if (!consume_if_match(TokenType::SEMICOLON)) {
            TokenType cur = current_token().get_type();
            if (cur != TokenType::EOF_TOKEN) {
                bool has_nl = false;
                size_t i = current_token_index_;
                while (i > 0) {
                    i--;
                    TokenType t = tokens_[i].get_type();
                    if (t == TokenType::NEWLINE) { has_nl = true; break; }
                    if (t != TokenType::WHITESPACE && t != TokenType::COMMENT) break;
                }
                if (!has_nl) {
                    add_error("SyntaxError: Unexpected token after export specifier list");
                    return nullptr;
                }
            }
        }

        Position end = get_current_position();
        return std::make_unique<ExportStatement>(std::move(specifiers), start, end);
    }

    auto declaration = parse_statement();
    if (!declaration) {
        add_error("Expected declaration after 'export'");
        return nullptr;
    }
    
    Position end = get_current_position();
    return std::make_unique<ExportStatement>(std::move(declaration), start, end);
}

std::unique_ptr<ImportSpecifier> Parser::parse_import_specifier() {
    Position start = current_token().get_start();

    // ES2022: ModuleExportName may be a StringLiteral: import { "name" as local }
    bool imported_is_string = match(TokenType::STRING);
    if (!imported_is_string && !match(TokenType::IDENTIFIER) && !match(TokenType::DEFAULT) &&
        !is_keyword_token(current_token().get_type())) {
        add_error("Expected identifier or 'default' in import specifier");
        return nullptr;
    }

    std::string imported_name = current_token().get_value();
    std::string local_name = imported_name;
    advance();

    // String import names: check for lone surrogates (IsStringWellFormedUnicode)
    if (imported_is_string) {
        for (size_t i = 0; i + 2 < imported_name.size(); i++) {
            unsigned char b0 = imported_name[i], b1 = imported_name[i+1], b2 = imported_name[i+2];
            if (b0 == 0xED && b1 >= 0xA0 && b1 <= 0xBF && b2 >= 0x80 && b2 <= 0xBF) {
                add_error("SyntaxError: Module import name contains lone surrogate");
                return nullptr;
            }
        }
    }

    // String import names MUST have "as localName"
    if (imported_is_string && !(match(TokenType::IDENTIFIER) && current_token().get_value() == "as")) {
        add_error("SyntaxError: String import name requires 'as' binding");
        return nullptr;
    }

    if (match(TokenType::IDENTIFIER) && current_token().get_value() == "as") {
        if (current_token().has_escaped_keyword()) {
            add_error("SyntaxError: 'as' cannot use unicode escape sequences in import specifier");
            return nullptr;
        }
        advance();

        if (current_token().get_type() != TokenType::IDENTIFIER) {
            add_error("Expected identifier after 'as'");
            return nullptr;
        }
        local_name = current_token().get_value();
        advance();
    }

    // In module (strict) code, eval/arguments cannot be local import bindings
    if (options_.source_type_module && (local_name == "eval" || local_name == "arguments")) {
        add_error("SyntaxError: '" + local_name + "' cannot be used as an import binding in module code");
        return nullptr;
    }

    Position end = get_current_position();
    return std::make_unique<ImportSpecifier>(imported_name, local_name, start, end);
}

std::unique_ptr<ExportSpecifier> Parser::parse_export_specifier() {
    Position start = current_token().get_start();

    // Helper: ES2022 ModuleExportName strings must be well-formed Unicode (no lone surrogates)
    auto check_module_export_name = [this](const std::string& s) -> bool {
        // Lone surrogates are encoded as 0xED 0xA0-0xBF 0x80-0xBF in our CESU-8 storage
        for (size_t i = 0; i + 2 < s.size(); i++) {
            unsigned char b0 = s[i], b1 = s[i+1], b2 = s[i+2];
            if (b0 == 0xED && b1 >= 0xA0 && b1 <= 0xBF && b2 >= 0x80 && b2 <= 0xBF) {
                add_error("SyntaxError: Module export name contains lone surrogate");
                return false;
            }
        }
        return true;
    };

    // ES2022: ModuleExportName can be a StringLiteral or IdentifierName
    bool local_is_string = match(TokenType::STRING);
    if (!local_is_string && !match(TokenType::IDENTIFIER) && !is_keyword_token(current_token().get_type())) {
        add_error("Expected identifier in export specifier");
        return nullptr;
    }

    std::string local_name = current_token().get_value();
    if (local_is_string && !check_module_export_name(local_name)) return nullptr;
    std::string exported_name = local_name;
    advance();

    if (match(TokenType::IDENTIFIER) && current_token().get_value() == "as") {
        if (current_token().has_escaped_keyword()) {
            add_error("SyntaxError: 'as' cannot use unicode escape sequences in export specifier");
            return nullptr;
        }
        advance();

        // exported name can also be a string literal
        bool exp_is_string = match(TokenType::STRING);
        if (!exp_is_string && !match(TokenType::IDENTIFIER) && !is_keyword_token(current_token().get_type())) {
            add_error("Expected identifier after 'as' in export specifier");
            return nullptr;
        }
        exported_name = current_token().get_value();
        if (exp_is_string && !check_module_export_name(exported_name)) return nullptr;
        advance();
    }

    Position end = get_current_position();
    return std::make_unique<ExportSpecifier>(local_name, exported_name, start, end);
}

std::unique_ptr<ASTNode> Parser::parse_destructuring_pattern(int depth) {

    Position start = get_current_position();
    
    if (current_token().get_type() == TokenType::LEFT_BRACKET) {
        advance();
        
        std::vector<std::unique_ptr<Identifier>> targets;
        std::vector<std::pair<size_t, std::unique_ptr<ASTNode>>> default_exprs;
        std::unique_ptr<ASTNode> nested_rest_pat;

        while (!match(TokenType::RIGHT_BRACKET) && !at_end()) {
            if (current_token().get_type() == TokenType::ELLIPSIS) {
                advance();

                if (current_token().get_type() == TokenType::LEFT_BRACKET ||
                    current_token().get_type() == TokenType::LEFT_BRACE) {
                    // ...[pattern] or ...{pattern}: nested rest destructuring
                    auto nested_pattern = parse_destructuring_pattern(depth + 1);
                    if (!nested_pattern) {
                        add_error("Invalid nested pattern after '...' in array destructuring");
                        return nullptr;
                    }
                    auto rest_id = std::make_unique<Identifier>(
                        "...__nested_rest__",
                        nested_pattern->get_start(),
                        nested_pattern->get_end()
                    );
                    targets.push_back(std::move(rest_id));
                    nested_rest_pat = std::move(nested_pattern);
                } else if (current_token().get_type() == TokenType::IDENTIFIER) {
                    auto rest_id = std::make_unique<Identifier>(
                        "..." + current_token().get_value(),
                        current_token().get_start(),
                        current_token().get_end()
                    );
                    targets.push_back(std::move(rest_id));
                    advance();
                } else {
                    add_error("Expected identifier after '...' in array destructuring");
                    return nullptr;
                }

                if (match(TokenType::COMMA)) {
                    add_error("Rest element must be last element in array destructuring");
                    return nullptr;
                }
                break;
                
            } else if (current_token().get_type() == TokenType::IDENTIFIER ||
                       (current_token().get_type() == TokenType::AWAIT && !options_.in_async_body && !options_.source_type_module && !options_.in_class_static_block) ||
                       (current_token().get_type() == TokenType::YIELD && !options_.in_generator_body && !options_.strict_mode)) {
                auto id = std::make_unique<Identifier>(
                    current_token().get_value(),
                    current_token().get_start(),
                    current_token().get_end()
                );
                targets.push_back(std::move(id));
                advance();

                if (match(TokenType::ASSIGN)) {
                    advance();
                    auto default_expr = parse_assignment_expression();
                    if (!default_expr) {
                        add_error("Expected expression after '=' in array destructuring");
                        return nullptr;
                    }
                    size_t target_index = targets.size() - 1;
                    default_exprs.emplace_back(target_index, std::move(default_expr));
                }
            } else if (current_token().get_type() == TokenType::LEFT_BRACKET) {
                // Nested array destructuring: recurse
                auto nested = parse_destructuring_pattern(depth + 1);
                if (!nested) {
                    add_error("Invalid nested array destructuring");
                    return nullptr;
                }
                // Encode all leaf variable names from the nested pattern
                std::function<std::string(ASTNode*)> extract_vars = [&](ASTNode* nd) -> std::string {
                    if (!nd) return "";
                    auto* da = dynamic_cast<DestructuringAssignment*>(nd);
                    if (!da) return "";
                    std::string result;
                    bool first = true;
                    for (const auto& t : da->get_targets()) {
                        if (!first) result += ",";
                        const std::string& nm = t->get_name();
                        if (nm.empty()) {
                            result += "\x01"; // elision sentinel -- runtime advances iterator without binding
                        } else {
                            result += nm;
                        }
                        first = false;
                    }
                    return result;
                };
                std::string nested_vars = extract_vars(nested.get());
                Position np_start = nested->get_start();
                Position np_end = nested->get_end();
                auto nested_placeholder = std::make_unique<Identifier>(
                    "__nested_vars:" + nested_vars,
                    np_start, np_end
                );
                targets.push_back(std::move(nested_placeholder));

                if (match(TokenType::ASSIGN)) {
                    advance();
                    auto default_expr = parse_assignment_expression();
                    if (!default_expr) {
                        add_error("Expected expression after '=' in nested array destructuring");
                        return nullptr;
                    }
                    size_t target_index = targets.size() - 1;
                    default_exprs.emplace_back(target_index, std::move(default_expr));
                }
            } else if (current_token().get_type() == TokenType::LEFT_BRACE) {
                // Nested object destructuring inside array: [a, {x:b, c}]
                auto nested = parse_destructuring_pattern(depth + 1);
                if (!nested) {
                    add_error("Invalid nested object destructuring in array");
                    return nullptr;
                }
                // Encode as __nested_obj:prop1>var1,prop2>var2
                std::string encoding = "__nested_obj:";
                if (auto* nd = dynamic_cast<DestructuringAssignment*>(nested.get())) {
                    const auto& nt = nd->get_targets();
                    const auto& nm = nd->get_property_mappings();
                    bool first = true;
                    // Add shorthand properties (those without explicit mappings)
                    for (const auto& t : nt) {
                        const std::string& name = t->get_name();
                        bool in_mappings = false;
                        for (const auto& m : nm) {
                            // Check if this target is the variable_name of a mapping
                            if (m.variable_name == name) { in_mappings = true; break; }
                        }
                        if (!in_mappings &&
                            name.find("__nested") == std::string::npos) {
                            if (!first) encoding += ",";
                            encoding += name + ">" + name;
                            first = false;
                        }
                    }
                    // Add explicitly mapped properties
                    for (const auto& m : nm) {
                        if (!first) encoding += ",";
                        encoding += m.property_name + ">" + m.variable_name;
                        first = false;
                    }
                }
                auto nested_id = std::make_unique<Identifier>(
                    encoding, nested->get_start(), nested->get_end()
                );
                targets.push_back(std::move(nested_id));

                if (match(TokenType::ASSIGN)) {
                    advance();
                    auto default_expr = parse_assignment_expression();
                    if (!default_expr) {
                        add_error("Expected expression after '=' in nested object destructuring");
                        return nullptr;
                    }
                    size_t target_index = targets.size() - 1;
                    default_exprs.emplace_back(target_index, std::move(default_expr));
                }
            } else if (current_token().get_type() == TokenType::COMMA) {
                auto placeholder = std::make_unique<Identifier>(
                    "",
                    current_token().get_start(),
                    current_token().get_end()
                );
                targets.push_back(std::move(placeholder));
            } else {
                add_error("Expected identifier or ',' in array destructuring");
                return nullptr;
            }
            
            if (match(TokenType::COMMA)) {
                advance();
            } else {
                break;
            }
        }
        
        if (!consume(TokenType::RIGHT_BRACKET)) {
            add_error("Expected ']' to close array destructuring");
            return nullptr;
        }
        
        Position end = get_current_position();
        auto destructuring = std::make_unique<DestructuringAssignment>(
            std::move(targets), nullptr, DestructuringAssignment::Type::ARRAY, start, end
        );

        for (auto& default_pair : default_exprs) {
            destructuring->add_default_value(default_pair.first, std::move(default_pair.second));
        }

        if (nested_rest_pat) {
            destructuring->set_nested_rest_pattern(std::move(nested_rest_pat));
        }

        return std::move(destructuring);
        
    } else if (current_token().get_type() == TokenType::LEFT_BRACE) {
        advance();
        
        std::vector<std::unique_ptr<Identifier>> targets;
        std::vector<std::pair<std::string, std::string>> property_mappings;
        std::vector<std::pair<std::string, std::shared_ptr<ASTNode>>> computed_key_exprs;
        std::vector<std::pair<size_t, std::unique_ptr<ASTNode>>> obj_default_exprs;

        while (!match(TokenType::RIGHT_BRACE) && !at_end()) {
            if (current_token().get_type() == TokenType::ELLIPSIS) {
                advance();

                if (current_token().get_type() != TokenType::IDENTIFIER) {
                    add_error("Expected identifier after '...' in object destructuring");
                    return nullptr;
                }

                auto rest_id = std::make_unique<Identifier>(
                    "..." + current_token().get_value(),
                    current_token().get_start(),
                    current_token().get_end()
                );
                targets.push_back(std::move(rest_id));
                advance();

                if (match(TokenType::COMMA)) {
                    add_error("Rest element must be last element in object destructuring");
                    return nullptr;
                }
                break;

            } else if (current_token().get_type() == TokenType::IDENTIFIER ||
                       current_token().get_type() == TokenType::NUMBER ||
                       current_token().get_type() == TokenType::STRING ||
                       current_token().get_type() == TokenType::BIGINT_LITERAL ||
                       (current_token().get_type() == TokenType::AWAIT && !options_.in_async_body && !options_.source_type_module && !options_.in_class_static_block) ||
                       (current_token().get_type() == TokenType::YIELD && !options_.in_generator_body && !options_.strict_mode)) {
                // AWAIT/YIELD used as identifier bindings are NOT literal keys (they behave like IDENTIFIER)
                bool is_literal_key = (current_token().get_type() == TokenType::NUMBER ||
                                       current_token().get_type() == TokenType::STRING ||
                                       current_token().get_type() == TokenType::BIGINT_LITERAL);
                if (!is_literal_key && current_token().has_escaped_keyword()) {
                    add_error("SyntaxError: Unicode escape sequences are not allowed in keywords");
                    return nullptr;
                }
                auto id = std::make_unique<Identifier>(
                    current_token().get_value(),
                    current_token().get_start(),
                    current_token().get_end()
                );
                targets.push_back(std::move(id));
                advance();

                if (match(TokenType::COLON)) {
                    advance();
                    
                    if (match(TokenType::LEFT_BRACE)) {
                        auto nested = parse_destructuring_pattern(depth + 1);
                        if (!nested) {
                            add_error("Invalid nested object destructuring");
                            return nullptr;
                        }

                        std::string nested_vars = extract_nested_variable_names(nested.get());

                        std::string original_property_name = targets.back()->get_name();


                        std::string proper_pattern = nested_vars;
                        if (auto nested_destructuring = dynamic_cast<DestructuringAssignment*>(nested.get())) {
                            const auto& mappings = nested_destructuring->get_property_mappings();

                            std::string property_name = "";
                            if (!mappings.empty()) {
                                property_name = mappings[0].property_name;
                            } else {
                                const auto& targets = nested_destructuring->get_targets();
                                if (!targets.empty()) {
                                    std::string first_target = targets[0]->get_name();

                                    if (first_target.find("__nested") == std::string::npos) {
                                        property_name = first_target;
                                    }
                                }
                            }

                            if (!property_name.empty()) {
                                size_t nested_pos = nested_vars.find("__nested:");
                                if (nested_pos != std::string::npos && nested_pos + 9 < nested_vars.length()) {
                                    proper_pattern = property_name + ":" + nested_vars;
                                } else {
                                    if (property_name == nested_vars) {
                                        proper_pattern = "__nested:" + nested_vars;
                                    } else {
                                        proper_pattern = property_name + ":" + nested_vars;
                                    }
                                }
                            }
                        }
                        property_mappings.emplace_back(original_property_name, proper_pattern);

                        if (match(TokenType::ASSIGN)) {
                            advance();
                            auto default_expr = parse_assignment_expression();
                            if (!default_expr) {
                                add_error("Expected expression after '=' in object destructuring default");
                                return nullptr;
                            }
                            size_t target_index = targets.size() - 1;
                            obj_default_exprs.emplace_back(target_index, std::move(default_expr));
                        }

                    } else if (match(TokenType::LEFT_BRACKET)) {
                        auto nested = parse_destructuring_pattern(depth + 1);
                        if (!nested) {
                            add_error("Invalid nested array destructuring");
                            return nullptr;
                        }

                        std::string nested_vars = extract_nested_variable_names(nested.get());

                        std::string original_property_name = targets.back()->get_name();

                        auto nested_id = std::make_unique<Identifier>(
                            "__nested_array:" + nested_vars,
                            nested->get_start(),
                            nested->get_end()
                        );

                        targets.pop_back();
                        targets.push_back(std::move(nested_id));

                        property_mappings.emplace_back(original_property_name, "__nested_array:" + nested_vars);

                        if (match(TokenType::ASSIGN)) {
                            advance();
                            auto default_expr = parse_assignment_expression();
                            if (!default_expr) {
                                add_error("Expected expression after '=' in object destructuring default");
                                return nullptr;
                            }
                            size_t target_index = targets.size() - 1;
                            obj_default_exprs.emplace_back(target_index, std::move(default_expr));
                        }
                    } else if (match(TokenType::IDENTIFIER)) {
                        std::string new_name = current_token().get_value();
                        Position new_pos = current_token().get_start();
                        Position new_end = current_token().get_end();
                        
                        std::string original_name = targets.back()->get_name();
                        
                        targets.pop_back();
                        auto new_id = std::make_unique<Identifier>(new_name, new_pos, new_end);
                        targets.push_back(std::move(new_id));
                        
                        property_mappings.emplace_back(original_name, new_name);
                        advance();

                        // Handle default value after renamed identifier: {x: a = expr}
                        if (match(TokenType::ASSIGN)) {
                            advance();
                            auto default_expr = parse_assignment_expression();
                            if (!default_expr) {
                                add_error("Expected expression after '=' in object destructuring default");
                                return nullptr;
                            }
                            size_t target_index = targets.size() - 1;
                            obj_default_exprs.emplace_back(target_index, std::move(default_expr));
                        }
                    } else {
                        add_error("Expected identifier or nested pattern after ':'");
                        return nullptr;
                    }
                } else if (!is_literal_key && match(TokenType::ASSIGN)) {
                    // Handle shorthand default: {a = expr}  (only valid for identifier keys)
                    advance();
                    auto default_expr = parse_assignment_expression();
                    if (!default_expr) {
                        add_error("Expected expression after '=' in object destructuring default");
                        return nullptr;
                    }
                    size_t target_index = targets.size() - 1;
                    obj_default_exprs.emplace_back(target_index, std::move(default_expr));
                } else if (is_literal_key) {
                    add_error("Expected ':' after numeric/string key in object destructuring");
                    return nullptr;
                }
            } else if (current_token().get_type() == TokenType::LEFT_BRACKET) {
                // Computed property key: { [expr]: varName }
                advance(); // skip '['
                auto key_expr = parse_assignment_expression();
                if (!key_expr) {
                    add_error("Expected expression in computed destructuring key");
                    return nullptr;
                }
                if (!consume(TokenType::RIGHT_BRACKET)) {
                    add_error("Expected ']' after computed destructuring key");
                    return nullptr;
                }
                if (!consume(TokenType::COLON)) {
                    add_error("Expected ':' after computed destructuring key");
                    return nullptr;
                }
                if (current_token().get_type() != TokenType::IDENTIFIER) {
                    add_error("Expected identifier after ':' in computed destructuring");
                    return nullptr;
                }
                std::string var_name = current_token().get_value();
                Position vstart = current_token().get_start();
                Position vend = current_token().get_end();
                advance();

                // Store: target is the variable, mapping uses __computed_N as property placeholder
                auto var_id = std::make_unique<Identifier>(var_name, vstart, vend);
                targets.push_back(std::move(var_id));

                // Store the computed key expression -- shared_ptr so clone() works
                std::string key_str = "__computed:" + key_expr->to_string();
                property_mappings.emplace_back(key_str, var_name);
                computed_key_exprs.emplace_back(key_str, std::shared_ptr<ASTNode>(key_expr.release()));

                if (match(TokenType::ASSIGN)) {
                    advance();
                    auto default_expr = parse_assignment_expression();
                    if (!default_expr) {
                        add_error("Expected expression after '=' in computed destructuring default");
                        return nullptr;
                    }
                    size_t target_index = targets.size() - 1;
                    obj_default_exprs.emplace_back(target_index, std::move(default_expr));
                }
            } else {
                add_error("Expected identifier in object destructuring");
                return nullptr;
            }

            if (match(TokenType::COMMA)) {
                advance();
            } else {
                break;
            }
        }

        if (!consume(TokenType::RIGHT_BRACE)) {
            add_error("Expected '}' to close object destructuring");
            return nullptr;
        }
        
        Position end = get_current_position();
        auto destructuring = std::make_unique<DestructuringAssignment>(
            std::move(targets), nullptr, DestructuringAssignment::Type::OBJECT, start, end
        );
        
        for (const auto& mapping : property_mappings) {
            auto it = std::find_if(computed_key_exprs.begin(), computed_key_exprs.end(),
                [&](const auto& p){ return p.first == mapping.first; });
            if (it != computed_key_exprs.end()) {
                destructuring->add_computed_property_mapping(mapping.first, mapping.second, it->second);
            } else {
                destructuring->add_property_mapping(mapping.first, mapping.second);
            }
        }

        for (auto& default_pair : obj_default_exprs) {
            destructuring->add_default_value(default_pair.first, std::move(default_pair.second));
        }

        return std::move(destructuring);
    }
    
    add_error("Expected '[' or '{' for destructuring pattern");
    return nullptr;
}

std::unique_ptr<ASTNode> Parser::parse_spread_element() {
    Position start = get_current_position();
    
    if (current_token().get_type() != TokenType::ELLIPSIS) {
        add_error("Expected '...' for spread element");
        return nullptr;
    }
    
    advance();

    bool saved_iae = options_.in_array_element;
    options_.in_array_element = true;
    auto argument = parse_assignment_expression();
    options_.in_array_element = saved_iae;
    if (!argument) {
        add_error("Expected expression after '...'");
        return nullptr;
    }

    Position end = get_current_position();
    return std::make_unique<SpreadElement>(std::move(argument), start, end);
}


std::unique_ptr<ASTNode> Parser::parse_jsx_element() {
    Position start = current_token().get_start();
    
    if (!consume(TokenType::LESS_THAN)) {
        add_error("Expected '<' at start of JSX element");
        return nullptr;
    }
    
    if (current_token().get_type() != TokenType::IDENTIFIER) {
        add_error("Expected JSX tag name");
        return nullptr;
    }
    
    std::string tag_name = current_token().get_value();
    advance();
    
    std::vector<std::unique_ptr<ASTNode>> attributes;
    while (current_token().get_type() == TokenType::IDENTIFIER) {
        auto attr = parse_jsx_attribute();
        if (attr) {
            attributes.push_back(std::move(attr));
        }
    }
    
    if (match(TokenType::DIVIDE)) {
        advance();
        if (!consume(TokenType::GREATER_THAN)) {
            add_error("Expected '>' after '/' in self-closing JSX tag");
            return nullptr;
        }
        Position end = get_current_position();
        return std::make_unique<JSXElement>(tag_name, std::move(attributes), 
                                            std::vector<std::unique_ptr<ASTNode>>(), true, start, end);
    }
    
    if (!consume(TokenType::GREATER_THAN)) {
        add_error("Expected '>' after JSX opening tag");
        return nullptr;
    }
    
    std::vector<std::unique_ptr<ASTNode>> children;
    while (current_token().get_type() != TokenType::EOF_TOKEN) {
        if (match(TokenType::LESS_THAN)) {
            size_t saved_pos = current_token_index_;
            advance();
            if (match(TokenType::DIVIDE)) {
                current_token_index_ = saved_pos;
                break;
            } else {
                current_token_index_ = saved_pos;
            }
        }
        
        
        if (match(TokenType::LEFT_BRACE)) {
            auto expr = parse_jsx_expression();
            if (expr) {
                children.push_back(std::move(expr));
            }
        } else if (match(TokenType::LESS_THAN)) {
            auto nested = parse_jsx_element();
            if (nested) {
                children.push_back(std::move(nested));
            }
        } else {
            auto text = parse_jsx_text();
            if (text) {
                children.push_back(std::move(text));
            }
        }
    }
    
    if (!consume(TokenType::LESS_THAN) || !consume(TokenType::DIVIDE)) {
        add_error("Expected '</' for JSX closing tag");
        return nullptr;
    }
    
    if (current_token().get_type() != TokenType::IDENTIFIER ||
        current_token().get_value() != tag_name) {
        add_error("JSX closing tag '" + current_token().get_value() + 
                  "' does not match opening tag '" + tag_name + "'");
        return nullptr;
    }
    advance();
    
    if (!consume(TokenType::GREATER_THAN)) {
        add_error("Expected '>' after JSX closing tag");
        return nullptr;
    }
    
    Position end = get_current_position();
    return std::make_unique<JSXElement>(tag_name, std::move(attributes), 
                                        std::move(children), false, start, end);
}

std::unique_ptr<ASTNode> Parser::parse_jsx_text() {
    Position start = current_token().get_start();
    std::string text;
    
    while (current_token().get_type() != TokenType::LESS_THAN &&
           current_token().get_type() != TokenType::LEFT_BRACE &&
           current_token().get_type() != TokenType::EOF_TOKEN) {
        text += current_token().get_value();
        if (current_token().get_type() == TokenType::WHITESPACE ||
            current_token().get_type() == TokenType::NEWLINE) {
            text += " ";
        }
        advance();
    }
    
    text.erase(0, text.find_first_not_of(" \t\n\r"));
    text.erase(text.find_last_not_of(" \t\n\r") + 1);
    
    Position end = get_current_position();
    return std::make_unique<JSXText>(text, start, end);
}

std::unique_ptr<ASTNode> Parser::parse_jsx_expression() {
    Position start = current_token().get_start();
    
    if (!consume(TokenType::LEFT_BRACE)) {
        add_error("Expected '{' at start of JSX expression");
        return nullptr;
    }
    
    auto expression = parse_expression();
    if (!expression) {
        add_error("Expected expression in JSX expression");
        return nullptr;
    }
    
    if (!consume(TokenType::RIGHT_BRACE)) {
        add_error("Expected '}' at end of JSX expression");
        return nullptr;
    }
    
    Position end = get_current_position();
    return std::make_unique<JSXExpression>(std::move(expression), start, end);
}

std::unique_ptr<ASTNode> Parser::parse_jsx_attribute() {
    Position start = current_token().get_start();
    
    if (current_token().get_type() != TokenType::IDENTIFIER) {
        add_error("Expected attribute name");
        return nullptr;
    }
    
    std::string attr_name = current_token().get_value();
    advance();
    
    std::unique_ptr<ASTNode> value = nullptr;
    
    if (consume(TokenType::ASSIGN)) {
        if (match(TokenType::STRING)) {
            value = parse_string_literal();
        } else if (match(TokenType::LEFT_BRACE)) {
            value = parse_jsx_expression();
        } else {
            add_error("Expected string literal or JSX expression after '='");
            return nullptr;
        }
    }
    
    Position end = get_current_position();
    return std::make_unique<JSXAttribute>(attr_name, std::move(value), start, end);
}

void Parser::check_for_use_strict_directive() {
    // Lookahead-only scan: detect 'use strict' without consuming tokens.
    // Consuming tokens here would cause string-literal expression statements
    // (e.g. eval("'1'")) to disappear from the parse tree.
    auto skip_trivia = [&](size_t i) {
        while (i < tokens_.size() &&
               (tokens_[i].get_type() == TokenType::NEWLINE ||
                tokens_[i].get_type() == TokenType::WHITESPACE ||
                tokens_[i].get_type() == TokenType::COMMENT)) {
            i++;
        }
        return i;
    };
    size_t idx = skip_trivia(current_token_index_);
    while (idx < tokens_.size() && tokens_[idx].get_type() == TokenType::STRING) {
        std::string str_value = tokens_[idx].get_value();
        idx = skip_trivia(idx + 1);
        if (idx < tokens_.size() && tokens_[idx].get_type() == TokenType::SEMICOLON) {
            idx = skip_trivia(idx + 1);
        }
        if (str_value == "use strict") {
            options_.strict_mode = true;
            return;
        }
    }
}

std::unique_ptr<ASTNode> Parser::parse_async_arrow_function(Position start) {
    std::vector<std::unique_ptr<Parameter>> params;
    bool has_non_simple_params = false;

    if (!consume(TokenType::LEFT_PAREN)) {
        add_error("Expected '(' for async arrow function parameters");
        return nullptr;
    }

    // Params are in async context: await is AwaitExpression (forbidden in async arrow params)
    bool saved_async_aaf_params = options_.in_async_body;
    options_.in_async_body = true;

    while (!match(TokenType::RIGHT_PAREN) && !at_end()) {
        Position param_start = current_token().get_start();
        bool is_rest = false;

        if (match(TokenType::ELLIPSIS)) {
            is_rest = true;
            has_non_simple_params = true;
            advance();
        }

        std::unique_ptr<Identifier> param_name = nullptr;
        if (current_token().get_type() == TokenType::LEFT_BRACKET ||
            current_token().get_type() == TokenType::LEFT_BRACE) {
            has_non_simple_params = true;
            auto destructuring = parse_destructuring_pattern();
            if (!destructuring) {
                add_error("Invalid destructuring pattern in async arrow function parameters");
                options_.in_async_body = saved_async_aaf_params;
                return nullptr;
            }
            static int aaf_destr_counter = 0;
            std::string synthetic_name = "__destr_" + std::to_string(aaf_destr_counter++);
            Position pp = get_current_position();
            param_name = std::make_unique<Identifier>(synthetic_name, pp, pp);
            std::unique_ptr<ASTNode> default_value = nullptr;
            if (!is_rest && match(TokenType::ASSIGN)) {
                advance();
                default_value = parse_assignment_expression();
                if (!default_value) {
                    add_error("Invalid default parameter value");
                    options_.in_async_body = saved_async_aaf_params;
                    return nullptr;
                }
            }
            Position param_end = get_current_position();
            auto param = std::make_unique<Parameter>(std::move(param_name), std::move(default_value), is_rest, param_start, param_end);
            param->set_destructuring_pattern(std::move(destructuring));
            params.push_back(std::move(param));
            if (match(TokenType::COMMA)) advance();
            else if (is_rest && !match(TokenType::RIGHT_PAREN)) {
                add_error("Rest parameter must be last formal parameter");
                options_.in_async_body = saved_async_aaf_params;
                return nullptr;
            }
            continue;
        } else if (current_token().get_type() == TokenType::IDENTIFIER) {
            const std::string& pn = current_token().get_value();
            if (options_.strict_mode && (pn == "eval" || pn == "arguments")) {
                add_error("SyntaxError: '" + pn + "' cannot be used as parameter name in strict mode");
                return nullptr;
            }
            param_name = std::make_unique<Identifier>(pn,
                                                      current_token().get_start(), current_token().get_end());
            advance();
        } else {
            add_error("Expected parameter name");
            return nullptr;
        }

        std::unique_ptr<ASTNode> default_value = nullptr;
        if (!is_rest && match(TokenType::ASSIGN)) {
            has_non_simple_params = true;
            advance();
            default_value = parse_assignment_expression();
            if (!default_value) {
                add_error("Invalid default parameter value");
                return nullptr;
            }
        }

        Position param_end = get_current_position();
        auto param = std::make_unique<Parameter>(std::move(param_name), std::move(default_value), is_rest, param_start, param_end);
        params.push_back(std::move(param));

        if (is_rest) {
            if (!match(TokenType::RIGHT_PAREN)) {
                add_error("Rest parameter must be last formal parameter");
                return nullptr;
            }
            break;
        }

        if (match(TokenType::COMMA)) {
            advance();
        } else if (!match(TokenType::RIGHT_PAREN)) {
            add_error("Expected ',' or ')' in parameter list");
            return nullptr;
        }
    }

    if (!consume(TokenType::RIGHT_PAREN)) {
        options_.in_async_body = saved_async_aaf_params;
        add_error("Expected ')' after parameters");
        return nullptr;
    }

    options_.in_async_body = saved_async_aaf_params;

    // await/yield in default expressions are forbidden in async arrow formals
    {
        std::string forbidden = find_forbidden_expr_in_params(params, false, true);
        if (!forbidden.empty()) {
            add_error("SyntaxError: " + forbidden + " expression not allowed in formal parameters of async arrow");
            return nullptr;
        }
    }

    // Duplicate param check: always SyntaxError for async arrow (implicitly strict)
    for (size_t pi = 0; pi < params.size(); pi++) {
        if (!params[pi]->get_name()) continue;
        const std::string& pn = params[pi]->get_name()->get_name();
        if (pn.empty()) continue;
        for (size_t pj = pi + 1; pj < params.size(); pj++) {
            if (!params[pj]->get_name()) continue;
            if (params[pj]->get_name()->get_name() == pn) {
                add_error("SyntaxError: Duplicate parameter name not allowed in async arrow function");
                return nullptr;
            }
        }
    }

    if (!consume(TokenType::ARROW)) {
        add_error("Expected '=>' for async arrow function");
        return nullptr;
    }

    std::unique_ptr<ASTNode> body = nullptr;
    bool saved_async_aa = options_.in_async_body;
    options_.in_async_body = true;
    if (match(TokenType::LEFT_BRACE)) {
        options_.function_depth++;
        body = parse_block_statement(true);
        options_.function_depth--;
        if (body && has_non_simple_params) {
            auto* block = static_cast<BlockStatement*>(body.get());
            for (const auto& stmt : block->get_statements()) {
                if (!stmt || stmt->get_type() != ASTNode::Type::EXPRESSION_STATEMENT) break;
                auto* es = static_cast<ExpressionStatement*>(stmt.get());
                if (!es->get_expression()) break;
                if (es->get_expression()->get_type() != ASTNode::Type::STRING_LITERAL) break;
                auto* sl = static_cast<StringLiteral*>(es->get_expression());
                if (sl->get_value() == "use strict") {
                    add_error("SyntaxError: Illegal 'use strict' directive in function with non-simple parameter list");
                    options_.in_async_body = saved_async_aa;
                    return nullptr;
                }
                break;
            }
        }
    } else {
        auto expr = parse_assignment_expression();
        if (!expr) {
            add_error("Expected function body");
            return nullptr;
        }

        Position ret_start = expr->get_start();
        Position ret_end = expr->get_end();
        auto return_stmt = std::make_unique<ReturnStatement>(std::move(expr), ret_start, ret_end);

        std::vector<std::unique_ptr<ASTNode>> statements;
        statements.push_back(std::move(return_stmt));
        body = std::make_unique<BlockStatement>(std::move(statements), ret_start, ret_end);
    }

    options_.in_async_body = saved_async_aa;

    if (!body) {
        add_error("Expected async arrow function body");
        return nullptr;
    }

    // Early error: params and body cannot have the same lexical binding
    if (body->get_type() == ASTNode::Type::BLOCK_STATEMENT) {
        auto* block = static_cast<BlockStatement*>(body.get());
        std::string dup = check_params_body_lex_conflict(params, block);
        if (!dup.empty()) {
            add_error("SyntaxError: Identifier '" + dup + "' is already declared (parameter name shadows lexical declaration)");
            return nullptr;
        }
    }

    Position end = get_current_position();

    return std::make_unique<AsyncFunctionExpression>(
        nullptr,
        std::move(params),
        std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body.release())),
        start, end, true
    );
}

std::unique_ptr<ASTNode> Parser::parse_async_arrow_function_single_param(Position start) {
    std::vector<std::unique_ptr<Parameter>> params;

    if (current_token().get_type() != TokenType::IDENTIFIER &&
        !is_keyword_token(current_token().get_type()) &&
        current_token().get_type() != TokenType::OF) {
        add_error("Expected identifier for async arrow function parameter");
        return nullptr;
    }

    auto param_name = std::make_unique<Identifier>(current_token().get_value(),
                                                   current_token().get_start(), current_token().get_end());
    Position param_end = current_token().get_end();
    advance();

    auto param = std::make_unique<Parameter>(std::move(param_name), nullptr, false, start, param_end);
    params.push_back(std::move(param));

    if (!consume(TokenType::ARROW)) {
        add_error("Expected '=>' for async arrow function");
        return nullptr;
    }

    std::unique_ptr<ASTNode> body = nullptr;
    bool saved_async_sap = options_.in_async_body;
    options_.in_async_body = true;
    if (match(TokenType::LEFT_BRACE)) {
        options_.function_depth++;
        body = parse_block_statement(true);
        options_.function_depth--;
    } else {
        auto expr = parse_assignment_expression();
        if (!expr) {
            add_error("Expected function body");
            options_.in_async_body = saved_async_sap;
            return nullptr;
        }

        Position ret_start = expr->get_start();
        Position ret_end = expr->get_end();
        auto return_stmt = std::make_unique<ReturnStatement>(std::move(expr), ret_start, ret_end);

        std::vector<std::unique_ptr<ASTNode>> statements;
        statements.push_back(std::move(return_stmt));
        body = std::make_unique<BlockStatement>(std::move(statements), ret_start, ret_end);
    }

    options_.in_async_body = saved_async_sap;

    if (!body) {
        add_error("Expected async arrow function body");
        return nullptr;
    }

    Position end = get_current_position();

    return std::make_unique<AsyncFunctionExpression>(
        nullptr,
        std::move(params),
        std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body.release())),
        start, end, true
    );
}

std::string Parser::extract_nested_variable_names(ASTNode* node) {
    if (!node) {
        return "";
    }

    std::string result = generate_proper_nested_pattern(node, 0);
    return result;
}

std::string Parser::generate_proper_nested_pattern(ASTNode* node, int depth) {
    if (!node) return "";

    if (node->get_type() == ASTNode::Type::IDENTIFIER) {
        auto* id = static_cast<Identifier*>(node);
        return id->get_name();
    }
    else if (node->get_type() == ASTNode::Type::OBJECT_LITERAL) {
        auto* obj = static_cast<ObjectLiteral*>(node);
        std::vector<std::string> nested_vars;

        for (const auto& prop : obj->get_properties()) {
            std::string prop_name = "";
            if (prop->key && prop->key->get_type() == ASTNode::Type::IDENTIFIER) {
                prop_name = static_cast<Identifier*>(prop->key.get())->get_name();
            }

            std::string value_pattern = generate_proper_nested_pattern(prop->value.get(), depth + 1);
            if (!value_pattern.empty()) {
                std::string prefixed_pattern = value_pattern;
                if (depth > 0 && !prop_name.empty()) {
                    prefixed_pattern = prop_name + ":__nested:" + value_pattern;
                } else if (!prop_name.empty()) {
                    prefixed_pattern = prop_name + ":" + value_pattern;
                } else {
                    prefixed_pattern = value_pattern;
                }
                nested_vars.push_back(prefixed_pattern);
            }
        }

        std::string result;
        for (size_t i = 0; i < nested_vars.size(); ++i) {
            if (i > 0) result += ",";
            result += nested_vars[i];
        }
        return result;
    }
    else if (node->get_type() == ASTNode::Type::DESTRUCTURING_ASSIGNMENT) {
        auto* destructuring = static_cast<DestructuringAssignment*>(node);
        const auto& mappings = destructuring->get_property_mappings();

        if (!mappings.empty()) {
            std::vector<std::string> nested_vars;
            for (const auto& mapping : mappings) {
                nested_vars.push_back(mapping.variable_name);
                break;
            }

            std::string result;
            for (size_t i = 0; i < nested_vars.size(); ++i) {
                if (i > 0) result += ",";
                result += nested_vars[i];
            }
            return result;
        } else {
            std::vector<std::string> nested_vars;
            for (const auto& target : destructuring->get_targets()) {
                std::string target_pattern = generate_proper_nested_pattern(target.get(), depth);
                if (!target_pattern.empty()) {
                    nested_vars.push_back(target_pattern);
                }
            }

            std::string result;
            for (size_t i = 0; i < nested_vars.size(); ++i) {
                if (i > 0) result += ",";
                result += nested_vars[i];
            }
            return result;
        }
    }

    return "";
}

void Parser::extract_variable_names_recursive(ASTNode* node, std::vector<std::string>& names) {
    if (!node) return;

    if (node->get_type() == ASTNode::Type::IDENTIFIER) {
        auto* id = static_cast<Identifier*>(node);
        names.push_back(id->get_name());
    }
    else if (node->get_type() == ASTNode::Type::DESTRUCTURING_ASSIGNMENT) {
        auto* destructuring = static_cast<DestructuringAssignment*>(node);
        for (const auto& target : destructuring->get_targets()) {
            extract_variable_names_recursive(target.get(), names);
        }
    }
    else if (node->get_type() == ASTNode::Type::OBJECT_LITERAL) {
        auto* obj = static_cast<ObjectLiteral*>(node);
        for (const auto& prop : obj->get_properties()) {
            extract_variable_names_recursive(prop->value.get(), names);
        }
    }
    else if (node->get_type() == ASTNode::Type::ARRAY_LITERAL) {
        auto* arr = static_cast<ArrayLiteral*>(node);
        for (const auto& elem : arr->get_elements()) {
            extract_variable_names_recursive(elem.get(), names);
        }
    }
}


}
