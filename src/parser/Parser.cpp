/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/parser/Parser.h"
#include <algorithm>
#include <iostream>
#include <map>

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
    
    Position end = get_current_position();
    return std::make_unique<Program>(std::move(statements), start, end);
}

std::unique_ptr<ASTNode> Parser::parse_statement() {
    TokenType current_type = current_token().get_type();
    
    switch (current_type) {
        case TokenType::VAR:
        case TokenType::LET:
        case TokenType::CONST:
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
            
        case TokenType::ASYNC:
            return parse_async_function_declaration();
            
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
            
        case TokenType::IMPORT:
            if (peek_token().get_type() == TokenType::LEFT_PAREN) {
                return parse_expression_statement();
            } else {
                return parse_import_statement();
            }
            
        case TokenType::EXPORT:
            return parse_export_statement();

        case TokenType::SEMICOLON:
            {
                Position start = current_token().get_start();
                Position end = current_token().get_end();
                advance();
                return std::make_unique<EmptyStatement>(start, end);
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
    if (match(TokenType::IDENTIFIER) && peek_token(1).get_type() == TokenType::ARROW) {
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
    
    if (is_assignment_operator(current_token().get_type())) {
        if (!is_valid_assignment_target(left.get())) {
            add_error("Invalid left-hand side in assignment");
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
        
        BinaryExpression::Operator op = token_to_binary_operator(op_token);
        Position end = right->get_end();
        
        return std::make_unique<BinaryExpression>(
            std::move(left), op, std::move(right), op_start, end
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
        
        auto consequent = parse_logical_or_expression();
        if (!consequent) {
            add_error("Expected expression after '?' in conditional expression");
            return nullptr;
        }
        
        if (current_token().get_type() != TokenType::COLON) {
            add_error("Expected ':' after consequent in conditional expression");
            return nullptr;
        }
        advance();
        
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
    return parse_binary_expression(
        [this]() { return parse_nullish_coalescing_expression(); },
        {TokenType::LOGICAL_OR}
    );
}

std::unique_ptr<ASTNode> Parser::parse_nullish_coalescing_expression() {
    auto left = parse_logical_and_expression();
    if (!left) return nullptr;
    
    while (match(TokenType::NULLISH_COALESCING)) {
        Position start = left->get_start();
        advance();
        
        auto right = parse_logical_and_expression();
        if (!right) {
            add_error("Expected expression after '??'");
            return left;
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

std::unique_ptr<ASTNode> Parser::parse_relational_expression() {
    return parse_binary_expression(
        [this]() { return parse_shift_expression(); },
        {TokenType::LESS_THAN, TokenType::GREATER_THAN, TokenType::LESS_EQUAL, TokenType::GREATER_EQUAL, TokenType::INSTANCEOF, TokenType::IN}
    );
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
        Position start = current_token().get_start();
        advance();
        
        auto argument = parse_unary_expression();
        if (!argument) {
            add_error("Expected expression after 'await'");
            return nullptr;
        }
        
        Position end = get_current_position();
        return std::make_unique<AwaitExpression>(std::move(argument), start, end);
    }


    if (is_unary_operator(current_token().get_type())) {
        TokenType op_token = current_token().get_type();
        Position start = current_token().get_start();
        advance();
        
        auto operand = parse_unary_expression();
        if (!operand) {
            add_error("Expected expression after unary operator");
            return nullptr;
        }
        
        UnaryExpression::Operator op = token_to_unary_operator(op_token);
        Position end = operand->get_end();
        
        return std::make_unique<UnaryExpression>(op, std::move(operand), true, start, end);
    }
    
    return parse_postfix_expression();
}

std::unique_ptr<ASTNode> Parser::parse_postfix_expression() {
    auto expr = parse_call_expression();
    if (!expr) return nullptr;
    
    while (current_token().get_type() == TokenType::INCREMENT || 
           current_token().get_type() == TokenType::DECREMENT) {
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

        if (peek_token().get_type() == TokenType::DOT) {
            advance();
            advance();

            if (current_token().get_type() == TokenType::IDENTIFIER &&
                current_token().get_value() == "target") {
                Position end = current_token().get_end();
                advance();
                return std::make_unique<MetaProperty>("new", "target", start, end);
            } else {
                add_error("Expected 'target' after 'new.'");
                return nullptr;
            }
        }

        advance();

        auto constructor = parse_primary_expression();
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
            }
        }

        std::vector<std::unique_ptr<ASTNode>> arguments;

        if (current_token().get_type() == TokenType::LEFT_PAREN) {
            advance();

            if (current_token().get_type() != TokenType::RIGHT_PAREN) {
                do {
                    auto arg = parse_assignment_expression();
                    if (!arg) {
                        add_error("Expected argument expression");
                        return nullptr;
                    }
                    arguments.push_back(std::move(arg));

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
                prop_start = current_token().get_start();
                advance();

                if (!match(TokenType::IDENTIFIER)) {
                    add_error("Expected identifier after '#' in member access");
                    return expr;
                }

                name = "#" + current_token().get_value();
                prop_end = current_token().get_end();
                advance();
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
                        auto arg = parse_assignment_expression();
                        if (!arg) {
                            add_error("Expected argument in optional call");
                            break;
                        }
                        arguments.push_back(std::move(arg));

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
                expr = std::make_unique<CallExpression>(std::move(expr), std::move(arguments), start, end);
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
            expr = std::make_unique<CallExpression>(std::move(expr), std::move(arguments), start, end);
        } else if (match(TokenType::TEMPLATE_LITERAL)) {
            const Token& template_token = current_token();
            Position template_start = template_token.get_start();
            Position template_end = template_token.get_end();
            std::string template_str = template_token.get_value();

            advance();

            std::vector<std::unique_ptr<ASTNode>> arguments;

            auto string_literal = std::make_unique<StringLiteral>(template_str, template_start, template_end);
            arguments.push_back(std::move(string_literal));

            Position end = template_end;
            expr = std::make_unique<CallExpression>(std::move(expr), std::move(arguments), start, end);
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
            return parse_undefined_literal();
        case TokenType::IDENTIFIER:
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
        case TokenType::YIELD:
            return parse_yield_expression();
        case TokenType::IMPORT:
            return parse_import_expression();
        case TokenType::LEFT_BRACE:
            return parse_object_literal();
        case TokenType::LEFT_BRACKET:
            return parse_array_literal();
        case TokenType::TEMPLATE_LITERAL:
            return parse_template_literal();
        case TokenType::REGEX:
            return parse_regex_literal();
        case TokenType::LESS_THAN:
            return parse_jsx_element();
        default: {
            std::string error_msg = "Unexpected token: '" + token.get_value() + "' (type: " + std::to_string(static_cast<int>(token.get_type())) + ") at line " + std::to_string(token.get_start().line);
            add_error(error_msg);
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

    Position start = token.get_start();
    Position end = token.get_end();
    advance();

    while (!at_end() && current_token().get_type() == TokenType::STRING) {
        value += current_token().get_value();
        end = current_token().get_end();
        advance();
    }

    return std::make_unique<StringLiteral>(value, start, end);
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
    advance();
    
    return std::make_unique<Identifier>("super", start, end);
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
    
    std::vector<TemplateLiteral::Element> elements;
    
    size_t pos = 0;
    while (pos < template_str.length()) {
        size_t expr_start = template_str.find("${", pos);
        
        if (expr_start == std::string::npos) {
            if (pos < template_str.length()) {
                elements.emplace_back(template_str.substr(pos));
            }
            break;
        }
        
        if (expr_start > pos) {
            elements.emplace_back(template_str.substr(pos, expr_start - pos));
        }
        
        size_t expr_end = std::string::npos;
        int brace_count = 1;
        for (size_t i = expr_start + 2; i < template_str.length(); ++i) {
            if (template_str[i] == '{') {
                brace_count++;
            } else if (template_str[i] == '}') {
                brace_count--;
                if (brace_count == 0) {
                    expr_end = i;
                    break;
                }
            }
        }

        if (expr_end == std::string::npos) {
            add_error("Unterminated expression in template literal");
            return nullptr;
        }
        
        std::string expr_str = template_str.substr(expr_start + 2, expr_end - expr_start - 2);
        
        Lexer expr_lexer(expr_str);
        TokenSequence expr_tokens = expr_lexer.tokenize();
        Parser expr_parser(std::move(expr_tokens));
        
        auto expression = expr_parser.parse_expression();
        if (!expression) {
            add_error("Invalid expression in template literal: " + expr_str);
            return nullptr;
        }
        
        elements.emplace_back(std::move(expression));
        pos = expr_end + 1;
    }
    
    return std::make_unique<TemplateLiteral>(std::move(elements), start, end);
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
    
    Position start = token.get_start();
    Position end = token.get_end();
    advance();
    
    return std::make_unique<Identifier>(name, start, end);
}

std::unique_ptr<ASTNode> Parser::parse_private_field() {
    Position start = get_current_position();

    if (!consume(TokenType::HASH)) {
        add_error("Expected '#'");
        return nullptr;
    }

    if (!match(TokenType::IDENTIFIER)) {
        add_error("Expected identifier after '#'");
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
    
    auto expr = parse_expression();
    if (!expr) {
        add_error("Expected expression inside parentheses");
        return nullptr;
    }
    
    if (!consume(TokenType::RIGHT_PAREN)) {
        add_error("Expected ')' after expression");
        return expr;
    }
    
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
        
        auto right = parse_operand();
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
           type == TokenType::YIELD;
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
           type == TokenType::UNSIGNED_RIGHT_SHIFT_ASSIGN;
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
           type == TokenType::UNDEFINED ||
           type == TokenType::NULL_LITERAL ||
           type == TokenType::BOOLEAN;
}

bool Parser::is_valid_assignment_target(ASTNode* node) const {
    if (!node) return false;

    switch (node->get_type()) {
        case ASTNode::Type::IDENTIFIER:
            return true;
        case ASTNode::Type::MEMBER_EXPRESSION:
            return true;
        case ASTNode::Type::CALL_EXPRESSION:
            return false;
        case ASTNode::Type::OBJECT_LITERAL:
            return true;
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
        
        if (current_token().get_type() != TokenType::IDENTIFIER) {
            add_error("Expected identifier in variable declaration");
            return nullptr;
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
        consume_if_match(TokenType::SEMICOLON);
    }
    
    Position end = get_current_position();
    return std::make_unique<VariableDeclaration>(std::move(declarations), kind, start, end);
}

std::unique_ptr<ASTNode> Parser::parse_block_statement() {
    Position start = get_current_position();
    
    if (!consume(TokenType::LEFT_BRACE)) {
        add_error("Expected '{'");
        return nullptr;
    }
    
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
    
    Position end = get_current_position();
    return std::make_unique<BlockStatement>(std::move(statements), start, end);
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
    
    auto consequent = parse_statement();
    if (!consequent) {
        add_error("Expected statement after if condition");
        return nullptr;
    }
    
    std::unique_ptr<ASTNode> alternate = nullptr;
    if (consume_if_match(TokenType::ELSE)) {
        alternate = parse_statement();
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
        if (match(TokenType::VAR) || match(TokenType::LET) || match(TokenType::CONST)) {
            
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

                goto check_for_of;
            }

            if (current_token().get_type() != TokenType::IDENTIFIER) {
                add_error("Expected identifier in variable declaration");
                return nullptr;
            }

            std::string var_name = current_token().get_value();
            Position var_start = current_token().get_start();
            Position var_end = current_token().get_end();
            advance();

            auto identifier = std::make_unique<Identifier>(var_name, var_start, var_end);

            std::unique_ptr<ASTNode> initializer = nullptr;
            if (current_token().get_type() == TokenType::ASSIGN) {
                advance();
                initializer = parse_assignment_expression();
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
            init = parse_postfix_expression();
            if (!init) {
                add_error("Expected initialization in for loop");
                return nullptr;
            }
        }
    }

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
        
        auto body = parse_statement();
        if (!body) {
            add_error("Failed to parse for...in body");
            return nullptr;
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

        auto body = parse_statement();
        if (!body) {
            add_error("Failed to parse for...in body");
            return nullptr;
        }

        Position end = get_current_position();
        return std::make_unique<ForInStatement>(std::move(init), std::move(object), std::move(body), start, end);
    }

    if (current_token().get_type() == TokenType::OF) {
        advance();
        
        if (at_end()) {
            add_error("Unexpected end of input after 'of'");
            return nullptr;
        }
        
        auto iterable = parse_expression();
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
        
        auto body = parse_statement();
        if (!body) {
            add_error("Failed to parse for...of body");
            return nullptr;
        }
        
        Position end = get_current_position();
        return std::make_unique<ForOfStatement>(std::move(init), std::move(iterable), std::move(body), is_await_loop, start, end);
    }
    
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
    
    auto body = parse_statement();
    if (!body) {
        add_error("Expected statement for for loop body");
        return nullptr;
    }
    
    Position end = get_current_position();
    return std::make_unique<ForStatement>(std::move(init), std::move(test), 
                                         std::move(update), std::move(body), start, end);
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
    
    auto body = parse_statement();
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
    
    auto body = parse_statement();
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

    if (current_token().get_type() == TokenType::IDENTIFIER && peek_token().get_type() == TokenType::COLON) {
        std::string label = current_token().get_value();
        advance();
        advance();

        auto statement = parse_statement();
        if (!statement) {
            add_error("Expected statement after label");
            return nullptr;
        }

        Position end = statement->get_end();
        return std::make_unique<LabeledStatement>(label, std::move(statement), start, end);
    }

    auto expr = parse_expression();
    if (!expr) {
        return nullptr;
    }

    start = expr->get_start();
    Position end = expr->get_end();

    if (match(TokenType::SEMICOLON)) {
        advance();
    } else if (at_end() || match(TokenType::RIGHT_BRACE) || current_token().get_start().line > end.line) {
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
    
    if (current_token().get_type() != TokenType::IDENTIFIER) {
        add_error("Expected function name");
        return nullptr;
    }
    
    auto id = std::make_unique<Identifier>(current_token().get_value(), 
                                         current_token().get_start(), current_token().get_end());
    advance();
    
    if (!consume(TokenType::LEFT_PAREN)) {
        add_error("Expected '(' after function name");
        return nullptr;
    }

    std::vector<std::unique_ptr<Parameter>> params;
    bool has_non_simple_params = false;

    while (!match(TokenType::RIGHT_PAREN) && !at_end()) {
        Position param_start = current_token().get_start();
        bool is_rest = false;

        if (match(TokenType::ELLIPSIS)) {
            is_rest = true;
            has_non_simple_params = true;
            advance();
        }

        std::unique_ptr<Identifier> param_name = nullptr;

        if (current_token().get_type() == TokenType::LEFT_BRACKET) {
            has_non_simple_params = true;
            advance();

            std::string destructuring_name = "__array_destructuring_";
            int bracket_count = 1;
            while (bracket_count > 0 && !at_end()) {
                if (current_token().get_type() == TokenType::LEFT_BRACKET) {
                    bracket_count++;
                } else if (current_token().get_type() == TokenType::RIGHT_BRACKET) {
                    bracket_count--;
                } else if (current_token().get_type() == TokenType::IDENTIFIER && bracket_count == 1) {
                    if (destructuring_name == "__array_destructuring_") {
                        destructuring_name += current_token().get_value();
                    }
                } else if (current_token().get_type() == TokenType::ASSIGN) {
                    advance();
                    if (current_token().get_type() == TokenType::LEFT_BRACKET) {
                        int inner_bracket_count = 1;
                        advance();
                        while (inner_bracket_count > 0 && !at_end()) {
                            if (current_token().get_type() == TokenType::LEFT_BRACKET) {
                                inner_bracket_count++;
                            } else if (current_token().get_type() == TokenType::RIGHT_BRACKET) {
                                inner_bracket_count--;
                            }
                            if (inner_bracket_count > 0) advance();
                        }
                        if (inner_bracket_count == 0) advance();
                        continue;
                    } else if (current_token().get_type() == TokenType::LEFT_BRACE) {
                        int inner_brace_count = 1;
                        advance();
                        while (inner_brace_count > 0 && !at_end()) {
                            if (current_token().get_type() == TokenType::LEFT_BRACE) {
                                inner_brace_count++;
                            } else if (current_token().get_type() == TokenType::RIGHT_BRACE) {
                                inner_brace_count--;
                            }
                            if (inner_brace_count > 0) advance();
                        }
                        if (inner_brace_count == 0) advance();
                        continue;
                    } else {
                        while (!at_end() &&
                               current_token().get_type() != TokenType::COMMA &&
                               current_token().get_type() != TokenType::RIGHT_BRACKET) {
                            advance();
                        }
                        continue;
                    }
                }
                advance();
            }

            Position param_pos = get_current_position();
            param_name = std::make_unique<Identifier>(destructuring_name, param_pos, param_pos);
        } else if (current_token().get_type() == TokenType::LEFT_BRACE) {
            has_non_simple_params = true;
            advance();

            std::string destructuring_name = "__destructuring_";
            int brace_count = 1;
            while (brace_count > 0 && !at_end()) {
                if (current_token().get_type() == TokenType::LEFT_BRACE) {
                    brace_count++;
                } else if (current_token().get_type() == TokenType::RIGHT_BRACE) {
                    brace_count--;
                } else if (current_token().get_type() == TokenType::IDENTIFIER && brace_count == 1) {
                    if (destructuring_name == "__destructuring_") {
                        destructuring_name += current_token().get_value();
                    }
                }
                advance();
            }

            Position param_pos = get_current_position();
            param_name = std::make_unique<Identifier>(destructuring_name, param_pos, param_pos);
        } else if (current_token().get_type() == TokenType::IDENTIFIER) {
            param_name = std::make_unique<Identifier>(current_token().get_value(),
                                                      current_token().get_start(), current_token().get_end());
            advance();
        } else {
            add_error("Expected parameter name or destructuring pattern");
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
        } else if (is_rest && match(TokenType::ASSIGN)) {
            add_error("Rest parameter cannot have default value");
            return nullptr;
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
        add_error("Expected ')' after parameters");
        return nullptr;
    }
    
    auto body = parse_block_statement();
    if (!body) {
        add_error("Expected function body");
        return nullptr;
    }
    
    if (has_non_simple_params && body) {
        BlockStatement* block = static_cast<BlockStatement*>(body.get());
        if (block && !block->get_statements().empty()) {
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
    
    Position end = get_current_position();
    return std::make_unique<FunctionDeclaration>(
        std::move(id), std::move(params), 
        std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body.release())),
        start, end, false, is_generator
    );
}

std::unique_ptr<ASTNode> Parser::parse_class_declaration() {
    Position start = get_current_position();
    
    if (!consume(TokenType::CLASS)) {
        add_error("Expected 'class'");
        return nullptr;
    }
    
    if (current_token().get_type() != TokenType::IDENTIFIER) {
        add_error("Expected class name");
        return nullptr;
    }
    
    auto id = parse_identifier();
    if (!id) return nullptr;
    
    std::unique_ptr<Identifier> superclass = nullptr;
    if (match(TokenType::EXTENDS)) {
        advance();
        
        if (current_token().get_type() != TokenType::IDENTIFIER) {
            add_error("Expected superclass name after 'extends'");
            return nullptr;
        }
        
        superclass = std::unique_ptr<Identifier>(static_cast<Identifier*>(parse_identifier().release()));
        if (!superclass) return nullptr;
    }
    
    if (!match(TokenType::LEFT_BRACE)) {
        add_error("Expected '{' to start class body");
        return nullptr;
    }
    
    advance();
    
    std::vector<std::unique_ptr<ASTNode>> statements;
    
    while (current_token().get_type() != TokenType::RIGHT_BRACE && !at_end()) {
        if (current_token().get_type() == TokenType::IDENTIFIER ||
            current_token().get_type() == TokenType::MULTIPLY ||
            current_token().get_type() == TokenType::LEFT_BRACKET ||
            current_token().get_type() == TokenType::HASH ||
            is_reserved_word_as_property_name()) {
            auto method = parse_method_definition();
            if (method) {
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
    
    advance();
    
    auto body = std::make_unique<BlockStatement>(std::move(statements), start, get_current_position());
    
    Position end = get_current_position();
    
    if (superclass) {
        return std::make_unique<ClassDeclaration>(
            std::unique_ptr<Identifier>(static_cast<Identifier*>(id.release())),
            std::move(superclass),
            std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body.release())),
            start, end
        );
    } else {
        return std::make_unique<ClassDeclaration>(
            std::unique_ptr<Identifier>(static_cast<Identifier*>(id.release())),
            std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body.release())),
            start, end
        );
    }
}

std::unique_ptr<ASTNode> Parser::parse_class_expression() {
    Position start = get_current_position();

    if (!consume(TokenType::CLASS)) {
        add_error("Expected 'class'");
        return nullptr;
    }

    std::unique_ptr<ASTNode> id = nullptr;
    if (current_token().get_type() == TokenType::IDENTIFIER) {
        id = parse_identifier();
        if (!id) return nullptr;
    }

    std::unique_ptr<Identifier> superclass = nullptr;
    if (match(TokenType::EXTENDS)) {
        advance();

        if (current_token().get_type() != TokenType::IDENTIFIER) {
            add_error("Expected superclass name after 'extends'");
            return nullptr;
        }

        superclass = std::unique_ptr<Identifier>(static_cast<Identifier*>(parse_identifier().release()));
        if (!superclass) return nullptr;
    }

    if (!match(TokenType::LEFT_BRACE)) {
        add_error("Expected '{' to start class body");
        return nullptr;
    }

    advance();

    std::vector<std::unique_ptr<ASTNode>> statements;

    while (current_token().get_type() != TokenType::RIGHT_BRACE && !at_end()) {
        if (current_token().get_type() == TokenType::IDENTIFIER ||
            current_token().get_type() == TokenType::MULTIPLY ||
            current_token().get_type() == TokenType::LEFT_BRACKET ||
            current_token().get_type() == TokenType::HASH ||
            is_reserved_word_as_property_name()) {
            auto method = parse_method_definition();
            if (method) {
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

    advance();

    auto body = std::make_unique<BlockStatement>(std::move(statements), start, get_current_position());

    Position end = get_current_position();

    if (id) {
        if (superclass) {
            return std::make_unique<ClassDeclaration>(
                std::unique_ptr<Identifier>(static_cast<Identifier*>(id.release())),
                std::move(superclass),
                std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body.release())),
                start, end
            );
        } else {
            return std::make_unique<ClassDeclaration>(
                std::unique_ptr<Identifier>(static_cast<Identifier*>(id.release())),
                std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body.release())),
                start, end
            );
        }
    } else {
        auto anonymous_id = std::make_unique<Identifier>("", start, start);
        if (superclass) {
            return std::make_unique<ClassDeclaration>(
                std::move(anonymous_id),
                std::move(superclass),
                std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body.release())),
                start, end
            );
        } else {
            return std::make_unique<ClassDeclaration>(
                std::move(anonymous_id),
                std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body.release())),
                start, end
            );
        }
    }
}

std::unique_ptr<ASTNode> Parser::parse_method_definition() {
    Position start = get_current_position();
    
    bool is_static = false;
    if (current_token().get_value() == "static") {
        is_static = true;
        advance();
    }

    bool is_async = false;
    if (current_token().get_type() == TokenType::ASYNC) {
        is_async = true;
        advance();
    }

    bool is_generator = false;
    if (current_token().get_type() == TokenType::MULTIPLY) {
        is_generator = true;
        advance();
    }

    MethodDefinition::Kind method_kind = MethodDefinition::METHOD;
    if (current_token().get_type() == TokenType::IDENTIFIER) {
        std::string token_value = current_token().get_value();
        if (token_value == "get") {
            method_kind = MethodDefinition::GETTER;
            advance();
        } else if (token_value == "set") {
            method_kind = MethodDefinition::SETTER;
            advance();
        }
    }

    std::unique_ptr<ASTNode> key = nullptr;
    bool computed = false;
    bool is_private = false;

    if (current_token().get_type() == TokenType::HASH) {
        is_private = true;
        advance();

        if (current_token().get_type() != TokenType::IDENTIFIER) {
            add_error("Expected identifier after '#' for private member");
            return nullptr;
        }

        std::string private_name = "#" + current_token().get_value();
        Position start = current_token().get_start();
        Position end = current_token().get_end();
        advance();
        key = std::make_unique<Identifier>(private_name, start, end);
    } else if (current_token().get_type() == TokenType::IDENTIFIER) {
        key = parse_identifier();
    } else if (is_reserved_word_as_property_name()) {
        std::string name = current_token().get_value();
        Position start = current_token().get_start();
        Position end = current_token().get_end();
        advance();
        key = std::make_unique<Identifier>(name, start, end);
    } else if (current_token().get_type() == TokenType::LEFT_BRACKET) {
        computed = true;
        advance();

        if (current_token().get_type() == TokenType::NUMBER) {
            key = parse_number_literal();
        } else if (current_token().get_type() == TokenType::STRING) {
            key = parse_string_literal();
        } else {
            key = parse_assignment_expression();
        }

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

    if (current_token().get_type() == TokenType::ASSIGN ||
        current_token().get_type() == TokenType::SEMICOLON ||
        current_token().get_type() == TokenType::RIGHT_BRACE) {

        std::unique_ptr<ASTNode> init = nullptr;

        if (current_token().get_type() == TokenType::ASSIGN) {
            advance();
            init = parse_assignment_expression();
            if (!init) {
                add_error("Expected field initializer after '='");
                return nullptr;
            }
        }

        if (current_token().get_type() == TokenType::SEMICOLON) {
            advance();
        }

        if (init) {
            auto this_id = std::make_unique<Identifier>("this", start, start);

            auto member_expr = std::make_unique<MemberExpression>(
                std::move(this_id),
                std::move(key),
                computed,
                start,
                get_current_position()
            );

            auto assignment = std::make_unique<AssignmentExpression>(
                std::move(member_expr),
                AssignmentExpression::Operator::ASSIGN,
                std::move(init),
                start,
                get_current_position()
            );
            return std::make_unique<ExpressionStatement>(std::move(assignment), start, get_current_position());
        } else {
            auto this_id = std::make_unique<Identifier>("this", start, start);
            auto member_expr = std::make_unique<MemberExpression>(
                std::move(this_id),
                std::move(key),
                computed,
                start,
                get_current_position()
            );
            auto undefined_val = std::make_unique<Identifier>("undefined", start, start);
            auto assignment = std::make_unique<AssignmentExpression>(
                std::move(member_expr),
                AssignmentExpression::Operator::ASSIGN,
                std::move(undefined_val),
                start,
                get_current_position()
            );
            return std::make_unique<ExpressionStatement>(std::move(assignment), start, get_current_position());
        }
    }

    MethodDefinition::Kind kind = method_kind;
    if (Identifier* key_id = static_cast<Identifier*>(key.get())) {
        if (key_id->get_name() == "constructor") {
            kind = MethodDefinition::CONSTRUCTOR;
        }
    }

    if (current_token().get_type() != TokenType::LEFT_PAREN) {
        add_error("Expected '(' for method parameters");
        return nullptr;
    }
    
    advance();
    
    std::vector<std::unique_ptr<Parameter>> params;
    while (current_token().get_type() != TokenType::RIGHT_PAREN && !at_end()) {
        Position param_start = get_current_position();
        bool is_rest = false;
        
        if (match(TokenType::ELLIPSIS)) {
            is_rest = true;
            advance();
        }
        
        std::unique_ptr<Identifier> param_name = nullptr;

        if (current_token().get_type() == TokenType::LEFT_BRACKET) {
            advance();

            std::string destructuring_name = "__array_destructuring_";
            int bracket_count = 1;
            while (bracket_count > 0 && !at_end()) {
                if (current_token().get_type() == TokenType::LEFT_BRACKET) {
                    bracket_count++;
                } else if (current_token().get_type() == TokenType::RIGHT_BRACKET) {
                    bracket_count--;
                } else if (current_token().get_type() == TokenType::IDENTIFIER && bracket_count == 1) {
                    if (destructuring_name == "__array_destructuring_") {
                        destructuring_name += current_token().get_value();
                    }
                } else if (current_token().get_type() == TokenType::ASSIGN) {
                    advance();
                    if (current_token().get_type() == TokenType::LEFT_BRACKET) {
                        int inner_bracket_count = 1;
                        advance();
                        while (inner_bracket_count > 0 && !at_end()) {
                            if (current_token().get_type() == TokenType::LEFT_BRACKET) {
                                inner_bracket_count++;
                            } else if (current_token().get_type() == TokenType::RIGHT_BRACKET) {
                                inner_bracket_count--;
                            }
                            if (inner_bracket_count > 0) advance();
                        }
                        if (inner_bracket_count == 0) advance();
                        continue;
                    } else if (current_token().get_type() == TokenType::LEFT_BRACE) {
                        int inner_brace_count = 1;
                        advance();
                        while (inner_brace_count > 0 && !at_end()) {
                            if (current_token().get_type() == TokenType::LEFT_BRACE) {
                                inner_brace_count++;
                            } else if (current_token().get_type() == TokenType::RIGHT_BRACE) {
                                inner_brace_count--;
                            }
                            if (inner_brace_count > 0) advance();
                        }
                        if (inner_brace_count == 0) advance();
                        continue;
                    } else {
                        while (!at_end() &&
                               current_token().get_type() != TokenType::COMMA &&
                               current_token().get_type() != TokenType::RIGHT_BRACKET) {
                            advance();
                        }
                        continue;
                    }
                }
                advance();
            }

            Position param_pos = get_current_position();
            param_name = std::make_unique<Identifier>(destructuring_name, param_pos, param_pos);
        } else if (current_token().get_type() == TokenType::LEFT_BRACE) {
            advance();

            std::string destructuring_name = "__destructuring_";
            int brace_count = 1;
            while (brace_count > 0 && !at_end()) {
                if (current_token().get_type() == TokenType::LEFT_BRACE) {
                    brace_count++;
                } else if (current_token().get_type() == TokenType::RIGHT_BRACE) {
                    brace_count--;
                } else if (current_token().get_type() == TokenType::IDENTIFIER && brace_count == 1) {
                    if (destructuring_name == "__destructuring_") {
                        destructuring_name += current_token().get_value();
                    }
                }
                advance();
            }

            Position param_pos = get_current_position();
            param_name = std::make_unique<Identifier>(destructuring_name, param_pos, param_pos);
        } else if (current_token().get_type() == TokenType::IDENTIFIER) {
            param_name = std::make_unique<Identifier>(current_token().get_value(),
                                                      current_token().get_start(), current_token().get_end());
            advance();
        } else {
            add_error("Expected parameter name or destructuring pattern");
            return nullptr;
        }

        std::unique_ptr<ASTNode> default_value = nullptr;
        if (!is_rest && match(TokenType::ASSIGN)) {
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
    
    if (!match(TokenType::RIGHT_PAREN)) {
        add_error("Expected ')' after parameters");
        return nullptr;
    }
    advance();
    
    if (current_token().get_type() != TokenType::LEFT_BRACE) {
        add_error("Expected '{' for method body");
        return nullptr;
    }
    
    auto body = parse_block_statement();
    if (!body) {
        add_error("Expected method body");
        return nullptr;
    }
    
    auto function_expr = std::make_unique<FunctionExpression>(
        nullptr,
        std::move(params),
        std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body.release())),
        start, get_current_position()
    );
    
    Position end = get_current_position();
    return std::make_unique<MethodDefinition>(
        std::move(key),
        std::move(function_expr),
        kind, is_static, computed, start, end
    );
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
        id = std::make_unique<Identifier>(current_token().get_value(),
                                        current_token().get_start(), current_token().get_end());
        advance();
    }

    if (!consume(TokenType::LEFT_PAREN)) {
        add_error("Expected '(' after 'function'");
        return nullptr;
    }

    std::vector<std::unique_ptr<Parameter>> params;
    bool has_non_simple_params = false;

    while (!match(TokenType::RIGHT_PAREN) && !at_end()) {
        Position param_start = get_current_position();
        bool is_rest = false;

        if (match(TokenType::ELLIPSIS)) {
            is_rest = true;
            has_non_simple_params = true;
            advance();
        }

        std::unique_ptr<Identifier> param_name = nullptr;

        if (current_token().get_type() == TokenType::LEFT_BRACKET) {
            has_non_simple_params = true;
            advance();

            std::string destructuring_name = "__array_destructuring_";
            int bracket_count = 1;
            while (bracket_count > 0 && !at_end()) {
                if (current_token().get_type() == TokenType::LEFT_BRACKET) {
                    bracket_count++;
                } else if (current_token().get_type() == TokenType::RIGHT_BRACKET) {
                    bracket_count--;
                } else if (current_token().get_type() == TokenType::IDENTIFIER && bracket_count == 1) {
                    if (destructuring_name == "__array_destructuring_") {
                        destructuring_name += current_token().get_value();
                    }
                } else if (current_token().get_type() == TokenType::ASSIGN) {
                    advance();
                    if (current_token().get_type() == TokenType::LEFT_BRACKET) {
                        int inner_bracket_count = 1;
                        advance();
                        while (inner_bracket_count > 0 && !at_end()) {
                            if (current_token().get_type() == TokenType::LEFT_BRACKET) {
                                inner_bracket_count++;
                            } else if (current_token().get_type() == TokenType::RIGHT_BRACKET) {
                                inner_bracket_count--;
                            }
                            if (inner_bracket_count > 0) advance();
                        }
                        if (inner_bracket_count == 0) advance();
                        continue;
                    } else if (current_token().get_type() == TokenType::LEFT_BRACE) {
                        int inner_brace_count = 1;
                        advance();
                        while (inner_brace_count > 0 && !at_end()) {
                            if (current_token().get_type() == TokenType::LEFT_BRACE) {
                                inner_brace_count++;
                            } else if (current_token().get_type() == TokenType::RIGHT_BRACE) {
                                inner_brace_count--;
                            }
                            if (inner_brace_count > 0) advance();
                        }
                        if (inner_brace_count == 0) advance();
                        continue;
                    } else {
                        while (!at_end() &&
                               current_token().get_type() != TokenType::COMMA &&
                               current_token().get_type() != TokenType::RIGHT_BRACKET) {
                            advance();
                        }
                        continue;
                    }
                }
                advance();
            }

            Position param_pos = get_current_position();
            param_name = std::make_unique<Identifier>(destructuring_name, param_pos, param_pos);
        } else if (current_token().get_type() == TokenType::LEFT_BRACE) {
            has_non_simple_params = true;
            advance();

            std::string destructuring_name = "__destructuring_";
            int brace_count = 1;
            while (brace_count > 0 && !at_end()) {
                if (current_token().get_type() == TokenType::LEFT_BRACE) {
                    brace_count++;
                } else if (current_token().get_type() == TokenType::RIGHT_BRACE) {
                    brace_count--;
                } else if (current_token().get_type() == TokenType::IDENTIFIER && brace_count == 1) {
                    if (destructuring_name == "__destructuring_") {
                        destructuring_name += current_token().get_value();
                    }
                }
                advance();
            }

            Position param_pos = get_current_position();
            param_name = std::make_unique<Identifier>(destructuring_name, param_pos, param_pos);
        } else if (current_token().get_type() == TokenType::IDENTIFIER) {
            param_name = std::make_unique<Identifier>(current_token().get_value(),
                                                      current_token().get_start(), current_token().get_end());
            advance();
        } else {
            add_error("Expected parameter name or destructuring pattern");
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
        } else if (is_rest && match(TokenType::ASSIGN)) {
            add_error("Rest parameter cannot have default value");
            return nullptr;
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
        add_error("Expected ')' after parameters");
        return nullptr;
    }
    
    auto body = parse_block_statement();
    if (!body) {
        add_error("Expected function body");
        return nullptr;
    }
    
    if (has_non_simple_params && body) {
        BlockStatement* block = static_cast<BlockStatement*>(body.get());
        if (block && !block->get_statements().empty()) {
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
    
    Position end = get_current_position();
    return std::make_unique<FunctionExpression>(
        std::move(id), std::move(params),
        std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body.release())),
        start, end
    );
}

std::unique_ptr<ASTNode> Parser::parse_async_function_expression() {
    Position start = get_current_position();

    if (!consume(TokenType::ASYNC)) {
        add_error("Expected 'async'");
        return nullptr;
    }

    if (match(TokenType::FUNCTION)) {
        advance();
    } else if (match(TokenType::LEFT_PAREN)) {
        return parse_async_arrow_function(start);
    } else if (match(TokenType::IDENTIFIER)) {
        return parse_async_arrow_function_single_param(start);
    } else {
        add_error("Expected 'function', '(', or identifier after 'async'");
        return nullptr;
    }

    bool is_generator = false;
    if (current_token().get_type() == TokenType::MULTIPLY) {
        advance();
        is_generator = true;
    }

    std::unique_ptr<Identifier> id = nullptr;
    if (current_token().get_type() == TokenType::IDENTIFIER) {
        id = std::make_unique<Identifier>(current_token().get_value(),
                                        current_token().get_start(), current_token().get_end());
        advance();
    }
    
    if (!consume(TokenType::LEFT_PAREN)) {
        add_error("Expected '(' after async function name");
        return nullptr;
    }

    std::vector<std::unique_ptr<Parameter>> params;
    bool has_non_simple_params = false;

    while (!match(TokenType::RIGHT_PAREN) && !at_end()) {
        Position param_start = current_token().get_start();
        bool is_rest = false;

        if (match(TokenType::ELLIPSIS)) {
            is_rest = true;
            has_non_simple_params = true;
            advance();
        }

        std::unique_ptr<Identifier> param_name = nullptr;

        if (current_token().get_type() == TokenType::LEFT_BRACKET) {
            has_non_simple_params = true;
            advance();

            std::string destructuring_name = "__array_destructuring_";
            int bracket_count = 1;
            while (bracket_count > 0 && !at_end()) {
                if (current_token().get_type() == TokenType::LEFT_BRACKET) {
                    bracket_count++;
                } else if (current_token().get_type() == TokenType::RIGHT_BRACKET) {
                    bracket_count--;
                } else if (current_token().get_type() == TokenType::IDENTIFIER && bracket_count == 1) {
                    if (destructuring_name == "__array_destructuring_") {
                        destructuring_name += current_token().get_value();
                    }
                } else if (current_token().get_type() == TokenType::ASSIGN) {
                    advance();
                    if (current_token().get_type() == TokenType::LEFT_BRACKET) {
                        int inner_bracket_count = 1;
                        advance();
                        while (inner_bracket_count > 0 && !at_end()) {
                            if (current_token().get_type() == TokenType::LEFT_BRACKET) {
                                inner_bracket_count++;
                            } else if (current_token().get_type() == TokenType::RIGHT_BRACKET) {
                                inner_bracket_count--;
                            }
                            if (inner_bracket_count > 0) advance();
                        }
                        if (inner_bracket_count == 0) advance();
                        continue;
                    } else if (current_token().get_type() == TokenType::LEFT_BRACE) {
                        int inner_brace_count = 1;
                        advance();
                        while (inner_brace_count > 0 && !at_end()) {
                            if (current_token().get_type() == TokenType::LEFT_BRACE) {
                                inner_brace_count++;
                            } else if (current_token().get_type() == TokenType::RIGHT_BRACE) {
                                inner_brace_count--;
                            }
                            if (inner_brace_count > 0) advance();
                        }
                        if (inner_brace_count == 0) advance();
                        continue;
                    } else {
                        while (!at_end() &&
                               current_token().get_type() != TokenType::COMMA &&
                               current_token().get_type() != TokenType::RIGHT_BRACKET) {
                            advance();
                        }
                        continue;
                    }
                }
                advance();
            }

            Position param_pos = get_current_position();
            param_name = std::make_unique<Identifier>(destructuring_name, param_pos, param_pos);
        } else if (current_token().get_type() == TokenType::LEFT_BRACE) {
            has_non_simple_params = true;
            advance();

            std::string destructuring_name = "__destructuring_";
            int brace_count = 1;
            while (brace_count > 0 && !at_end()) {
                if (current_token().get_type() == TokenType::LEFT_BRACE) {
                    brace_count++;
                } else if (current_token().get_type() == TokenType::RIGHT_BRACE) {
                    brace_count--;
                } else if (current_token().get_type() == TokenType::IDENTIFIER && brace_count == 1) {
                    if (destructuring_name == "__destructuring_") {
                        destructuring_name += current_token().get_value();
                    }
                }
                advance();
            }

            Position param_pos = get_current_position();
            param_name = std::make_unique<Identifier>(destructuring_name, param_pos, param_pos);
        } else if (current_token().get_type() == TokenType::IDENTIFIER) {
            param_name = std::make_unique<Identifier>(current_token().get_value(),
                                                      current_token().get_start(), current_token().get_end());
            advance();
        } else {
            add_error("Expected parameter name or destructuring pattern");
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
        } else if (is_rest && match(TokenType::ASSIGN)) {
            add_error("Rest parameter cannot have default value");
            return nullptr;
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
        add_error("Expected ')' after parameters");
        return nullptr;
    }
    
    auto body = parse_block_statement();
    if (!body) {
        add_error("Expected async function body");
        return nullptr;
    }
    
    Position end = get_current_position();
    return std::make_unique<AsyncFunctionExpression>(
        std::move(id), std::move(params),
        std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body.release())),
        start, end
    );
}

std::unique_ptr<ASTNode> Parser::parse_async_function_declaration() {
    Position start = get_current_position();
    
    if (!consume(TokenType::ASYNC)) {
        add_error("Expected 'async'");
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

    if (current_token().get_type() != TokenType::IDENTIFIER) {
        add_error("Expected function name after 'async function'");
        return nullptr;
    }
    
    auto id = std::make_unique<Identifier>(current_token().get_value(),
                                        current_token().get_start(), current_token().get_end());
    advance();

    if (!consume(TokenType::LEFT_PAREN)) {
        add_error("Expected '(' after async function name");
        return nullptr;
    }

    std::vector<std::unique_ptr<Parameter>> params;
    bool has_non_simple_params = false;

    while (!match(TokenType::RIGHT_PAREN) && !at_end()) {
        Position param_start = current_token().get_start();
        bool is_rest = false;

        if (match(TokenType::ELLIPSIS)) {
            is_rest = true;
            has_non_simple_params = true;
            advance();
        }

        std::unique_ptr<Identifier> param_name = nullptr;

        if (current_token().get_type() == TokenType::LEFT_BRACKET) {
            has_non_simple_params = true;
            advance();

            std::string destructuring_name = "__array_destructuring_";
            int bracket_count = 1;
            while (bracket_count > 0 && !at_end()) {
                if (current_token().get_type() == TokenType::LEFT_BRACKET) {
                    bracket_count++;
                } else if (current_token().get_type() == TokenType::RIGHT_BRACKET) {
                    bracket_count--;
                } else if (current_token().get_type() == TokenType::IDENTIFIER && bracket_count == 1) {
                    if (destructuring_name == "__array_destructuring_") {
                        destructuring_name += current_token().get_value();
                    }
                } else if (current_token().get_type() == TokenType::ASSIGN) {
                    advance();
                    if (current_token().get_type() == TokenType::LEFT_BRACKET) {
                        int inner_bracket_count = 1;
                        advance();
                        while (inner_bracket_count > 0 && !at_end()) {
                            if (current_token().get_type() == TokenType::LEFT_BRACKET) {
                                inner_bracket_count++;
                            } else if (current_token().get_type() == TokenType::RIGHT_BRACKET) {
                                inner_bracket_count--;
                            }
                            if (inner_bracket_count > 0) advance();
                        }
                        if (inner_bracket_count == 0) advance();
                        continue;
                    } else if (current_token().get_type() == TokenType::LEFT_BRACE) {
                        int inner_brace_count = 1;
                        advance();
                        while (inner_brace_count > 0 && !at_end()) {
                            if (current_token().get_type() == TokenType::LEFT_BRACE) {
                                inner_brace_count++;
                            } else if (current_token().get_type() == TokenType::RIGHT_BRACE) {
                                inner_brace_count--;
                            }
                            if (inner_brace_count > 0) advance();
                        }
                        if (inner_brace_count == 0) advance();
                        continue;
                    } else {
                        while (!at_end() &&
                               current_token().get_type() != TokenType::COMMA &&
                               current_token().get_type() != TokenType::RIGHT_BRACKET) {
                            advance();
                        }
                        continue;
                    }
                }
                advance();
            }

            Position param_pos = get_current_position();
            param_name = std::make_unique<Identifier>(destructuring_name, param_pos, param_pos);
        } else if (current_token().get_type() == TokenType::LEFT_BRACE) {
            has_non_simple_params = true;
            advance();

            std::string destructuring_name = "__destructuring_";
            int brace_count = 1;
            while (brace_count > 0 && !at_end()) {
                if (current_token().get_type() == TokenType::LEFT_BRACE) {
                    brace_count++;
                } else if (current_token().get_type() == TokenType::RIGHT_BRACE) {
                    brace_count--;
                } else if (current_token().get_type() == TokenType::IDENTIFIER && brace_count == 1) {
                    if (destructuring_name == "__destructuring_") {
                        destructuring_name += current_token().get_value();
                    }
                }
                advance();
            }

            Position param_pos = get_current_position();
            param_name = std::make_unique<Identifier>(destructuring_name, param_pos, param_pos);
        } else if (current_token().get_type() == TokenType::IDENTIFIER) {
            param_name = std::make_unique<Identifier>(current_token().get_value(),
                                                      current_token().get_start(), current_token().get_end());
            advance();
        } else {
            add_error("Expected parameter name or destructuring pattern");
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
        } else if (is_rest && match(TokenType::ASSIGN)) {
            add_error("Rest parameter cannot have default value");
            return nullptr;
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
        add_error("Expected ')' after parameters");
        return nullptr;
    }
    
    auto body = parse_block_statement();
    if (!body) {
        add_error("Expected async function body");
        return nullptr;
    }
    
    Position end = get_current_position();
    return std::make_unique<FunctionDeclaration>(
        std::move(id), std::move(params),
        std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body.release())),
        start, end, true, false
    );
}

std::unique_ptr<ASTNode> Parser::parse_arrow_function() {
    Position start = get_current_position();
    std::vector<std::unique_ptr<Parameter>> params;
    bool has_non_simple_params = false;
    
    if (match(TokenType::IDENTIFIER)) {
        Position param_start = get_current_position();
        auto param_name = std::make_unique<Identifier>(current_token().get_value(),
                                                      current_token().get_start(), current_token().get_end());
        advance();
        
        Position param_end = get_current_position();
        auto param = std::make_unique<Parameter>(std::move(param_name), nullptr, false, param_start, param_end);
        params.push_back(std::move(param));
    } else if (match(TokenType::LEFT_PAREN)) {
        advance();
        
        while (!match(TokenType::RIGHT_PAREN) && !at_end()) {
            if (current_token().get_type() == TokenType::IDENTIFIER) {
            } else if (current_token().get_type() == TokenType::LEFT_BRACE) {
                has_non_simple_params = true;
                advance();
                int brace_count = 1;
                while (brace_count > 0 && !at_end()) {
                    if (current_token().get_type() == TokenType::LEFT_BRACE) {
                        brace_count++;
                    } else if (current_token().get_type() == TokenType::RIGHT_BRACE) {
                        brace_count--;
                    }
                    advance();
                }
                Position param_start = get_current_position();
                auto param_name = std::make_unique<Identifier>("__destructured", param_start, param_start);
                Position param_end = get_current_position();
                auto param = std::make_unique<Parameter>(std::move(param_name), nullptr, false, param_start, param_end);
                params.push_back(std::move(param));
                
                if (match(TokenType::COMMA)) {
                    advance();
                }
                continue;
            } else if (current_token().get_type() == TokenType::LEFT_BRACKET) {
                has_non_simple_params = true;
                advance();
                int bracket_count = 1;
                while (bracket_count > 0 && !at_end()) {
                    if (current_token().get_type() == TokenType::LEFT_BRACKET) {
                        bracket_count++;
                    } else if (current_token().get_type() == TokenType::RIGHT_BRACKET) {
                        bracket_count--;
                    }
                    advance();
                }
                Position param_start = get_current_position();
                auto param_name = std::make_unique<Identifier>("__destructured", param_start, param_start);
                Position param_end = get_current_position();
                auto param = std::make_unique<Parameter>(std::move(param_name), nullptr, false, param_start, param_end);
                params.push_back(std::move(param));
                
                if (match(TokenType::COMMA)) {
                    advance();
                }
                continue;
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
        
        if (!consume(TokenType::RIGHT_PAREN)) {
            add_error("Expected ')' after arrow function parameters");
            return nullptr;
        }
    }
    
    if (!consume(TokenType::ARROW)) {
        add_error("Expected '=>' in arrow function");
        return nullptr;
    }
    
    std::unique_ptr<ASTNode> body;
    if (match(TokenType::LEFT_BRACE)) {
        body = parse_block_statement();
    } else {
        body = parse_assignment_expression();
    }
    
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
    return std::make_unique<ArrowFunctionExpression>(
        std::move(params), std::move(body), false, start, end
    );
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
    
    if (!consume(TokenType::YIELD)) {
        add_error("Expected 'yield'");
        return nullptr;
    }
    
    bool is_delegate = false;
    if (match(TokenType::MULTIPLY)) {
        is_delegate = true;
        advance();
    }
    
    std::unique_ptr<ASTNode> argument = nullptr;
    if (!at_end() && current_token().get_type() != TokenType::SEMICOLON &&
        current_token().get_type() != TokenType::RIGHT_BRACE &&
        current_token().get_type() != TokenType::RIGHT_PAREN) {
        argument = parse_assignment_expression();
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

    if (!consume(TokenType::LEFT_PAREN)) {
        add_error("Expected '(' after 'import'");
        return nullptr;
    }

    auto specifier = parse_assignment_expression();
    if (!specifier) {
        add_error("Expected module specifier in import()");
        return nullptr;
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

    std::string label;
    if (current_token().get_type() == TokenType::IDENTIFIER &&
        current_token().get_start().line == start.line) {
        label = current_token().get_value();
        advance();
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

    std::string label;
    if (current_token().get_type() == TokenType::IDENTIFIER &&
        current_token().get_start().line == start.line) {
        label = current_token().get_value();
        advance();
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
    Lexer lexer(source);
    TokenSequence tokens = lexer.tokenize();
    Parser::ParseOptions options;
    options.source_type_module = true;
    return std::make_unique<Parser>(std::move(tokens), options);
}

}

std::unique_ptr<ASTNode> Parser::parse_object_literal() {
    Position start = get_current_position();
    
    if (!consume(TokenType::LEFT_BRACE)) {
        add_error("Expected '{'");
        return nullptr;
    }
    
    std::vector<std::unique_ptr<ObjectLiteral::Property>> properties;
    
    if (match(TokenType::RIGHT_BRACE)) {
        advance();
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
        
        ObjectLiteral::PropertyType property_type = ObjectLiteral::PropertyType::Value;
        bool is_async = false;

        if (match(TokenType::IDENTIFIER)) {
            if (current_token().get_value() == "get") {
                size_t saved_pos = current_token_index_;
                advance();

                bool is_method_shorthand = match(TokenType::LEFT_PAREN);
                bool is_getter_syntax = !is_method_shorthand && (match(TokenType::LEFT_BRACKET) || match(TokenType::IDENTIFIER) || match(TokenType::STRING) || match(TokenType::NUMBER));
                bool is_normal_property = match(TokenType::COLON);

                current_token_index_ = saved_pos;

                if (is_getter_syntax && !is_normal_property) {
                    property_type = ObjectLiteral::PropertyType::Getter;
                    advance();
                }
            } else if (current_token().get_value() == "set") {
                size_t saved_pos = current_token_index_;
                advance();

                bool is_method_shorthand = match(TokenType::LEFT_PAREN);
                bool is_setter_syntax = !is_method_shorthand && (match(TokenType::LEFT_BRACKET) || match(TokenType::IDENTIFIER) || match(TokenType::STRING) || match(TokenType::NUMBER));
                bool is_normal_property = match(TokenType::COLON);

                current_token_index_ = saved_pos;

                if (is_setter_syntax && !is_normal_property) {
                    property_type = ObjectLiteral::PropertyType::Setter;
                    advance();
                }
            } else if (current_token().get_value() == "async") {
                is_async = true;
                advance();
            }
        } else if (match(TokenType::ASYNC)) {
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
            key = parse_identifier();
        } else if (match(TokenType::STRING)) {
            key = parse_string_literal();
        } else if (match(TokenType::NUMBER)) {
            key = parse_number_literal();
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
            if (!match(TokenType::RIGHT_PAREN)) {
                do {
                    Position param_start = current_token().get_start();
                    bool is_rest = false;

                    if (match(TokenType::ELLIPSIS)) {
                        is_rest = true;
                        advance();
                    }

                    std::unique_ptr<Identifier> param_name = nullptr;

                    if (current_token().get_type() == TokenType::LEFT_BRACKET) {
                        advance();

                        std::string destructuring_name = "__array_destructuring_";
                        int bracket_count = 1;
                        while (bracket_count > 0 && !at_end()) {
                            if (current_token().get_type() == TokenType::LEFT_BRACKET) {
                                bracket_count++;
                            } else if (current_token().get_type() == TokenType::RIGHT_BRACKET) {
                                bracket_count--;
                            } else if (current_token().get_type() == TokenType::IDENTIFIER && bracket_count == 1) {
                                if (destructuring_name == "__array_destructuring_") {
                                    destructuring_name += current_token().get_value();
                                }
                            }
                            advance();
                        }

                        Position param_pos = get_current_position();
                        param_name = std::make_unique<Identifier>(destructuring_name, param_pos, param_pos);
                    } else if (current_token().get_type() == TokenType::LEFT_BRACE) {
                        advance();

                        std::string destructuring_name = "__destructuring_";
                        int brace_count = 1;
                        while (brace_count > 0 && !at_end()) {
                            if (current_token().get_type() == TokenType::LEFT_BRACE) {
                                brace_count++;
                            } else if (current_token().get_type() == TokenType::RIGHT_BRACE) {
                                brace_count--;
                            } else if (current_token().get_type() == TokenType::IDENTIFIER && brace_count == 1) {
                                if (destructuring_name == "__destructuring_") {
                                    destructuring_name += current_token().get_value();
                                }
                            }
                            advance();
                        }

                        Position param_pos = get_current_position();
                        param_name = std::make_unique<Identifier>(destructuring_name, param_pos, param_pos);
                    } else if (current_token().get_type() == TokenType::IDENTIFIER) {
                        param_name = std::make_unique<Identifier>(
                            current_token().get_value(),
                            current_token().get_start(),
                            current_token().get_end()
                        );
                        advance();
                    } else {
                        add_error("Expected parameter name or destructuring pattern");
                        return nullptr;
                    }

                    std::unique_ptr<ASTNode> default_value = nullptr;
                    if (match(TokenType::ASSIGN)) {
                        advance();
                        default_value = parse_assignment_expression();
                        if (!default_value) {
                            add_error("Expected expression after '=' in parameter default");
                            return nullptr;
                        }
                    }

                    Position param_end = get_current_position();
                    auto param = std::make_unique<Parameter>(std::move(param_name), std::move(default_value), is_rest, param_start, param_end);
                    params.push_back(std::move(param));

                    if (match(TokenType::COMMA)) {
                        advance();
                    } else {
                        break;
                    }
                } while (!at_end());
            }
            
            if (!consume(TokenType::RIGHT_PAREN)) {
                add_error("Expected ')' after parameters");
                return nullptr;
            }
            
            if (!match(TokenType::LEFT_BRACE)) {
                add_error("Expected '{' for method body");
                return nullptr;
            }
            
            auto body = parse_block_statement();
            if (!body) {
                add_error("Expected method body");
                return nullptr;
            }
            
            std::unique_ptr<ASTNode> method_value;
            if (is_async) {
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
                    get_current_position()
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
                
                if (auto* identifier_key = dynamic_cast<Identifier*>(key.get())) {
                    auto value = std::make_unique<Identifier>(
                        identifier_key->get_name(),
                        identifier_key->get_start(),
                        identifier_key->get_end()
                    );
                    
                    auto property = std::make_unique<ObjectLiteral::Property>(
                        std::move(key), std::move(value), computed, false
                    );
                    properties.push_back(std::move(property));
                } else {
                    add_error("Shorthand properties can only be used with identifier keys");
                    return nullptr;
                }
            } else {
                if (!consume(TokenType::COLON)) {
                    add_error("Expected ':' after property key");
                    return nullptr;
                }
                
                auto value = parse_assignment_expression();
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
    
    Position end = get_current_position();
    return std::make_unique<ObjectLiteral>(std::move(properties), start, end);
}

std::unique_ptr<ASTNode> Parser::parse_array_literal() {
    Position start = get_current_position();
    
    if (!consume(TokenType::LEFT_BRACKET)) {
        add_error("Expected '['");
        return nullptr;
    }
    
    std::vector<std::unique_ptr<ASTNode>> elements;
    
    if (match(TokenType::RIGHT_BRACKET)) {
        advance();
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
                    return nullptr;
                }
                elements.push_back(std::move(spread));
            } else {
                auto element = parse_assignment_expression();
                if (!element) {
                    add_error("Expected array element");
                    return nullptr;
                }
                elements.push_back(std::move(element));
            }
        }
        
        if (match(TokenType::COMMA)) {
            advance();
            if (match(TokenType::RIGHT_BRACKET)) {
                break;
            }
        } else {
            break;
        }
        
    } while (!at_end() && !match(TokenType::RIGHT_BRACKET));
    
    if (!consume(TokenType::RIGHT_BRACKET)) {
        add_error("Expected ']' to close array literal");
        return nullptr;
    }
    
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
    
    if (!consume(TokenType::LEFT_PAREN)) {
        add_error("Expected '(' after 'catch'");
        return nullptr;
    }
    
    if (!match(TokenType::IDENTIFIER)) {
        add_error("Expected identifier in catch clause");
        return nullptr;
    }
    
    std::string parameter_name = current_token().get_value();
    advance();
    
    if (!consume(TokenType::RIGHT_PAREN)) {
        add_error("Expected ')' after catch parameter");
        return nullptr;
    }
    
    auto body = parse_block_statement();
    if (!body) {
        add_error("Expected block statement in catch clause");
        return nullptr;
    }
    
    Position end = get_current_position();
    return std::make_unique<CatchClause>(parameter_name, std::move(body), start, end);
}

std::unique_ptr<ASTNode> Parser::parse_throw_statement() {
    Position start = current_token().get_start();
    advance();
    
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
    
    std::vector<std::unique_ptr<ASTNode>> cases;
    
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
            while (!match(TokenType::CASE) && !match(TokenType::DEFAULT) && 
                   !match(TokenType::RIGHT_BRACE) && !at_end()) {
                auto stmt = parse_statement();
                if (stmt) {
                    consequent.push_back(std::move(stmt));
                } else {
                    break;
                }
            }
            
            Position case_end = get_current_position();
            cases.push_back(std::make_unique<CaseClause>(
                std::move(test), std::move(consequent), case_start, case_end
            ));
            
        } else if (match(TokenType::DEFAULT)) {
            Position default_start = current_token().get_start();
            advance();
            
            if (!consume(TokenType::COLON)) {
                add_error("Expected ':' after 'default'");
                return nullptr;
            }
            
            std::vector<std::unique_ptr<ASTNode>> consequent;
            while (!match(TokenType::CASE) && !match(TokenType::DEFAULT) && 
                   !match(TokenType::RIGHT_BRACE) && !at_end()) {
                auto stmt = parse_statement();
                if (stmt) {
                    consequent.push_back(std::move(stmt));
                } else {
                    break;
                }
            }
            
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
    
    Position end = get_current_position();
    return std::make_unique<SwitchStatement>(
        std::move(discriminant), std::move(cases), start, end
    );
}


std::unique_ptr<ASTNode> Parser::parse_import_statement() {
    Position start = current_token().get_start();
    advance();
    
    if (match(TokenType::MULTIPLY)) {
        advance();
        
        if (current_token().get_type() != TokenType::IDENTIFIER || current_token().get_value() != "as") {
            add_error("Expected 'as' after '*' in import statement");
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
        
        Position end = get_current_position();
        return std::make_unique<ImportStatement>(std::move(specifiers), module_source, start, end);
    }
    
    if (match(TokenType::IDENTIFIER)) {
        std::string default_alias = current_token().get_value();
        advance();
        
        if (match(TokenType::COMMA)) {
            advance();
            
            if (!match(TokenType::LEFT_BRACE)) {
                add_error("Expected '{' after ',' in mixed import statement");
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
        
        Position end = get_current_position();
        return std::make_unique<ImportStatement>(default_alias, module_source, true, start, end);
    }
    
    add_error("Invalid import statement syntax");
    return nullptr;
}

std::unique_ptr<ASTNode> Parser::parse_export_statement() {
    Position start = current_token().get_start();
    advance();
    
    if (match(TokenType::DEFAULT)) {
        advance();
        
        auto default_export = parse_assignment_expression();
        if (!default_export) {
            add_error("Expected expression after 'export default'");
            return nullptr;
        }
        
        Position end = get_current_position();
        return std::make_unique<ExportStatement>(std::move(default_export), true, start, end);
    }
    
    if (match(TokenType::LEFT_BRACE)) {
        advance();
        
        std::vector<std::unique_ptr<ExportSpecifier>> specifiers;
        
        while (!match(TokenType::RIGHT_BRACE) && !at_end()) {
            auto specifier = parse_export_specifier();
            if (specifier) {
                specifiers.push_back(std::move(specifier));
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
            advance();
            
            Position end = get_current_position();
            return std::make_unique<ExportStatement>(std::move(specifiers), source_module, start, end);
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
    
    if (!match(TokenType::IDENTIFIER) && !match(TokenType::DEFAULT)) {
        add_error("Expected identifier or 'default' in import specifier");
        return nullptr;
    }
    
    std::string imported_name = current_token().get_value();
    std::string local_name = imported_name;
    advance();
    
    if (match(TokenType::IDENTIFIER) && current_token().get_value() == "as") {
        advance();
        
        if (current_token().get_type() != TokenType::IDENTIFIER) {
            add_error("Expected identifier after 'as'");
            return nullptr;
        }
        local_name = current_token().get_value();
        advance();
    }
    
    Position end = get_current_position();
    return std::make_unique<ImportSpecifier>(imported_name, local_name, start, end);
}

std::unique_ptr<ExportSpecifier> Parser::parse_export_specifier() {
    Position start = current_token().get_start();
    
    if (!match(TokenType::IDENTIFIER)) {
        add_error("Expected identifier in export specifier");
        return nullptr;
    }
    
    std::string local_name = current_token().get_value();
    std::string exported_name = local_name;
    advance();
    
    if (match(TokenType::IDENTIFIER) && current_token().get_value() == "as") {
        advance();
        
        if (current_token().get_type() != TokenType::IDENTIFIER) {
            add_error("Expected identifier after 'as'");
            return nullptr;
        }
        exported_name = current_token().get_value();
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
        
        while (!match(TokenType::RIGHT_BRACKET) && !at_end()) {
            if (current_token().get_type() == TokenType::ELLIPSIS) {
                advance();
                
                if (current_token().get_type() != TokenType::IDENTIFIER) {
                    add_error("Expected identifier after '...' in array destructuring");
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
                    add_error("Rest element must be last element in array destructuring");
                    return nullptr;
                }
                break;
                
            } else if (current_token().get_type() == TokenType::IDENTIFIER) {
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
                advance();
                
                std::string nested_vars = "";
                while (!match(TokenType::RIGHT_BRACKET) && !at_end()) {
                    if (current_token().get_type() == TokenType::IDENTIFIER) {
                        if (!nested_vars.empty()) nested_vars += ",";
                        nested_vars += current_token().get_value();
                        advance();
                    } else if (current_token().get_type() == TokenType::COMMA) {
                        advance();
                    } else {
                        advance();
                    }
                }
                
                if (!consume(TokenType::RIGHT_BRACKET)) {
                    add_error("Expected ']' in nested destructuring");
                    return nullptr;
                }
                
                
                auto nested_placeholder = std::make_unique<Identifier>(
                    "__nested_vars:" + nested_vars,
                    current_token().get_start(),
                    current_token().get_end()
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
        
        return std::move(destructuring);
        
    } else if (current_token().get_type() == TokenType::LEFT_BRACE) {
        advance();
        
        std::vector<std::unique_ptr<Identifier>> targets;
        std::vector<std::pair<std::string, std::string>> property_mappings;
        
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
                
            } else if (current_token().get_type() == TokenType::IDENTIFIER) {
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
                        printf("DEBUG: extracted nested_vars: '%s'\n", nested_vars.c_str());

                        std::string original_property_name = targets.back()->get_name();


                        std::string proper_pattern = nested_vars;
                        if (auto nested_destructuring = dynamic_cast<DestructuringAssignment*>(nested.get())) {
                            const auto& mappings = nested_destructuring->get_property_mappings();
                            printf("DEBUG: Found %zu property mappings in nested destructuring\n", mappings.size());

                            std::string property_name = "";
                            if (!mappings.empty()) {
                                property_name = mappings[0].property_name;
                                printf("DEBUG: First property mapping: '%s' -> '%s'\n",
                                       mappings[0].property_name.c_str(), mappings[0].variable_name.c_str());
                            } else {
                                const auto& targets = nested_destructuring->get_targets();
                                if (!targets.empty()) {
                                    std::string first_target = targets[0]->get_name();
                                    printf("DEBUG: No property mappings, using target: '%s'\n", first_target.c_str());

                                    if (first_target.find("__nested") == std::string::npos) {
                                        property_name = first_target;
                                    }
                                }
                            }

                            if (!property_name.empty()) {
                                printf("DEBUG: Using property name: '%s'\n", property_name.c_str());
                                size_t nested_pos = nested_vars.find("__nested:");
                                if (nested_pos != std::string::npos && nested_pos + 9 < nested_vars.length()) {
                                    proper_pattern = property_name + ":" + nested_vars;
                                    printf("DEBUG: Built complete navigation pattern: '%s'\n", proper_pattern.c_str());
                                } else {
                                    printf("DEBUG: Comparing property_name='%s' with nested_vars='%s'\n", property_name.c_str(), nested_vars.c_str());
                                    if (property_name == nested_vars) {
                                        proper_pattern = "__nested:" + nested_vars;
                                        printf("DEBUG: Built nested access pattern: '%s'\n", proper_pattern.c_str());
                                    } else {
                                        proper_pattern = property_name + ":" + nested_vars;
                                        printf("DEBUG: Built nested renaming/navigation pattern: '%s'\n", proper_pattern.c_str());
                                    }
                                }
                            }
                        }
                        property_mappings.emplace_back(original_property_name, proper_pattern);

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
                    } else {
                        add_error("Expected identifier or nested pattern after ':'");
                        return nullptr;
                    }
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
            destructuring->add_property_mapping(mapping.first, mapping.second);
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
    
    auto argument = parse_assignment_expression();
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
    while (!at_end() && current_token().get_type() == TokenType::STRING) {
        std::string str_value = current_token().get_value();
        
        if (str_value == "\"use strict\"" || str_value == "'use strict'") {
            options_.strict_mode = true;
            advance();
            
            if (match(TokenType::SEMICOLON)) {
                advance();
            }
            
            continue;
        }
        
        advance();
        if (match(TokenType::SEMICOLON)) {
            advance();
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

    while (!match(TokenType::RIGHT_PAREN) && !at_end()) {
        Position param_start = current_token().get_start();
        bool is_rest = false;

        if (match(TokenType::ELLIPSIS)) {
            is_rest = true;
            has_non_simple_params = true;
            advance();
        }

        std::unique_ptr<Identifier> param_name = nullptr;
        if (current_token().get_type() == TokenType::IDENTIFIER) {
            param_name = std::make_unique<Identifier>(current_token().get_value(),
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
        add_error("Expected ')' after parameters");
        return nullptr;
    }

    if (!consume(TokenType::ARROW)) {
        add_error("Expected '=>' for async arrow function");
        return nullptr;
    }

    std::unique_ptr<ASTNode> body = nullptr;
    if (match(TokenType::LEFT_BRACE)) {
        body = parse_block_statement();
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

    if (!body) {
        add_error("Expected async arrow function body");
        return nullptr;
    }

    Position end = get_current_position();

    return std::make_unique<AsyncFunctionExpression>(
        nullptr,
        std::move(params),
        std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body.release())),
        start, end
    );
}

std::unique_ptr<ASTNode> Parser::parse_async_arrow_function_single_param(Position start) {
    std::vector<std::unique_ptr<Parameter>> params;

    if (current_token().get_type() != TokenType::IDENTIFIER) {
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
    if (match(TokenType::LEFT_BRACE)) {
        body = parse_block_statement();
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

    if (!body) {
        add_error("Expected async arrow function body");
        return nullptr;
    }

    Position end = get_current_position();

    return std::make_unique<AsyncFunctionExpression>(
        nullptr,
        std::move(params),
        std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body.release())),
        start, end
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

    printf("DEBUG PARSER: generate_proper_nested_pattern called with depth %d, node type %d\n", depth, (int)node->get_type());

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

        printf("DEBUG: Found %zu property mappings in nested destructuring\n", mappings.size());

        if (!mappings.empty()) {
            std::vector<std::string> nested_vars;
            for (const auto& mapping : mappings) {
                printf("DEBUG: First property mapping: '%s' -> '%s'\n", mapping.property_name.c_str(), mapping.variable_name.c_str());
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
            printf("DEBUG: No property mappings, using target: '%s'\n",
                   !destructuring->get_targets().empty() ? destructuring->get_targets()[0]->get_name().c_str() : "none");
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
