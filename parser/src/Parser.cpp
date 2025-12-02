/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../include/Parser.h"
#include <algorithm>
#include <iostream>
#include <map>

namespace Quanta {

//=============================================================================
// Parser Implementation
//=============================================================================

Parser::Parser(TokenSequence tokens)
    : tokens_(std::move(tokens)), current_token_index_(0) {
    options_.allow_return_outside_function = false;
    options_.allow_await_outside_async = false;
    options_.strict_mode = false;
    options_.source_type_module = false;
    
    // Skip initial newlines, whitespace, and comments
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
    // Skip initial newlines, whitespace, and comments
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
    
    // Check for "use strict" directive at the beginning
    check_for_use_strict_directive();
    
    while (!at_end()) {
        try {
            auto statement = parse_statement();
            if (statement) {
                statements.push_back(std::move(statement));
            } else {
                // If parse_statement returns nullptr, we need to advance to avoid infinite loops
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
    // Check what kind of statement this is
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
            // Check if this is dynamic import: import() vs static import: import {}
            if (peek_token().get_type() == TokenType::LEFT_PAREN) {
                // This is dynamic import: import(...) - treat as expression
                return parse_expression_statement();
            } else {
                // This is static import: import {} from "..." - parse as statement
                return parse_import_statement();
            }
            
        case TokenType::EXPORT:
            return parse_export_statement();

        case TokenType::SEMICOLON:
            // Empty statement
            {
                Position start = current_token().get_start();
                Position end = current_token().get_end();
                advance(); // consume ';'
                return std::make_unique<EmptyStatement>(start, end);
            }

        default:
            return parse_expression_statement();
    }
}

std::unique_ptr<ASTNode> Parser::parse_expression() {
    // Parse comma expressions (lowest precedence)
    auto left = parse_assignment_expression();
    if (!left) return nullptr;
    
    // Handle comma operator: expr1, expr2, expr3
    while (match(TokenType::COMMA)) {
        advance(); // consume ','
        auto right = parse_assignment_expression();
        if (!right) {
            add_error("Expected expression after ','");
            return nullptr;
        }
        
        // Create comma expression node - evaluates left, discards it, returns right
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
    // Check for arrow function: identifier => expression
    if (match(TokenType::IDENTIFIER) && peek_token(1).get_type() == TokenType::ARROW) {
        return parse_arrow_function();
    }
    
    // Check for arrow function: (params) => expression
    if (match(TokenType::LEFT_PAREN)) {
        size_t saved_pos = current_token_index_;
        if (try_parse_arrow_function_params()) {
            // Restore position and parse as arrow function
            current_token_index_ = saved_pos;
            return parse_arrow_function();
        }
        // Restore position and continue with normal parsing
        current_token_index_ = saved_pos;
    }
    
    auto left = parse_conditional_expression();
    if (!left) return nullptr;
    
    if (is_assignment_operator(current_token().get_type())) {
        // Validate left-hand side for assignment
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
    // Prevent infinite recursion with depth limit
    if (depth > 100) {
        add_error("Maximum ternary nesting depth exceeded");
        return nullptr;
    }
    
    // Parse the test condition (logical OR expression)
    auto test = parse_logical_or_expression();
    if (!test) return nullptr;
    
    // Check for ternary operator
    if (current_token().get_type() == TokenType::QUESTION) {
        advance(); // consume '?'
        Position start = test->get_start();
        
        // Parse consequent - use logical OR to avoid mutual recursion
        auto consequent = parse_logical_or_expression();
        if (!consequent) {
            add_error("Expected expression after '?' in conditional expression");
            return nullptr;
        }
        
        if (current_token().get_type() != TokenType::COLON) {
            add_error("Expected ':' after consequent in conditional expression");
            return nullptr;
        }
        advance(); // consume ':'
        
        // For right-associativity, recursively parse another conditional expression
        // but with depth tracking to prevent infinite recursion
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
        advance(); // consume '??'
        
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
    
    // Right-associative
    if (match(TokenType::EXPONENT)) {
        Position op_start = current_token().get_start();
        advance();
        
        auto right = parse_exponentiation_expression(); // Recursive for right-associativity
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
    // Handle 'await' expression
    if (current_token().get_type() == TokenType::AWAIT) {
        Position start = current_token().get_start();
        advance(); // consume 'await'
        
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
    
    // Handle postfix increment/decrement (x++, x--)
    while (current_token().get_type() == TokenType::INCREMENT || 
           current_token().get_type() == TokenType::DECREMENT) {
        TokenType op_token = current_token().get_type();
        Position start = expr->get_start();
        Position end = current_token().get_end();
        advance(); // consume ++ or --
        
        UnaryExpression::Operator op = (op_token == TokenType::INCREMENT) ? 
            UnaryExpression::Operator::POST_INCREMENT : 
            UnaryExpression::Operator::POST_DECREMENT;
        
        expr = std::make_unique<UnaryExpression>(op, std::move(expr), false, start, end);
    }
    
    return expr;
}

std::unique_ptr<ASTNode> Parser::parse_call_expression() {
    std::unique_ptr<ASTNode> expr;

    // Handle 'new' expression at call expression level to allow member access
    if (current_token().get_type() == TokenType::NEW) {
        Position start = current_token().get_start();

        // Check for new.target metaproperty
        if (peek_token().get_type() == TokenType::DOT) {
            advance(); // consume 'new'
            advance(); // consume '.'

            if (current_token().get_type() == TokenType::TARGET) {
                Position end = current_token().get_end();
                advance(); // consume 'target'
                return std::make_unique<MetaProperty>("new", "target", start, end);
            } else {
                add_error("Expected 'target' after 'new.'");
                return nullptr;
            }
        }

        advance(); // consume 'new'

        // Parse constructor expression (can be identifier or member expression)
        auto constructor = parse_primary_expression();
        if (!constructor) {
            add_error("Expected constructor expression after 'new'");
            return nullptr;
        }

        // Handle member access for constructor (e.g., obj.Function)
        while (match(TokenType::DOT) || match(TokenType::LEFT_BRACKET)) {
            Position ctor_start = constructor->get_start();

            if (match(TokenType::DOT)) {
                advance(); // consume '.'

                // Support private field access: obj.#field
                std::string name;
                Position prop_start;
                Position prop_end;

                if (match(TokenType::HASH)) {
                    // Private field access
                    prop_start = current_token().get_start();
                    advance(); // consume '#'

                    if (!match(TokenType::IDENTIFIER)) {
                        add_error("Expected identifier after '#' in member access");
                        return nullptr;
                    }

                    name = "#" + current_token().get_value();
                    prop_end = current_token().get_end();
                    advance();
                } else if (match(TokenType::IDENTIFIER) || is_keyword_token(current_token().get_type())) {
                    // Regular property access
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
            // Could add LEFT_BRACKET handling here for computed properties if needed
        }

        std::vector<std::unique_ptr<ASTNode>> arguments;

        // Parse arguments if parentheses are present
        if (current_token().get_type() == TokenType::LEFT_PAREN) {
            advance(); // consume '('

            if (current_token().get_type() != TokenType::RIGHT_PAREN) {
                do {
                    auto arg = parse_assignment_expression();
                    if (!arg) {
                        add_error("Expected argument expression");
                        return nullptr;
                    }
                    arguments.push_back(std::move(arg));
                } while (consume_if_match(TokenType::COMMA));
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
    
    // Parse member access and function calls in any order (supports chaining like .then().then())
    while (match(TokenType::DOT) || match(TokenType::LEFT_BRACKET) ||
           match(TokenType::OPTIONAL_CHAINING) || match(TokenType::LEFT_PAREN) ||
           match(TokenType::TEMPLATE_LITERAL)) {
        Position start = expr->get_start();
        
        if (match(TokenType::DOT)) {
            // Member access: obj.property or obj.#privateField
            advance(); // consume '.'

            // Support private field access: obj.#field
            std::string name;
            Position prop_start;
            Position prop_end;

            if (match(TokenType::HASH)) {
                // Private field access
                prop_start = current_token().get_start();
                advance(); // consume '#'

                if (!match(TokenType::IDENTIFIER)) {
                    add_error("Expected identifier after '#' in member access");
                    return expr;
                }

                name = "#" + current_token().get_value();
                prop_end = current_token().get_end();
                advance();
            } else if (match(TokenType::IDENTIFIER) || is_keyword_token(current_token().get_type())) {
                // Regular property access
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
            // Computed member access: obj[property]
            advance(); // consume '['
            
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
            // Optional chaining: obj?.property
            advance(); // consume '?.'
            
            if (match(TokenType::LEFT_BRACKET)) {
                // obj?.[computed]
                advance(); // consume '['
                
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
                // obj?.() - optional function call
                advance(); // consume '('

                std::vector<std::unique_ptr<ASTNode>> arguments;
                if (!match(TokenType::RIGHT_PAREN)) {
                    do {
                        auto arg = parse_assignment_expression();
                        if (!arg) {
                            add_error("Expected argument in optional call");
                            break;
                        }
                        arguments.push_back(std::move(arg));
                    } while (consume_if_match(TokenType::COMMA));
                }

                if (!consume(TokenType::RIGHT_PAREN)) {
                    add_error("Expected ')' after optional call arguments");
                    return expr;
                }

                Position end = get_current_position();
                // Create CallExpression for optional call (?.())
                expr = std::make_unique<CallExpression>(std::move(expr), std::move(arguments), start, end);
            } else {
                // obj?.property
                if (!match(TokenType::IDENTIFIER) && current_token().get_type() != TokenType::FOR &&
                    current_token().get_type() != TokenType::FROM && current_token().get_type() != TokenType::OF &&
                    current_token().get_type() != TokenType::DELETE) {
                    add_error("Expected property name after '?.'");
                    return expr;
                }

                // Create property identifier from current token (identifier or keyword)
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
            // Function call: func(args)
            advance(); // consume '('
            
            std::vector<std::unique_ptr<ASTNode>> arguments;
            
            if (!match(TokenType::RIGHT_PAREN)) {
                do {
                    // Check for spread element in function call: func(...args)
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
                } while (consume_if_match(TokenType::COMMA));
            }
            
            if (!consume(TokenType::RIGHT_PAREN)) {
                add_error("Expected ')' after function arguments");
                return expr;
            }
            
            Position end = get_current_position();
            expr = std::make_unique<CallExpression>(std::move(expr), std::move(arguments), start, end);
        } else if (match(TokenType::TEMPLATE_LITERAL)) {
            // Tagged template literal: func`template`
            const Token& template_token = current_token();
            Position template_start = template_token.get_start();
            Position template_end = template_token.get_end();
            std::string template_str = template_token.get_value();

            advance(); // consume template literal

            // Create template object argument for tagged template
            std::vector<std::unique_ptr<ASTNode>> arguments;

            // For simplicity, create a string literal argument
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
            advance(); // consume '.'

            // Allow identifiers, keywords, and private fields as property names
            if (!match(TokenType::IDENTIFIER) && !is_keyword_token(current_token().get_type()) && !match(TokenType::HASH)) {
                // DEBUG: Show what token we got
                add_error("Expected property name after '.', got token type: " + std::to_string(static_cast<int>(current_token().get_type())));
                return expr;
            }

            std::unique_ptr<Identifier> property;
            if (match(TokenType::HASH)) {
                // Parse private field: obj.#field
                auto private_field = parse_private_field();
                if (!private_field) {
                    add_error("Invalid private field after '.'");
                    return expr;
                }
                property = std::unique_ptr<Identifier>(static_cast<Identifier*>(private_field.release()));
            } else {
                // Create property identifier from current token (identifier or keyword)
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
            advance(); // consume '?.'
            
            if (match(TokenType::LEFT_BRACKET)) {
                // obj?.[computed]
                advance(); // consume '['
                
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
                // obj?.property (allow both identifiers and keywords)
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
        } else { // LEFT_BRACKET
            advance(); // consume '['
            
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
            advance(); // CRITICAL: Advance to prevent infinite loops
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

    // Handle implicit string concatenation: consecutive string literals
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
    
    // Create a special identifier for 'this'
    return std::make_unique<Identifier>("this", start, end);
}

std::unique_ptr<ASTNode> Parser::parse_super_expression() {
    const Token& token = current_token();
    Position start = token.get_start();
    Position end = token.get_end();
    advance();
    
    // Create a special identifier for 'super'
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
    
    advance(); // consume template literal token
    
    std::vector<TemplateLiteral::Element> elements;
    
    // Parse the template string for ${} expressions
    size_t pos = 0;
    while (pos < template_str.length()) {
        size_t expr_start = template_str.find("${", pos);
        
        if (expr_start == std::string::npos) {
            // No more expressions, add remaining text
            if (pos < template_str.length()) {
                elements.emplace_back(template_str.substr(pos));
            }
            break;
        }
        
        // Add text before expression
        if (expr_start > pos) {
            elements.emplace_back(template_str.substr(pos, expr_start - pos));
        }
        
        // Find the closing } with proper brace counting
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
        
        // Extract and parse the expression
        std::string expr_str = template_str.substr(expr_start + 2, expr_end - expr_start - 2);
        
        // Create a mini-lexer and parser for the expression
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
    
    advance(); // consume regex token
    
    // Parse the regex string to extract pattern and flags
    // Expected format: /pattern/flags
    if (regex_str.length() < 2 || regex_str[0] != '/') {
        add_error("Invalid regex literal format");
        return nullptr;
    }
    
    // Find the closing slash
    size_t closing_slash = regex_str.find_last_of('/');
    if (closing_slash == 0 || closing_slash == std::string::npos) {
        add_error("Invalid regex literal: missing closing slash");
        return nullptr;
    }
    
    // Extract pattern and flags
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
    std::string name = "#" + token.get_value(); // Include # in the name
    Position end = token.get_end();
    advance();

    // For now, return as Identifier with # prefix
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

//=============================================================================
// Helper Methods
//=============================================================================

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
        default: return UnaryExpression::Operator::PLUS; // fallback
    }
}

//=============================================================================
// Token Navigation
//=============================================================================

const Token& Parser::current_token() const {
    return tokens_[current_token_index_];
}

const Token& Parser::peek_token(size_t offset) const {
    return tokens_[current_token_index_ + offset];
}

void Parser::advance() {
    if (current_token_index_ < tokens_.size() - 1) {
        current_token_index_++;
        // Skip newlines, whitespace, and comments
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
    // Allow reserved words as property/method names
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

//=============================================================================
// Error Handling
//=============================================================================

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

//=============================================================================
// Validation
//=============================================================================

bool Parser::is_assignment_operator(TokenType type) const {
    return type == TokenType::ASSIGN ||
           type == TokenType::PLUS_ASSIGN ||
           type == TokenType::MINUS_ASSIGN ||
           type == TokenType::MULTIPLY_ASSIGN ||
           type == TokenType::DIVIDE_ASSIGN ||
           type == TokenType::MODULO_ASSIGN;
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
            // Call expressions are not valid assignment targets
            return false;
        case ASTNode::Type::OBJECT_LITERAL:
            // In destructuring context, object literals can be assignment targets
            return true;
        case ASTNode::Type::ARRAY_LITERAL:
            // In destructuring context, array literals can be assignment targets
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

//=============================================================================
// Statement Parsing Implementation
//=============================================================================

std::unique_ptr<ASTNode> Parser::parse_variable_declaration() {
    return parse_variable_declaration(true);
}

std::unique_ptr<ASTNode> Parser::parse_variable_declaration(bool consume_semicolon) {
    Position start = get_current_position();
    
    // Get declaration kind (var, let, const)
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
    advance(); // consume var/let/const
    
    std::vector<std::unique_ptr<VariableDeclarator>> declarations;
    
    do {
        // Check for destructuring pattern
        if (current_token().get_type() == TokenType::LEFT_BRACKET || 
            current_token().get_type() == TokenType::LEFT_BRACE) {
            // Parse destructuring pattern
            auto destructuring = parse_destructuring_pattern();
            if (!destructuring) {
                add_error("Invalid destructuring pattern");
                return nullptr;
            }
            
            // Destructuring must have an initializer
            if (!consume_if_match(TokenType::ASSIGN)) {
                add_error("Destructuring declaration must have an initializer");
                return nullptr;
            }
            
            auto init = parse_assignment_expression();
            if (!init) {
                add_error("Expected expression after '=' in destructuring declaration");
                return nullptr;
            }
            
            // Set the source for the destructuring assignment
            DestructuringAssignment* dest = static_cast<DestructuringAssignment*>(destructuring.get());
            dest->set_source(std::move(init));
            
            // Create a special variable declarator that wraps the destructuring assignment
            auto declarator = std::make_unique<VariableDeclarator>(
                std::make_unique<Identifier>("", start, start), // dummy identifier
                std::move(destructuring),
                kind,
                start,
                get_current_position()
            );
            
            declarations.push_back(std::move(declarator));
            break; // Only one destructuring per declaration
        }
        
        // Parse identifier
        if (current_token().get_type() != TokenType::IDENTIFIER) {
            add_error("Expected identifier in variable declaration");
            return nullptr;
        }
        
        auto id = std::make_unique<Identifier>(current_token().get_value(), 
                                             current_token().get_start(), current_token().get_end());
        advance();
        
        // Parse optional initializer
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
    
    // Consume semicolon only if requested
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
            // Skip to next statement on error
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

    // Check for 'await' keyword (for-await-of loops)
    bool is_await_loop = false;
    if (match(TokenType::AWAIT)) {
        is_await_loop = true;
        advance();
    }

    if (!consume(TokenType::LEFT_PAREN)) {
        add_error("Expected '(' after 'for'");
        return nullptr;
    }
    
    // Parse init (can be variable declaration or expression)
    std::unique_ptr<ASTNode> init = nullptr;
    if (!match(TokenType::SEMICOLON)) {
        if (match(TokenType::VAR) || match(TokenType::LET) || match(TokenType::CONST)) {
            
            // For for...of loops, we need to parse the variable part manually
            // since parse_variable_declaration expects a full declaration
            Position decl_start = get_current_position();
            
            // Get the declaration kind
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
            advance(); // consume var/let/const
            
            // Handle destructuring patterns in for loops
            if (current_token().get_type() == TokenType::LEFT_BRACKET ||
                current_token().get_type() == TokenType::LEFT_BRACE) {

                // Parse destructuring pattern
                auto destructuring = parse_destructuring_pattern();
                if (!destructuring) {
                    add_error("Failed to parse destructuring pattern");
                    return nullptr;
                }

                // Check for assignment (= expression) - needed for regular for loops
                if (current_token().get_type() == TokenType::ASSIGN) {
                    advance(); // consume '='
                    auto initializer = parse_assignment_expression();
                    if (!initializer) {
                        add_error("Expected expression after '=' in destructuring assignment");
                        return nullptr;
                    }

                    // Create assignment expression with destructuring on left
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
                    // No assignment - for for...of loops
                    init = std::move(destructuring);
                }

                // Check if this is for...of or regular for loop
                goto check_for_of;
            }

            // Parse regular identifier
            if (current_token().get_type() != TokenType::IDENTIFIER) {
                add_error("Expected identifier in variable declaration");
                return nullptr;
            }

            std::string var_name = current_token().get_value();
            Position var_start = current_token().get_start();
            Position var_end = current_token().get_end();
            advance(); // consume identifier

            // Create a variable declarator - check for initializer
            auto identifier = std::make_unique<Identifier>(var_name, var_start, var_end);

            // Check for initializer (= expression) for regular for loops
            std::unique_ptr<ASTNode> initializer = nullptr;
            if (current_token().get_type() == TokenType::ASSIGN) {
                advance(); // consume '='
                initializer = parse_assignment_expression();
                if (!initializer) {
                    add_error("Expected expression after '=' in variable declaration");
                    return nullptr;
                }
            }

            auto declarator = std::make_unique<VariableDeclarator>(
                std::move(identifier),
                std::move(initializer), // include initializer if present
                kind,
                var_start,
                var_end
            );
            
            std::vector<std::unique_ptr<VariableDeclarator>> declarations;
            declarations.push_back(std::move(declarator));

            // Handle multiple variable declarations separated by commas
            while (match(TokenType::COMMA)) {
                advance(); // consume ','

                auto next_identifier = parse_identifier();
                if (!next_identifier) {
                    add_error("Expected identifier after ',' in variable declaration");
                    return nullptr;
                }

                std::unique_ptr<ASTNode> next_initializer = nullptr;
                if (match(TokenType::ASSIGN)) {
                    advance(); // consume '='
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

    // Check for for...in syntax (for both variable declarations and expressions)
    if (current_token().get_type() == TokenType::IN) {
        advance(); // consume 'in'
        
        // Safety check for end of input
        if (at_end()) {
            add_error("Unexpected end of input after 'in'");
            return nullptr;
        }
        
        // Parse the object expression
        auto object = parse_expression();
        if (!object) {
            add_error("Expected expression after 'in' in for...in loop");
            return nullptr;
        }
        
        // Expect closing parenthesis
        if (at_end() || current_token().get_type() != TokenType::RIGHT_PAREN) {
            add_error("Expected ')' after for...in object");
            return nullptr;
        }
        advance(); // consume ')'
        
        // Parse body
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
    // Check for for...in syntax first
    if (current_token().get_type() == TokenType::IN) {
        advance(); // consume 'in'

        // Safety check for end of input
        if (at_end()) {
            add_error("Unexpected end of input after 'in'");
            return nullptr;
        }

        // Parse the object expression
        auto object = parse_expression();
        if (!object) {
            add_error("Expected expression after 'in' in for...in loop");
            return nullptr;
        }

        // Expect closing parenthesis
        if (at_end() || current_token().get_type() != TokenType::RIGHT_PAREN) {
            add_error("Expected ')' after for...in object");
            return nullptr;
        }
        advance(); // consume ')'

        // Parse body
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

    // Check for for...of syntax
    if (current_token().get_type() == TokenType::OF) {
        advance(); // consume 'of'
        
        // Safety check for end of input
        if (at_end()) {
            add_error("Unexpected end of input after 'of'");
            return nullptr;
        }
        
        // Parse the iterable expression - allow any expression
        auto iterable = parse_expression();
        if (!iterable) {
            add_error("Expected expression after 'of' in for...of loop");
            return nullptr;
        }
        
        // Expect closing parenthesis
        if (at_end() || current_token().get_type() != TokenType::RIGHT_PAREN) {
            add_error("Expected ')' after for...of iterable");
            return nullptr;
        }
        advance(); // consume ')'
        
        // Parse body
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
    
    // Parse test condition
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
    
    // Parse update expression
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
    
    // Parse body
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
    
    // Parse test condition
    auto test = parse_expression();
    if (!test) {
        add_error("Expected condition in while loop");
        return nullptr;
    }
    
    if (!consume(TokenType::RIGHT_PAREN)) {
        add_error("Expected ')' after while condition");
        return nullptr;
    }
    
    // Parse body
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
    
    // Parse body first (this is the key difference from while)
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
    
    // Parse test condition
    auto test = parse_expression();
    if (!test) {
        add_error("Expected condition in do-while loop");
        return nullptr;
    }
    
    if (!consume(TokenType::RIGHT_PAREN)) {
        add_error("Expected ')' after do-while condition");
        return nullptr;
    }
    
    // Consume optional semicolon
    consume_if_match(TokenType::SEMICOLON);
    
    Position end = get_current_position();
    return std::make_unique<DoWhileStatement>(std::move(body), std::move(test), start, end);
}

std::unique_ptr<ASTNode> Parser::parse_expression_statement() {
    Position start = get_current_position();

    // Check for labeled statement: identifier followed by ':'
    if (current_token().get_type() == TokenType::IDENTIFIER && peek_token().get_type() == TokenType::COLON) {
        std::string label = current_token().get_value();
        advance(); // consume identifier
        advance(); // consume ':'

        // Parse the statement after the label
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

    // Automatic Semicolon Insertion (ASI) rules:
    // 1. If there's an explicit semicolon, consume it
    // 2. If we're at EOF, line terminator, or '}', ASI applies
    // 3. Otherwise, we can still continue (for expressions that span multiple lines)
    if (match(TokenType::SEMICOLON)) {
        advance(); // Consume explicit semicolon
    } else if (at_end() || match(TokenType::RIGHT_BRACE) || current_token().get_start().line > end.line) {
        // ASI: Automatic semicolon insertion applies
        // - At end of file
        // - Before '}'
        // - After line terminator (different line)
        // No need to consume anything, semicolon is automatically inserted
    }
    // If none of the above, continue without semicolon (valid for multi-line expressions)
    
    return std::make_unique<ExpressionStatement>(std::move(expr), start, end);
}

std::unique_ptr<ASTNode> Parser::parse_function_declaration() {
    Position start = get_current_position();
    
    if (!consume(TokenType::FUNCTION)) {
        add_error("Expected 'function'");
        return nullptr;
    }
    
    // Check for generator function (function*)
    bool is_generator = false;
    if (current_token().get_type() == TokenType::MULTIPLY) {
        advance(); // consume '*'
        is_generator = true;
    }
    
    // Parse function name
    if (current_token().get_type() != TokenType::IDENTIFIER) {
        add_error("Expected function name");
        return nullptr;
    }
    
    auto id = std::make_unique<Identifier>(current_token().get_value(), 
                                         current_token().get_start(), current_token().get_end());
    advance();
    
    // Parse parameter list
    if (!consume(TokenType::LEFT_PAREN)) {
        add_error("Expected '(' after function name");
        return nullptr;
    }

    std::vector<std::unique_ptr<Parameter>> params;
    bool has_non_simple_params = false;  // Track if any parameter is non-simple

    while (!match(TokenType::RIGHT_PAREN) && !at_end()) {
        Position param_start = current_token().get_start();
        bool is_rest = false;

        // Check for rest parameter syntax: ...args
        if (match(TokenType::ELLIPSIS)) {
            is_rest = true;
            has_non_simple_params = true;  // Rest parameters are non-simple
            advance(); // consume '...'
        }

        std::unique_ptr<Identifier> param_name = nullptr;

        if (current_token().get_type() == TokenType::LEFT_BRACKET) {
            // Array destructuring parameter: [a, b, c]
            has_non_simple_params = true;  // Destructuring makes params non-simple
            advance(); // consume '['

            // Skip through the array destructuring pattern
            std::string destructuring_name = "__array_destructuring_";
            int bracket_count = 1;
            while (bracket_count > 0 && !at_end()) {
                if (current_token().get_type() == TokenType::LEFT_BRACKET) {
                    bracket_count++;
                } else if (current_token().get_type() == TokenType::RIGHT_BRACKET) {
                    bracket_count--;
                } else if (current_token().get_type() == TokenType::IDENTIFIER && bracket_count == 1) {
                    // Collect first identifier for parameter name
                    if (destructuring_name == "__array_destructuring_") {
                        destructuring_name += current_token().get_value();
                    }
                } else if (current_token().get_type() == TokenType::ASSIGN) {
                    // Handle default value assignment within destructuring: [x] = [1]
                    advance(); // consume '='
                    // Skip the default value expression (could be array, object, etc.)
                    if (current_token().get_type() == TokenType::LEFT_BRACKET) {
                        // Array default value: = [1, 2, 3]
                        int inner_bracket_count = 1;
                        advance(); // consume '['
                        while (inner_bracket_count > 0 && !at_end()) {
                            if (current_token().get_type() == TokenType::LEFT_BRACKET) {
                                inner_bracket_count++;
                            } else if (current_token().get_type() == TokenType::RIGHT_BRACKET) {
                                inner_bracket_count--;
                            }
                            if (inner_bracket_count > 0) advance();
                        }
                        if (inner_bracket_count == 0) advance(); // consume final ']'
                        continue; // Don't advance again at end of main loop
                    } else if (current_token().get_type() == TokenType::LEFT_BRACE) {
                        // Object default value: = {a: 1}
                        int inner_brace_count = 1;
                        advance(); // consume '{'
                        while (inner_brace_count > 0 && !at_end()) {
                            if (current_token().get_type() == TokenType::LEFT_BRACE) {
                                inner_brace_count++;
                            } else if (current_token().get_type() == TokenType::RIGHT_BRACE) {
                                inner_brace_count--;
                            }
                            if (inner_brace_count > 0) advance();
                        }
                        if (inner_brace_count == 0) advance(); // consume final '}'
                        continue; // Don't advance again at end of main loop
                    } else {
                        // Simple default value: = 5, = "string", etc.
                        while (!at_end() &&
                               current_token().get_type() != TokenType::COMMA &&
                               current_token().get_type() != TokenType::RIGHT_BRACKET) {
                            advance();
                        }
                        continue; // Don't advance again at end of main loop
                    }
                }
                advance();
            }

            // Create identifier for the destructured parameter
            Position param_pos = get_current_position();
            param_name = std::make_unique<Identifier>(destructuring_name, param_pos, param_pos);
        } else if (current_token().get_type() == TokenType::LEFT_BRACE) {
            // Object destructuring parameter: {a, b, c}
            has_non_simple_params = true;  // Destructuring makes params non-simple
            advance(); // consume '{'

            // Skip through the destructuring pattern
            std::string destructuring_name = "__destructuring_";
            int brace_count = 1;
            while (brace_count > 0 && !at_end()) {
                if (current_token().get_type() == TokenType::LEFT_BRACE) {
                    brace_count++;
                } else if (current_token().get_type() == TokenType::RIGHT_BRACE) {
                    brace_count--;
                } else if (current_token().get_type() == TokenType::IDENTIFIER && brace_count == 1) {
                    // Collect first identifier for parameter name
                    if (destructuring_name == "__destructuring_") {
                        destructuring_name += current_token().get_value();
                    }
                }
                advance();
            }

            // Create identifier for the destructured parameter
            Position param_pos = get_current_position();
            param_name = std::make_unique<Identifier>(destructuring_name, param_pos, param_pos);
        } else if (current_token().get_type() == TokenType::IDENTIFIER) {
            // Regular identifier parameter
            param_name = std::make_unique<Identifier>(current_token().get_value(),
                                                      current_token().get_start(), current_token().get_end());
            advance();
        } else {
            add_error("Expected parameter name or destructuring pattern");
            return nullptr;
        }
        
        // Check for default parameter syntax: param = value
        // Note: Rest parameters cannot have default values
        std::unique_ptr<ASTNode> default_value = nullptr;
        if (!is_rest && match(TokenType::ASSIGN)) {
            has_non_simple_params = true;  // Default parameters make params non-simple
            advance(); // consume '='
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
        
        // Rest parameter must be last
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
    
    // Parse function body
    auto body = parse_block_statement();
    if (!body) {
        add_error("Expected function body");
        return nullptr;
    }
    
    // ECMAScript validation: Non-simple parameters cannot be used with strict mode
    if (has_non_simple_params && body) {
        // Check if function body contains "use strict" directive
        BlockStatement* block = static_cast<BlockStatement*>(body.get());
        if (block && !block->get_statements().empty()) {
            // Check first statement for "use strict" directive
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
        start, end, false, is_generator  // is_async = false, is_generator = variable
    );
}

std::unique_ptr<ASTNode> Parser::parse_class_declaration() {
    Position start = get_current_position();
    
    if (!consume(TokenType::CLASS)) {
        add_error("Expected 'class'");
        return nullptr;
    }
    
    // Parse class name
    if (current_token().get_type() != TokenType::IDENTIFIER) {
        add_error("Expected class name");
        return nullptr;
    }
    
    auto id = parse_identifier();
    if (!id) return nullptr;
    
    // Check for extends clause
    std::unique_ptr<Identifier> superclass = nullptr;
    if (match(TokenType::EXTENDS)) {
        advance(); // consume 'extends'
        
        if (current_token().get_type() != TokenType::IDENTIFIER) {
            add_error("Expected superclass name after 'extends'");
            return nullptr;
        }
        
        superclass = std::unique_ptr<Identifier>(static_cast<Identifier*>(parse_identifier().release()));
        if (!superclass) return nullptr;
    }
    
    // Parse class body with methods
    if (!match(TokenType::LEFT_BRACE)) {
        add_error("Expected '{' to start class body");
        return nullptr;
    }
    
    advance(); // consume '{'
    
    // Parse class body - now parse methods properly
    std::vector<std::unique_ptr<ASTNode>> statements;
    
    // Parse methods within the class
    while (current_token().get_type() != TokenType::RIGHT_BRACE && !at_end()) {
        if (current_token().get_type() == TokenType::IDENTIFIER ||
            current_token().get_type() == TokenType::MULTIPLY ||
            current_token().get_type() == TokenType::LEFT_BRACKET ||
            current_token().get_type() == TokenType::HASH ||  // Private members
            is_reserved_word_as_property_name()) {
            // Parse method definition (regular, generator, private, or reserved word method)
            auto method = parse_method_definition();
            if (method) {
                statements.push_back(std::move(method));
            } else {
                // If method parsing fails, skip this token to avoid infinite loop
                advance();
            }
        } else {
            // Skip non-identifier tokens
            advance();
        }
    }
    
    if (!match(TokenType::RIGHT_BRACE)) {
        add_error("Expected '}' to close class body");
        return nullptr;
    }
    
    advance(); // consume '}'
    
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

    // Parse optional class name (class expressions can be anonymous)
    std::unique_ptr<ASTNode> id = nullptr;
    if (current_token().get_type() == TokenType::IDENTIFIER) {
        id = parse_identifier();
        if (!id) return nullptr;
    }

    // Check for extends clause
    std::unique_ptr<Identifier> superclass = nullptr;
    if (match(TokenType::EXTENDS)) {
        advance(); // consume 'extends'

        if (current_token().get_type() != TokenType::IDENTIFIER) {
            add_error("Expected superclass name after 'extends'");
            return nullptr;
        }

        superclass = std::unique_ptr<Identifier>(static_cast<Identifier*>(parse_identifier().release()));
        if (!superclass) return nullptr;
    }

    // Parse class body with methods
    if (!match(TokenType::LEFT_BRACE)) {
        add_error("Expected '{' to start class body");
        return nullptr;
    }

    advance(); // consume '{'

    // Parse class body - methods
    std::vector<std::unique_ptr<ASTNode>> statements;

    // Parse methods within the class
    while (current_token().get_type() != TokenType::RIGHT_BRACE && !at_end()) {
        if (current_token().get_type() == TokenType::IDENTIFIER ||
            current_token().get_type() == TokenType::MULTIPLY ||
            current_token().get_type() == TokenType::LEFT_BRACKET ||
            current_token().get_type() == TokenType::HASH ||  // Private members
            is_reserved_word_as_property_name()) {
            // Parse method definition (regular, generator, private, or reserved word method)
            auto method = parse_method_definition();
            if (method) {
                statements.push_back(std::move(method));
            } else {
                // If method parsing fails, skip this token to avoid infinite loop
                advance();
            }
        } else {
            // Skip non-identifier tokens
            advance();
        }
    }

    if (!match(TokenType::RIGHT_BRACE)) {
        add_error("Expected '}' to close class body");
        return nullptr;
    }

    advance(); // consume '}'

    auto body = std::make_unique<BlockStatement>(std::move(statements), start, get_current_position());

    Position end = get_current_position();

    // Create class with optional name
    if (id) {
        // Named class expression
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
        // Anonymous class expression - use empty identifier
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
    
    // Check for static keyword
    bool is_static = false;
    if (current_token().get_value() == "static") {
        is_static = true;
        advance(); // consume 'static'
    }

    // Check for async keyword
    bool is_async = false;
    if (current_token().get_type() == TokenType::ASYNC) {
        is_async = true;
        advance(); // consume 'async'
    }

    // Check for generator method syntax: *methodName
    bool is_generator = false;
    if (current_token().get_type() == TokenType::MULTIPLY) {
        is_generator = true;
        advance(); // consume '*'
    }

    // Check for getter/setter method syntax: get/set propertyName
    MethodDefinition::Kind method_kind = MethodDefinition::METHOD;
    if (current_token().get_type() == TokenType::IDENTIFIER) {
        std::string token_value = current_token().get_value();
        if (token_value == "get") {
            method_kind = MethodDefinition::GETTER;
            advance(); // consume 'get'
        } else if (token_value == "set") {
            method_kind = MethodDefinition::SETTER;
            advance(); // consume 'set'
        }
    }

    // Parse method name (property name for getter/setter) - support computed properties and private members
    std::unique_ptr<ASTNode> key = nullptr;
    bool computed = false;
    bool is_private = false;

    // Check for private member syntax: #name
    if (current_token().get_type() == TokenType::HASH) {
        is_private = true;
        advance(); // consume '#'

        if (current_token().get_type() != TokenType::IDENTIFIER) {
            add_error("Expected identifier after '#' for private member");
            return nullptr;
        }

        // Create identifier with '#' prefix
        std::string private_name = "#" + current_token().get_value();
        Position start = current_token().get_start();
        Position end = current_token().get_end();
        advance(); // consume identifier
        key = std::make_unique<Identifier>(private_name, start, end);
    } else if (current_token().get_type() == TokenType::IDENTIFIER) {
        key = parse_identifier();
    } else if (is_reserved_word_as_property_name()) {
        // Allow reserved words as method/property names (e.g., get return(), set default())
        std::string name = current_token().get_value();
        Position start = current_token().get_start();
        Position end = current_token().get_end();
        advance(); // consume reserved word
        key = std::make_unique<Identifier>(name, start, end);
    } else if (current_token().get_type() == TokenType::LEFT_BRACKET) {
        // Computed property: [expr]()
        computed = true;
        advance(); // consume '['

        // Handle expressions in computed properties
        if (current_token().get_type() == TokenType::NUMBER) {
            key = parse_number_literal();
        } else if (current_token().get_type() == TokenType::STRING) {
            key = parse_string_literal();
        } else {
            // Try full expression parsing for all cases
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

    // Check if this is a class field (has '=' or ';' instead of '(')
    // Class field syntax: fieldName = value; or [computed] = value;
    if (current_token().get_type() == TokenType::ASSIGN ||
        current_token().get_type() == TokenType::SEMICOLON ||
        current_token().get_type() == TokenType::RIGHT_BRACE) {

        // This is a class field, not a method
        std::unique_ptr<ASTNode> init = nullptr;

        if (current_token().get_type() == TokenType::ASSIGN) {
            advance(); // consume '='
            // Parse field initializer expression
            init = parse_assignment_expression();
            if (!init) {
                add_error("Expected field initializer after '='");
                return nullptr;
            }
        }

        // Optional semicolon after field
        if (current_token().get_type() == TokenType::SEMICOLON) {
            advance();
        }

        // Create an ExpressionStatement to represent the field
        // We'll wrap the assignment in an expression statement
        if (init) {
            auto assignment = std::make_unique<AssignmentExpression>(
                std::move(key),
                AssignmentExpression::Operator::ASSIGN,  // Use simple assignment operator
                std::move(init),
                start,
                get_current_position()
            );
            return std::make_unique<ExpressionStatement>(std::move(assignment), start, get_current_position());
        } else {
            // Field without initializer - just return the key as an expression statement
            return std::make_unique<ExpressionStatement>(std::move(key), start, get_current_position());
        }
    }

    // Finalize method kind (prioritize constructor, then getter/setter, then regular method)
    MethodDefinition::Kind kind = method_kind;
    if (Identifier* key_id = static_cast<Identifier*>(key.get())) {
        if (key_id->get_name() == "constructor") {
            kind = MethodDefinition::CONSTRUCTOR;
        }
    }

    // Parse function parameters and body (this is a method, not a field)
    if (current_token().get_type() != TokenType::LEFT_PAREN) {
        add_error("Expected '(' for method parameters");
        return nullptr;
    }
    
    advance(); // consume '('
    
    // Parse parameters
    std::vector<std::unique_ptr<Parameter>> params;
    while (current_token().get_type() != TokenType::RIGHT_PAREN && !at_end()) {
        Position param_start = get_current_position();
        bool is_rest = false;
        
        // Check for rest parameter syntax: ...args
        if (match(TokenType::ELLIPSIS)) {
            is_rest = true;
            advance(); // consume '...'
        }
        
        std::unique_ptr<Identifier> param_name = nullptr;

        if (current_token().get_type() == TokenType::LEFT_BRACKET) {
            // Array destructuring parameter: [a, b, c]
            advance(); // consume '['

            // Skip through the array destructuring pattern
            std::string destructuring_name = "__array_destructuring_";
            int bracket_count = 1;
            while (bracket_count > 0 && !at_end()) {
                if (current_token().get_type() == TokenType::LEFT_BRACKET) {
                    bracket_count++;
                } else if (current_token().get_type() == TokenType::RIGHT_BRACKET) {
                    bracket_count--;
                } else if (current_token().get_type() == TokenType::IDENTIFIER && bracket_count == 1) {
                    // Collect first identifier for parameter name
                    if (destructuring_name == "__array_destructuring_") {
                        destructuring_name += current_token().get_value();
                    }
                } else if (current_token().get_type() == TokenType::ASSIGN) {
                    // Handle default value assignment within destructuring: [x] = [1]
                    advance(); // consume '='
                    // Skip the default value expression (could be array, object, etc.)
                    if (current_token().get_type() == TokenType::LEFT_BRACKET) {
                        // Array default value: = [1, 2, 3]
                        int inner_bracket_count = 1;
                        advance(); // consume '['
                        while (inner_bracket_count > 0 && !at_end()) {
                            if (current_token().get_type() == TokenType::LEFT_BRACKET) {
                                inner_bracket_count++;
                            } else if (current_token().get_type() == TokenType::RIGHT_BRACKET) {
                                inner_bracket_count--;
                            }
                            if (inner_bracket_count > 0) advance();
                        }
                        if (inner_bracket_count == 0) advance(); // consume final ']'
                        continue; // Don't advance again at end of main loop
                    } else if (current_token().get_type() == TokenType::LEFT_BRACE) {
                        // Object default value: = {a: 1}
                        int inner_brace_count = 1;
                        advance(); // consume '{'
                        while (inner_brace_count > 0 && !at_end()) {
                            if (current_token().get_type() == TokenType::LEFT_BRACE) {
                                inner_brace_count++;
                            } else if (current_token().get_type() == TokenType::RIGHT_BRACE) {
                                inner_brace_count--;
                            }
                            if (inner_brace_count > 0) advance();
                        }
                        if (inner_brace_count == 0) advance(); // consume final '}'
                        continue; // Don't advance again at end of main loop
                    } else {
                        // Simple default value: = 5, = "string", etc.
                        while (!at_end() &&
                               current_token().get_type() != TokenType::COMMA &&
                               current_token().get_type() != TokenType::RIGHT_BRACKET) {
                            advance();
                        }
                        continue; // Don't advance again at end of main loop
                    }
                }
                advance();
            }

            // Create identifier for the destructured parameter
            Position param_pos = get_current_position();
            param_name = std::make_unique<Identifier>(destructuring_name, param_pos, param_pos);
        } else if (current_token().get_type() == TokenType::LEFT_BRACE) {
            // Object destructuring parameter: {a, b, c}
            advance(); // consume '{'

            // Skip through the destructuring pattern
            std::string destructuring_name = "__destructuring_";
            int brace_count = 1;
            while (brace_count > 0 && !at_end()) {
                if (current_token().get_type() == TokenType::LEFT_BRACE) {
                    brace_count++;
                } else if (current_token().get_type() == TokenType::RIGHT_BRACE) {
                    brace_count--;
                } else if (current_token().get_type() == TokenType::IDENTIFIER && brace_count == 1) {
                    // Collect first identifier for parameter name
                    if (destructuring_name == "__destructuring_") {
                        destructuring_name += current_token().get_value();
                    }
                }
                advance();
            }

            // Create identifier for the destructured parameter
            Position param_pos = get_current_position();
            param_name = std::make_unique<Identifier>(destructuring_name, param_pos, param_pos);
        } else if (current_token().get_type() == TokenType::IDENTIFIER) {
            // Regular identifier parameter
            param_name = std::make_unique<Identifier>(current_token().get_value(),
                                                      current_token().get_start(), current_token().get_end());
            advance();
        } else {
            add_error("Expected parameter name or destructuring pattern");
            return nullptr;
        }

        // Check for default parameter syntax: param = value
        // Note: Rest parameters cannot have default values
        std::unique_ptr<ASTNode> default_value = nullptr;
        if (!is_rest && match(TokenType::ASSIGN)) {
            advance(); // consume '='
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
        
        // Rest parameter must be last
        if (is_rest) {
            if (current_token().get_type() != TokenType::RIGHT_PAREN) {
                add_error("Rest parameter must be last formal parameter");
                return nullptr;
            }
            break;
        }
        
        if (current_token().get_type() == TokenType::COMMA) {
            advance(); // consume ','
        } else if (current_token().get_type() != TokenType::RIGHT_PAREN) {
            add_error("Expected ',' or ')' in parameter list");
            return nullptr;
        }
    }
    
    if (!match(TokenType::RIGHT_PAREN)) {
        add_error("Expected ')' after parameters");
        return nullptr;
    }
    advance(); // consume ')'
    
    // Parse method body
    if (current_token().get_type() != TokenType::LEFT_BRACE) {
        add_error("Expected '{' for method body");
        return nullptr;
    }
    
    auto body = parse_block_statement();
    if (!body) {
        add_error("Expected method body");
        return nullptr;
    }
    
    // Create function expression for the method
    auto function_expr = std::make_unique<FunctionExpression>(
        nullptr, // no name for method functions
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

    // Check for generator function (function*)
    bool is_generator = false;
    if (match(TokenType::MULTIPLY)) {
        is_generator = true;
        advance();
    }

    // Parse optional function name
    std::unique_ptr<Identifier> id = nullptr;
    if (current_token().get_type() == TokenType::IDENTIFIER) {
        id = std::make_unique<Identifier>(current_token().get_value(),
                                        current_token().get_start(), current_token().get_end());
        advance();
    }

    // Parse parameter list
    if (!consume(TokenType::LEFT_PAREN)) {
        add_error("Expected '(' after 'function'");
        return nullptr;
    }

    std::vector<std::unique_ptr<Parameter>> params;
    bool has_non_simple_params = false;  // Track if any parameter is non-simple

    while (!match(TokenType::RIGHT_PAREN) && !at_end()) {
        Position param_start = get_current_position();
        bool is_rest = false;

        // Check for rest parameter syntax: ...args
        if (match(TokenType::ELLIPSIS)) {
            is_rest = true;
            has_non_simple_params = true;  // Rest parameters are non-simple
            advance(); // consume '...'
        }

        std::unique_ptr<Identifier> param_name = nullptr;

        if (current_token().get_type() == TokenType::LEFT_BRACKET) {
            // Array destructuring parameter: [a, b, c]
            has_non_simple_params = true;  // Destructuring makes params non-simple
            advance(); // consume '['

            // Skip through the array destructuring pattern
            std::string destructuring_name = "__array_destructuring_";
            int bracket_count = 1;
            while (bracket_count > 0 && !at_end()) {
                if (current_token().get_type() == TokenType::LEFT_BRACKET) {
                    bracket_count++;
                } else if (current_token().get_type() == TokenType::RIGHT_BRACKET) {
                    bracket_count--;
                } else if (current_token().get_type() == TokenType::IDENTIFIER && bracket_count == 1) {
                    // Collect first identifier for parameter name
                    if (destructuring_name == "__array_destructuring_") {
                        destructuring_name += current_token().get_value();
                    }
                } else if (current_token().get_type() == TokenType::ASSIGN) {
                    // Handle default value assignment within destructuring: [x] = [1]
                    advance(); // consume '='
                    // Skip the default value expression (could be array, object, etc.)
                    if (current_token().get_type() == TokenType::LEFT_BRACKET) {
                        // Array default value: = [1, 2, 3]
                        int inner_bracket_count = 1;
                        advance(); // consume '['
                        while (inner_bracket_count > 0 && !at_end()) {
                            if (current_token().get_type() == TokenType::LEFT_BRACKET) {
                                inner_bracket_count++;
                            } else if (current_token().get_type() == TokenType::RIGHT_BRACKET) {
                                inner_bracket_count--;
                            }
                            if (inner_bracket_count > 0) advance();
                        }
                        if (inner_bracket_count == 0) advance(); // consume final ']'
                        continue; // Don't advance again at end of main loop
                    } else if (current_token().get_type() == TokenType::LEFT_BRACE) {
                        // Object default value: = {a: 1}
                        int inner_brace_count = 1;
                        advance(); // consume '{'
                        while (inner_brace_count > 0 && !at_end()) {
                            if (current_token().get_type() == TokenType::LEFT_BRACE) {
                                inner_brace_count++;
                            } else if (current_token().get_type() == TokenType::RIGHT_BRACE) {
                                inner_brace_count--;
                            }
                            if (inner_brace_count > 0) advance();
                        }
                        if (inner_brace_count == 0) advance(); // consume final '}'
                        continue; // Don't advance again at end of main loop
                    } else {
                        // Simple default value: = 5, = "string", etc.
                        while (!at_end() &&
                               current_token().get_type() != TokenType::COMMA &&
                               current_token().get_type() != TokenType::RIGHT_BRACKET) {
                            advance();
                        }
                        continue; // Don't advance again at end of main loop
                    }
                }
                advance();
            }

            // Create identifier for the destructured parameter
            Position param_pos = get_current_position();
            param_name = std::make_unique<Identifier>(destructuring_name, param_pos, param_pos);
        } else if (current_token().get_type() == TokenType::LEFT_BRACE) {
            // Object destructuring parameter: {a, b, c}
            has_non_simple_params = true;  // Destructuring makes params non-simple
            advance(); // consume '{'

            // Skip through the destructuring pattern
            std::string destructuring_name = "__destructuring_";
            int brace_count = 1;
            while (brace_count > 0 && !at_end()) {
                if (current_token().get_type() == TokenType::LEFT_BRACE) {
                    brace_count++;
                } else if (current_token().get_type() == TokenType::RIGHT_BRACE) {
                    brace_count--;
                } else if (current_token().get_type() == TokenType::IDENTIFIER && brace_count == 1) {
                    // Collect first identifier for parameter name
                    if (destructuring_name == "__destructuring_") {
                        destructuring_name += current_token().get_value();
                    }
                }
                advance();
            }

            // Create identifier for the destructured parameter
            Position param_pos = get_current_position();
            param_name = std::make_unique<Identifier>(destructuring_name, param_pos, param_pos);
        } else if (current_token().get_type() == TokenType::IDENTIFIER) {
            // Regular identifier parameter
            param_name = std::make_unique<Identifier>(current_token().get_value(),
                                                      current_token().get_start(), current_token().get_end());
            advance();
        } else {
            add_error("Expected parameter name or destructuring pattern");
            return nullptr;
        }
        
        // Check for default parameter syntax: param = value
        // Note: Rest parameters cannot have default values
        std::unique_ptr<ASTNode> default_value = nullptr;
        if (!is_rest && match(TokenType::ASSIGN)) {
            has_non_simple_params = true;  // Default parameters make params non-simple
            advance(); // consume '='
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
        
        // Rest parameter must be last
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
    
    // Parse function body
    auto body = parse_block_statement();
    if (!body) {
        add_error("Expected function body");
        return nullptr;
    }
    
    // ECMAScript validation: Non-simple parameters cannot be used with strict mode
    if (has_non_simple_params && body) {
        // Check if function body contains "use strict" directive
        BlockStatement* block = static_cast<BlockStatement*>(body.get());
        if (block && !block->get_statements().empty()) {
            // Check first statement for "use strict" directive
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

    // Check if next token is 'function' (async function) or '(' (async arrow function) or identifier (async arrow)
    if (match(TokenType::FUNCTION)) {
        advance(); // consume 'function'
        // Continue with regular async function parsing
    } else if (match(TokenType::LEFT_PAREN)) {
        // This is an async arrow function: async () => {}
        return parse_async_arrow_function(start);
    } else if (match(TokenType::IDENTIFIER)) {
        // This is an async arrow function with single parameter: async x => {}
        return parse_async_arrow_function_single_param(start);
    } else {
        add_error("Expected 'function', '(', or identifier after 'async'");
        return nullptr;
    }

    // Check for async generator function expression (async function*)
    bool is_generator = false;
    if (current_token().get_type() == TokenType::MULTIPLY) {
        advance(); // consume '*'
        is_generator = true;
    }

    // Parse optional function name
    std::unique_ptr<Identifier> id = nullptr;
    if (current_token().get_type() == TokenType::IDENTIFIER) {
        id = std::make_unique<Identifier>(current_token().get_value(),
                                        current_token().get_start(), current_token().get_end());
        advance();
    }
    
    // Parse parameters
    if (!consume(TokenType::LEFT_PAREN)) {
        add_error("Expected '(' after async function name");
        return nullptr;
    }

    std::vector<std::unique_ptr<Parameter>> params;
    bool has_non_simple_params = false;  // Track if any parameter is non-simple

    while (!match(TokenType::RIGHT_PAREN) && !at_end()) {
        Position param_start = current_token().get_start();
        bool is_rest = false;

        // Check for rest parameter syntax: ...args
        if (match(TokenType::ELLIPSIS)) {
            is_rest = true;
            has_non_simple_params = true;  // Rest parameters are non-simple
            advance(); // consume '...'
        }

        std::unique_ptr<Identifier> param_name = nullptr;

        if (current_token().get_type() == TokenType::LEFT_BRACKET) {
            // Array destructuring parameter: [a, b, c]
            has_non_simple_params = true;  // Destructuring makes params non-simple
            advance(); // consume '['

            // Skip through the array destructuring pattern
            std::string destructuring_name = "__array_destructuring_";
            int bracket_count = 1;
            while (bracket_count > 0 && !at_end()) {
                if (current_token().get_type() == TokenType::LEFT_BRACKET) {
                    bracket_count++;
                } else if (current_token().get_type() == TokenType::RIGHT_BRACKET) {
                    bracket_count--;
                } else if (current_token().get_type() == TokenType::IDENTIFIER && bracket_count == 1) {
                    // Collect first identifier for parameter name
                    if (destructuring_name == "__array_destructuring_") {
                        destructuring_name += current_token().get_value();
                    }
                } else if (current_token().get_type() == TokenType::ASSIGN) {
                    // Handle default value assignment within destructuring: [x] = [1]
                    advance(); // consume '='
                    // Skip the default value expression (could be array, object, etc.)
                    if (current_token().get_type() == TokenType::LEFT_BRACKET) {
                        // Array default value: = [1, 2, 3]
                        int inner_bracket_count = 1;
                        advance(); // consume '['
                        while (inner_bracket_count > 0 && !at_end()) {
                            if (current_token().get_type() == TokenType::LEFT_BRACKET) {
                                inner_bracket_count++;
                            } else if (current_token().get_type() == TokenType::RIGHT_BRACKET) {
                                inner_bracket_count--;
                            }
                            if (inner_bracket_count > 0) advance();
                        }
                        if (inner_bracket_count == 0) advance(); // consume final ']'
                        continue; // Don't advance again at end of main loop
                    } else if (current_token().get_type() == TokenType::LEFT_BRACE) {
                        // Object default value: = {a: 1}
                        int inner_brace_count = 1;
                        advance(); // consume '{'
                        while (inner_brace_count > 0 && !at_end()) {
                            if (current_token().get_type() == TokenType::LEFT_BRACE) {
                                inner_brace_count++;
                            } else if (current_token().get_type() == TokenType::RIGHT_BRACE) {
                                inner_brace_count--;
                            }
                            if (inner_brace_count > 0) advance();
                        }
                        if (inner_brace_count == 0) advance(); // consume final '}'
                        continue; // Don't advance again at end of main loop
                    } else {
                        // Simple default value: = 5, = "string", etc.
                        while (!at_end() &&
                               current_token().get_type() != TokenType::COMMA &&
                               current_token().get_type() != TokenType::RIGHT_BRACKET) {
                            advance();
                        }
                        continue; // Don't advance again at end of main loop
                    }
                }
                advance();
            }

            // Create identifier for the destructured parameter
            Position param_pos = get_current_position();
            param_name = std::make_unique<Identifier>(destructuring_name, param_pos, param_pos);
        } else if (current_token().get_type() == TokenType::LEFT_BRACE) {
            // Object destructuring parameter: {a, b, c}
            has_non_simple_params = true;  // Destructuring makes params non-simple
            advance(); // consume '{'

            // Skip through the destructuring pattern
            std::string destructuring_name = "__destructuring_";
            int brace_count = 1;
            while (brace_count > 0 && !at_end()) {
                if (current_token().get_type() == TokenType::LEFT_BRACE) {
                    brace_count++;
                } else if (current_token().get_type() == TokenType::RIGHT_BRACE) {
                    brace_count--;
                } else if (current_token().get_type() == TokenType::IDENTIFIER && brace_count == 1) {
                    // Collect first identifier for parameter name
                    if (destructuring_name == "__destructuring_") {
                        destructuring_name += current_token().get_value();
                    }
                }
                advance();
            }

            // Create identifier for the destructured parameter
            Position param_pos = get_current_position();
            param_name = std::make_unique<Identifier>(destructuring_name, param_pos, param_pos);
        } else if (current_token().get_type() == TokenType::IDENTIFIER) {
            // Regular identifier parameter
            param_name = std::make_unique<Identifier>(current_token().get_value(),
                                                      current_token().get_start(), current_token().get_end());
            advance();
        } else {
            add_error("Expected parameter name or destructuring pattern");
            return nullptr;
        }

        // Check for default parameter syntax: param = value
        // Note: Rest parameters cannot have default values
        std::unique_ptr<ASTNode> default_value = nullptr;
        if (!is_rest && match(TokenType::ASSIGN)) {
            has_non_simple_params = true;  // Default parameters make params non-simple
            advance(); // consume '='
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

        // Rest parameter must be last
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
    
    // Parse function body
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

    // Check for async generator function (async function*)
    bool is_generator = false;
    if (current_token().get_type() == TokenType::MULTIPLY) {
        advance(); // consume '*'
        is_generator = true;
    }

    // Parse function name (required for declarations)
    if (current_token().get_type() != TokenType::IDENTIFIER) {
        add_error("Expected function name after 'async function'");
        return nullptr;
    }
    
    auto id = std::make_unique<Identifier>(current_token().get_value(),
                                        current_token().get_start(), current_token().get_end());
    advance();

    // Parse parameters
    if (!consume(TokenType::LEFT_PAREN)) {
        add_error("Expected '(' after async function name");
        return nullptr;
    }

    std::vector<std::unique_ptr<Parameter>> params;
    bool has_non_simple_params = false;  // Track if any parameter is non-simple

    while (!match(TokenType::RIGHT_PAREN) && !at_end()) {
        Position param_start = current_token().get_start();
        bool is_rest = false;

        // Check for rest parameter syntax: ...args
        if (match(TokenType::ELLIPSIS)) {
            is_rest = true;
            has_non_simple_params = true;  // Rest parameters are non-simple
            advance(); // consume '...'
        }

        std::unique_ptr<Identifier> param_name = nullptr;

        if (current_token().get_type() == TokenType::LEFT_BRACKET) {
            // Array destructuring parameter: [a, b, c]
            has_non_simple_params = true;  // Destructuring makes params non-simple
            advance(); // consume '['

            // Skip through the array destructuring pattern
            std::string destructuring_name = "__array_destructuring_";
            int bracket_count = 1;
            while (bracket_count > 0 && !at_end()) {
                if (current_token().get_type() == TokenType::LEFT_BRACKET) {
                    bracket_count++;
                } else if (current_token().get_type() == TokenType::RIGHT_BRACKET) {
                    bracket_count--;
                } else if (current_token().get_type() == TokenType::IDENTIFIER && bracket_count == 1) {
                    // Collect first identifier for parameter name
                    if (destructuring_name == "__array_destructuring_") {
                        destructuring_name += current_token().get_value();
                    }
                } else if (current_token().get_type() == TokenType::ASSIGN) {
                    // Handle default value assignment within destructuring: [x] = [1]
                    advance(); // consume '='
                    // Skip the default value expression (could be array, object, etc.)
                    if (current_token().get_type() == TokenType::LEFT_BRACKET) {
                        // Array default value: = [1, 2, 3]
                        int inner_bracket_count = 1;
                        advance(); // consume '['
                        while (inner_bracket_count > 0 && !at_end()) {
                            if (current_token().get_type() == TokenType::LEFT_BRACKET) {
                                inner_bracket_count++;
                            } else if (current_token().get_type() == TokenType::RIGHT_BRACKET) {
                                inner_bracket_count--;
                            }
                            if (inner_bracket_count > 0) advance();
                        }
                        if (inner_bracket_count == 0) advance(); // consume final ']'
                        continue; // Don't advance again at end of main loop
                    } else if (current_token().get_type() == TokenType::LEFT_BRACE) {
                        // Object default value: = {a: 1}
                        int inner_brace_count = 1;
                        advance(); // consume '{'
                        while (inner_brace_count > 0 && !at_end()) {
                            if (current_token().get_type() == TokenType::LEFT_BRACE) {
                                inner_brace_count++;
                            } else if (current_token().get_type() == TokenType::RIGHT_BRACE) {
                                inner_brace_count--;
                            }
                            if (inner_brace_count > 0) advance();
                        }
                        if (inner_brace_count == 0) advance(); // consume final '}'
                        continue; // Don't advance again at end of main loop
                    } else {
                        // Simple default value: = 5, = "string", etc.
                        while (!at_end() &&
                               current_token().get_type() != TokenType::COMMA &&
                               current_token().get_type() != TokenType::RIGHT_BRACKET) {
                            advance();
                        }
                        continue; // Don't advance again at end of main loop
                    }
                }
                advance();
            }

            // Create identifier for the destructured parameter
            Position param_pos = get_current_position();
            param_name = std::make_unique<Identifier>(destructuring_name, param_pos, param_pos);
        } else if (current_token().get_type() == TokenType::LEFT_BRACE) {
            // Object destructuring parameter: {a, b, c}
            has_non_simple_params = true;  // Destructuring makes params non-simple
            advance(); // consume '{'

            // Skip through the destructuring pattern
            std::string destructuring_name = "__destructuring_";
            int brace_count = 1;
            while (brace_count > 0 && !at_end()) {
                if (current_token().get_type() == TokenType::LEFT_BRACE) {
                    brace_count++;
                } else if (current_token().get_type() == TokenType::RIGHT_BRACE) {
                    brace_count--;
                } else if (current_token().get_type() == TokenType::IDENTIFIER && brace_count == 1) {
                    // Collect first identifier for parameter name
                    if (destructuring_name == "__destructuring_") {
                        destructuring_name += current_token().get_value();
                    }
                }
                advance();
            }

            // Create identifier for the destructured parameter
            Position param_pos = get_current_position();
            param_name = std::make_unique<Identifier>(destructuring_name, param_pos, param_pos);
        } else if (current_token().get_type() == TokenType::IDENTIFIER) {
            // Regular identifier parameter
            param_name = std::make_unique<Identifier>(current_token().get_value(),
                                                      current_token().get_start(), current_token().get_end());
            advance();
        } else {
            add_error("Expected parameter name or destructuring pattern");
            return nullptr;
        }

        // Check for default parameter syntax: param = value
        // Note: Rest parameters cannot have default values
        std::unique_ptr<ASTNode> default_value = nullptr;
        if (!is_rest && match(TokenType::ASSIGN)) {
            has_non_simple_params = true;  // Default parameters make params non-simple
            advance(); // consume '='
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

        // Rest parameter must be last
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
    
    // Parse function body
    auto body = parse_block_statement();
    if (!body) {
        add_error("Expected async function body");
        return nullptr;
    }
    
    Position end = get_current_position();
    // Create FunctionDeclaration with async flag set to true
    return std::make_unique<FunctionDeclaration>(
        std::move(id), std::move(params),
        std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body.release())),
        start, end, true, false  // is_async = true, is_generator = false
    );
}

std::unique_ptr<ASTNode> Parser::parse_arrow_function() {
    Position start = get_current_position();
    std::vector<std::unique_ptr<Parameter>> params;
    bool has_non_simple_params = false;  // Track non-simple parameters
    
    // Parse parameters
    if (match(TokenType::IDENTIFIER)) {
        // Single parameter without parentheses: x => x + 1
        Position param_start = get_current_position();
        auto param_name = std::make_unique<Identifier>(current_token().get_value(),
                                                      current_token().get_start(), current_token().get_end());
        advance();
        
        // Arrow functions with single param don't support default values without parentheses
        Position param_end = get_current_position();
        auto param = std::make_unique<Parameter>(std::move(param_name), nullptr, false, param_start, param_end);
        params.push_back(std::move(param));
    } else if (match(TokenType::LEFT_PAREN)) {
        // Multiple parameters with parentheses: (x, y) => x + y
        advance(); // consume '('
        
        while (!match(TokenType::RIGHT_PAREN) && !at_end()) {
            // Handle different parameter types
            if (current_token().get_type() == TokenType::IDENTIFIER) {
                // Regular identifier parameter
            } else if (current_token().get_type() == TokenType::LEFT_BRACE) {
                // Object destructuring parameter: {x, y}
                // For now, create a dummy parameter and skip the destructuring pattern
                has_non_simple_params = true;  // Destructuring makes params non-simple
                advance(); // consume '{'
                // Skip until matching '}'
                int brace_count = 1;
                while (brace_count > 0 && !at_end()) {
                    if (current_token().get_type() == TokenType::LEFT_BRACE) {
                        brace_count++;
                    } else if (current_token().get_type() == TokenType::RIGHT_BRACE) {
                        brace_count--;
                    }
                    advance();
                }
                // Create a dummy parameter
                Position param_start = get_current_position();
                auto param_name = std::make_unique<Identifier>("__destructured", param_start, param_start);
                Position param_end = get_current_position();
                auto param = std::make_unique<Parameter>(std::move(param_name), nullptr, false, param_start, param_end);
                params.push_back(std::move(param));
                
                // Skip comma if present
                if (match(TokenType::COMMA)) {
                    advance();
                }
                continue;
            } else if (current_token().get_type() == TokenType::LEFT_BRACKET) {
                // Array destructuring parameter: [a, b]
                has_non_simple_params = true;  // Destructuring makes params non-simple
                advance(); // consume '['
                // Skip until matching ']'
                int bracket_count = 1;
                while (bracket_count > 0 && !at_end()) {
                    if (current_token().get_type() == TokenType::LEFT_BRACKET) {
                        bracket_count++;
                    } else if (current_token().get_type() == TokenType::RIGHT_BRACKET) {
                        bracket_count--;
                    }
                    advance();
                }
                // Create a dummy parameter
                Position param_start = get_current_position();
                auto param_name = std::make_unique<Identifier>("__destructured", param_start, param_start);
                Position param_end = get_current_position();
                auto param = std::make_unique<Parameter>(std::move(param_name), nullptr, false, param_start, param_end);
                params.push_back(std::move(param));
                
                // Skip comma if present
                if (match(TokenType::COMMA)) {
                    advance();
                }
                continue;
            } else {
                // Unknown parameter type, skip it for compatibility
                advance();
                continue;
            }
            
            Position param_start = get_current_position();
            auto param_name = std::make_unique<Identifier>(current_token().get_value(),
                                                          current_token().get_start(), current_token().get_end());
            advance();
            
            // Check for default parameter syntax: param = value
            std::unique_ptr<ASTNode> default_value = nullptr;
            if (match(TokenType::ASSIGN)) {
                has_non_simple_params = true;  // Default values make params non-simple
                advance(); // consume '='
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
    
    // Consume '=>'
    if (!consume(TokenType::ARROW)) {
        add_error("Expected '=>' in arrow function");
        return nullptr;
    }
    
    // Parse body (can be expression or block statement)
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
    
    // Validate strict mode with non-simple parameters (ECMAScript rule)
    if (has_non_simple_params && body) {
        // Check if body is a block statement with "use strict"
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
    // Try to parse parameter list and look for '=>'
    if (!match(TokenType::LEFT_PAREN)) {
        return false;
    }
    
    size_t paren_count = 1;
    size_t lookahead = 1;
    
    // Look ahead to find matching ')' and then check for '=>'
    while (lookahead < tokens_.size() && paren_count > 0) {
        TokenType type = peek_token(lookahead).get_type();
        if (type == TokenType::LEFT_PAREN) {
            paren_count++;
        } else if (type == TokenType::RIGHT_PAREN) {
            paren_count--;
        }
        lookahead++;
    }
    
    // Check if next token after ')' is '=>'
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
    
    // Check for yield* (delegating generator)
    bool is_delegate = false;
    if (match(TokenType::MULTIPLY)) {
        is_delegate = true;
        advance();
    }
    
    // Parse optional argument
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

    // Dynamic import: import(specifier)
    if (!consume(TokenType::LEFT_PAREN)) {
        add_error("Expected '(' after 'import'");
        return nullptr;
    }

    // Parse module specifier expression
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

    // Create a call expression with 'import' as the function name
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
    
    // Check if there's an expression after return
    if (!match(TokenType::SEMICOLON) && !at_end() && 
        current_token().get_start().line == start.line) { // same line
        argument = parse_expression();
        if (!argument) {
            add_error("Invalid return expression");
            return nullptr;
        }
    }
    
    // Consume optional semicolon
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

    // Check for optional label (must be on same line, no line terminator)
    std::string label;
    if (current_token().get_type() == TokenType::IDENTIFIER &&
        current_token().get_start().line == start.line) {
        label = current_token().get_value();
        advance(); // consume label
    }

    // Consume optional semicolon
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

    // Check for optional label (must be on same line, no line terminator)
    std::string label;
    if (current_token().get_type() == TokenType::IDENTIFIER &&
        current_token().get_start().line == start.line) {
        label = current_token().get_value();
        advance(); // consume label
    }

    // Consume optional semicolon
    consume_if_match(TokenType::SEMICOLON);

    Position end = get_current_position();
    return std::make_unique<ContinueStatement>(start, end, label);
}

//=============================================================================
// ParserFactory Implementation
//=============================================================================

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

} // namespace ParserFactory

std::unique_ptr<ASTNode> Parser::parse_object_literal() {
    Position start = get_current_position();
    
    if (!consume(TokenType::LEFT_BRACE)) {
        add_error("Expected '{'");
        return nullptr;
    }
    
    std::vector<std::unique_ptr<ObjectLiteral::Property>> properties;
    
    // Handle empty object {}
    if (match(TokenType::RIGHT_BRACE)) {
        advance();
        Position end = get_current_position();
        return std::make_unique<ObjectLiteral>(std::move(properties), start, end);
    }
    
    // Parse properties
    do {
        // Check for spread element: {...obj}
        if (match(TokenType::ELLIPSIS)) {
            auto spread = parse_spread_element();
            if (!spread) {
                add_error("Invalid spread element in object literal");
                return nullptr;
            }
            properties.push_back(std::make_unique<ObjectLiteral::Property>(
                nullptr, std::move(spread), false, false
            ));
            
            // Continue with next property
            if (match(TokenType::COMMA)) {
                advance();
                continue;
            } else {
                break;
            }
        }
        
        // Check for getter/setter or async method
        ObjectLiteral::PropertyType property_type = ObjectLiteral::PropertyType::Value;
        bool is_async = false;

        // Check for get/set keywords (contextual keywords, so check as identifiers)
        if (match(TokenType::IDENTIFIER)) {
            if (current_token().get_value() == "get") {
                // Lookahead to see if this is getter syntax: get key() {} or get [key]() {}
                // If followed by : then it's a normal property: {get: value}
                size_t saved_pos = current_token_index_;
                advance(); // temporarily consume 'get'

                // For getter syntax, we need a property name (identifier, string, number, or computed [])
                // If get is immediately followed by (, it's a method shorthand get() {}
                bool is_method_shorthand = match(TokenType::LEFT_PAREN);
                bool is_getter_syntax = !is_method_shorthand && (match(TokenType::LEFT_BRACKET) || match(TokenType::IDENTIFIER) || match(TokenType::STRING) || match(TokenType::NUMBER));
                bool is_normal_property = match(TokenType::COLON);

                // Reset position
                current_token_index_ = saved_pos;

                if (is_getter_syntax && !is_normal_property) {
                    property_type = ObjectLiteral::PropertyType::Getter;
                    advance(); // consume 'get'
                }
            } else if (current_token().get_value() == "set") {
                // Same lookahead for setter
                size_t saved_pos = current_token_index_;
                advance(); // temporarily consume 'set'

                // For setter syntax, we need a property name (identifier, string, number, or computed [])
                // If set is immediately followed by (, it's a method shorthand set() {}
                bool is_method_shorthand = match(TokenType::LEFT_PAREN);
                bool is_setter_syntax = !is_method_shorthand && (match(TokenType::LEFT_BRACKET) || match(TokenType::IDENTIFIER) || match(TokenType::STRING) || match(TokenType::NUMBER));
                bool is_normal_property = match(TokenType::COLON);

                // Reset position
                current_token_index_ = saved_pos;

                if (is_setter_syntax && !is_normal_property) {
                    property_type = ObjectLiteral::PropertyType::Setter;
                    advance(); // consume 'set'
                }
            } else if (current_token().get_value() == "async") {
                is_async = true;
                advance(); // consume 'async'
            }
        } else if (match(TokenType::ASYNC)) {
            is_async = true;
            advance(); // consume 'async'
        }

        // Check for generator method: *methodName()
        bool is_generator = false;
        if (match(TokenType::MULTIPLY)) {
            is_generator = true;
            advance(); // consume '*'
        }

        // Parse property key
        std::unique_ptr<ASTNode> key;
        bool computed = false;
        
        if (match(TokenType::LEFT_BRACKET)) {
            // Computed property [expr]: value
            advance(); // consume '['
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
            // Regular property key
            key = parse_identifier();
        } else if (match(TokenType::STRING)) {
            // String property key
            key = parse_string_literal();
        } else if (match(TokenType::NUMBER)) {
            // Numeric property key
            key = parse_number_literal();
        } else if (is_keyword_token(current_token().get_type())) {
            // Reserved keyword as property key (valid in ECMAScript)
            key = std::make_unique<Identifier>(current_token().get_value(),
                                               current_token().get_start(), current_token().get_end());
            advance(); // consume keyword token
        } else {
            add_error("Expected property key");
            return nullptr;
        }
        
        // Check for getter/setter or method syntax
        if (property_type == ObjectLiteral::PropertyType::Getter || property_type == ObjectLiteral::PropertyType::Setter) {
            // Getter/setter must have function syntax: get/set key() {}
            if (!match(TokenType::LEFT_PAREN)) {
                add_error(property_type == ObjectLiteral::PropertyType::Getter ?
                          "Expected '(' after getter property key" :
                          "Expected '(' after setter property key");
                return nullptr;
            }
        }

        // Check for method syntax: key() {} or async key() {} or getter/setter syntax
        if (match(TokenType::LEFT_PAREN)) {
            // This is a method (ES6+ syntax)
            advance(); // consume '('
            
            // Parse parameters
            std::vector<std::unique_ptr<Parameter>> params;
            if (!match(TokenType::RIGHT_PAREN)) {
                do {
                    Position param_start = current_token().get_start();
                    bool is_rest = false;

                    // Check for rest parameter syntax: ...args
                    if (match(TokenType::ELLIPSIS)) {
                        is_rest = true;
                        advance(); // consume '...'
                    }

                    std::unique_ptr<Identifier> param_name = nullptr;

                    if (current_token().get_type() == TokenType::LEFT_BRACKET) {
                        // Array destructuring parameter: [a, b, c]
                        advance(); // consume '['

                        // Skip through the array destructuring pattern
                        std::string destructuring_name = "__array_destructuring_";
                        int bracket_count = 1;
                        while (bracket_count > 0 && !at_end()) {
                            if (current_token().get_type() == TokenType::LEFT_BRACKET) {
                                bracket_count++;
                            } else if (current_token().get_type() == TokenType::RIGHT_BRACKET) {
                                bracket_count--;
                            } else if (current_token().get_type() == TokenType::IDENTIFIER && bracket_count == 1) {
                                // Collect first identifier for parameter name
                                if (destructuring_name == "__array_destructuring_") {
                                    destructuring_name += current_token().get_value();
                                }
                            }
                            advance();
                        }

                        // Create identifier for the destructured parameter
                        Position param_pos = get_current_position();
                        param_name = std::make_unique<Identifier>(destructuring_name, param_pos, param_pos);
                    } else if (current_token().get_type() == TokenType::LEFT_BRACE) {
                        // Object destructuring parameter: {a, b, c}
                        advance(); // consume '{'

                        // Skip through the destructuring pattern
                        std::string destructuring_name = "__destructuring_";
                        int brace_count = 1;
                        while (brace_count > 0 && !at_end()) {
                            if (current_token().get_type() == TokenType::LEFT_BRACE) {
                                brace_count++;
                            } else if (current_token().get_type() == TokenType::RIGHT_BRACE) {
                                brace_count--;
                            } else if (current_token().get_type() == TokenType::IDENTIFIER && brace_count == 1) {
                                // Collect first identifier for parameter name
                                if (destructuring_name == "__destructuring_") {
                                    destructuring_name += current_token().get_value();
                                }
                            }
                            advance();
                        }

                        // Create identifier for the destructured parameter
                        Position param_pos = get_current_position();
                        param_name = std::make_unique<Identifier>(destructuring_name, param_pos, param_pos);
                    } else if (current_token().get_type() == TokenType::IDENTIFIER) {
                        // Regular identifier parameter
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

                    // Check for default parameter: param = value
                    std::unique_ptr<ASTNode> default_value = nullptr;
                    if (match(TokenType::ASSIGN)) {
                        advance(); // consume '='
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
            
            // Parse method body
            if (!match(TokenType::LEFT_BRACE)) {
                add_error("Expected '{' for method body");
                return nullptr;
            }
            
            auto body = parse_block_statement();
            if (!body) {
                add_error("Expected method body");
                return nullptr;
            }
            
            // Create method (function expression)
            std::unique_ptr<ASTNode> method_value;
            if (is_async) {
                // Create AsyncFunctionExpression with proper constructor arguments
                auto block_body = std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body.release()));
                method_value = std::make_unique<AsyncFunctionExpression>(
                    nullptr, // no name for method
                    std::move(params),
                    std::move(block_body),
                    key->get_start(),
                    get_current_position()
                );
            } else if (is_generator) {
                // Create GeneratorExpression for generator methods
                auto block_body = std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body.release()));
                method_value = std::make_unique<FunctionExpression>(
                    nullptr, // no name for method
                    std::move(params),
                    std::move(block_body),
                    key->get_start(),
                    get_current_position()
                );
                // TODO: Add proper GeneratorExpression support when available
            } else {
                // Create regular FunctionExpression with proper constructor arguments
                auto block_body = std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body.release()));
                method_value = std::make_unique<FunctionExpression>(
                    nullptr, // no name for method
                    std::move(params),
                    std::move(block_body),
                    key->get_start(),
                    get_current_position()
                );
            }
            
            // Create property with function value (method, getter, or setter)
            ObjectLiteral::PropertyType final_type = property_type;
            if (final_type == ObjectLiteral::PropertyType::Value) {
                final_type = ObjectLiteral::PropertyType::Method; // Convert value to method for function syntax
            }
            auto property = std::make_unique<ObjectLiteral::Property>(
                std::move(key), std::move(method_value), computed, final_type
            );
            properties.push_back(std::move(property));
        } else {
            // Check for ES6 shorthand property syntax: {x} is equivalent to {x: x}
            if (match(TokenType::COMMA) || match(TokenType::RIGHT_BRACE)) {
                // This is shorthand property syntax - key without value means key: key
                // The key should be an identifier for shorthand to work
                if (!key) {
                    add_error("Invalid shorthand property");
                    return nullptr;
                }
                
                // Create identifier for the value (same as key)
                if (auto* identifier_key = dynamic_cast<Identifier*>(key.get())) {
                    // Create a new identifier with the same name as the key for the value
                    auto value = std::make_unique<Identifier>(
                        identifier_key->get_name(),
                        identifier_key->get_start(),
                        identifier_key->get_end()
                    );
                    
                    // Create property with shorthand (key and value have same identifier)
                    auto property = std::make_unique<ObjectLiteral::Property>(
                        std::move(key), std::move(value), computed, false
                    );
                    properties.push_back(std::move(property));
                } else {
                    add_error("Shorthand properties can only be used with identifier keys");
                    return nullptr;
                }
            } else {
                // Regular property with colon syntax: key: value
                if (!consume(TokenType::COLON)) {
                    add_error("Expected ':' after property key");
                    return nullptr;
                }
                
                // Parse property value
                auto value = parse_assignment_expression();
                if (!value) {
                    add_error("Expected property value");
                    return nullptr;
                }
                
                // Create property
                auto property = std::make_unique<ObjectLiteral::Property>(
                    std::move(key), std::move(value), computed, false
                );
                properties.push_back(std::move(property));
            }
        }
        
        // Continue if there's a comma
        if (match(TokenType::COMMA)) {
            advance();
            // Allow trailing comma
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
    
    // Handle empty array []
    if (match(TokenType::RIGHT_BRACKET)) {
        advance();
        Position end = get_current_position();
        return std::make_unique<ArrayLiteral>(std::move(elements), start, end);
    }
    
    // Parse array elements
    do {
        // Handle sparse arrays (e.g., [1, , 3])
        if (match(TokenType::COMMA)) {
            // Add undefined for empty slots
            elements.push_back(std::make_unique<UndefinedLiteral>(get_current_position(), get_current_position()));
        } else {
            // Check for spread element
            if (match(TokenType::ELLIPSIS)) {
                auto spread = parse_spread_element();
                if (!spread) {
                    add_error("Invalid spread element");
                    return nullptr;
                }
                elements.push_back(std::move(spread));
            } else {
                // Parse element expression
                auto element = parse_assignment_expression();
                if (!element) {
                    add_error("Expected array element");
                    return nullptr;
                }
                elements.push_back(std::move(element));
            }
        }
        
        // Continue if there's a comma
        if (match(TokenType::COMMA)) {
            advance();
            // Allow trailing comma
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

//=============================================================================
// Stage 9: Error Handling & Advanced Control Flow Parsing
//=============================================================================

std::unique_ptr<ASTNode> Parser::parse_try_statement() {
    Position start = current_token().get_start();
    advance(); // consume 'try'
    
    // Parse try block (must be a block statement)
    auto try_block = parse_block_statement();
    if (!try_block) {
        add_error("Expected block statement after 'try'");
        return nullptr;
    }
    
    std::unique_ptr<ASTNode> catch_clause = nullptr;
    std::unique_ptr<ASTNode> finally_block = nullptr;
    
    // Parse optional catch clause
    if (match(TokenType::CATCH)) {
        catch_clause = parse_catch_clause();
        if (!catch_clause) {
            add_error("Invalid catch clause");
            return nullptr;
        }
    }
    
    // Parse optional finally block
    if (match(TokenType::FINALLY)) {
        advance(); // consume 'finally'
        finally_block = parse_block_statement();
        if (!finally_block) {
            add_error("Expected block statement after 'finally'");
            return nullptr;
        }
    }
    
    // Must have either catch or finally (or both)
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
    advance(); // consume 'catch'
    
    // Parse parameter: catch (e)
    if (!consume(TokenType::LEFT_PAREN)) {
        add_error("Expected '(' after 'catch'");
        return nullptr;
    }
    
    if (!match(TokenType::IDENTIFIER)) {
        add_error("Expected identifier in catch clause");
        return nullptr;
    }
    
    std::string parameter_name = current_token().get_value();
    advance(); // consume identifier
    
    if (!consume(TokenType::RIGHT_PAREN)) {
        add_error("Expected ')' after catch parameter");
        return nullptr;
    }
    
    // Parse catch body (must be a block statement)
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
    advance(); // consume 'throw'
    
    // No line terminator allowed between 'throw' and expression
    auto expression = parse_expression();
    if (!expression) {
        add_error("Expected expression after 'throw'");
        return nullptr;
    }
    
    // Consume optional semicolon (ASI - Automatic Semicolon Insertion)
    consume_if_match(TokenType::SEMICOLON);
    
    Position end = get_current_position();
    return std::make_unique<ThrowStatement>(std::move(expression), start, end);
}

std::unique_ptr<ASTNode> Parser::parse_switch_statement() {
    Position start = current_token().get_start();
    advance(); // consume 'switch'
    
    // Parse discriminant: switch (expr)
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
    
    // Parse switch body: { case ... default ... }
    if (!consume(TokenType::LEFT_BRACE)) {
        add_error("Expected '{' after switch expression");
        return nullptr;
    }
    
    std::vector<std::unique_ptr<ASTNode>> cases;
    
    while (!match(TokenType::RIGHT_BRACE) && !at_end()) {
        if (match(TokenType::CASE)) {
            // Parse case clause
            Position case_start = current_token().get_start();
            advance(); // consume 'case'
            
            auto test = parse_expression();
            if (!test) {
                add_error("Expected expression after 'case'");
                return nullptr;
            }
            
            if (!consume(TokenType::COLON)) {
                add_error("Expected ':' after case expression");
                return nullptr;
            }
            
            // Parse consequent statements until next case/default/}
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
            // Parse default clause
            Position default_start = current_token().get_start();
            advance(); // consume 'default'
            
            if (!consume(TokenType::COLON)) {
                add_error("Expected ':' after 'default'");
                return nullptr;
            }
            
            // Parse consequent statements
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

//=============================================================================
// Stage 10: Module parsing
//=============================================================================

std::unique_ptr<ASTNode> Parser::parse_import_statement() {
    Position start = current_token().get_start();
    advance(); // consume 'import'
    
    // Check for different import patterns
    if (match(TokenType::MULTIPLY)) {
        // import * as name from "module"
        advance(); // consume '*'
        
        if (current_token().get_type() != TokenType::IDENTIFIER || current_token().get_value() != "as") {
            add_error("Expected 'as' after '*' in import statement");
            return nullptr;
        }
        advance(); // consume 'as'
        
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
        advance(); // consume 'from'
        
        if (current_token().get_type() != TokenType::STRING) {
            add_error("Expected string literal after 'from'");
            return nullptr;
        }
        std::string module_source = current_token().get_value();
        // advance(); // REMOVED: This was causing token position corruption after namespace imports
        
        Position end = get_current_position();
        return std::make_unique<ImportStatement>(namespace_alias, module_source, start, end);
    }
    
    if (match(TokenType::LEFT_BRACE)) {
        // import { name1, name2 as alias } from "module"
        advance(); // consume '{'
        
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
        advance(); // consume '}'
        
        if (current_token().get_type() != TokenType::FROM) {
            add_error("Expected 'from' in import statement");
            return nullptr;
        }
        advance(); // consume 'from'
        
        if (current_token().get_type() != TokenType::STRING) {
            add_error("Expected string literal after 'from'");
            return nullptr;
        }
        std::string module_source = current_token().get_value();
        // advance(); // REMOVED: This was causing token position corruption after named imports
        
        Position end = get_current_position();
        return std::make_unique<ImportStatement>(std::move(specifiers), module_source, start, end);
    }
    
    if (match(TokenType::IDENTIFIER)) {
        // import name from "module" (default import) OR import defaultThing, { namedThing } from "module" (mixed import)
        std::string default_alias = current_token().get_value();
        advance();
        
        // Check if this is a mixed import (default + named)
        if (match(TokenType::COMMA)) {
            advance(); // consume ','
            
            if (!match(TokenType::LEFT_BRACE)) {
                add_error("Expected '{' after ',' in mixed import statement");
                return nullptr;
            }
            advance(); // consume '{'
            
            // Parse named imports
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
            advance(); // consume '}'
            
            if (current_token().get_type() != TokenType::FROM) {
                add_error("Expected 'from' in mixed import statement");
                return nullptr;
            }
            advance(); // consume 'from'
            
            if (current_token().get_type() != TokenType::STRING) {
                add_error("Expected string literal after 'from'");
                return nullptr;
            }
            std::string module_source = current_token().get_value();
            
            Position end = get_current_position();
            // Create a mixed import statement with both default and named imports
            return std::make_unique<ImportStatement>(default_alias, std::move(specifiers), module_source, start, end);
        }
        
        // Regular default import
        if (current_token().get_type() != TokenType::FROM) {
            add_error("Expected 'from' in import statement");
            return nullptr;
        }
        advance(); // consume 'from'
        
        if (current_token().get_type() != TokenType::STRING) {
            add_error("Expected string literal after 'from'");
            return nullptr;
        }
        std::string module_source = current_token().get_value();
        // advance(); // REMOVED: This was causing token position corruption after default imports
        
        Position end = get_current_position();
        return std::make_unique<ImportStatement>(default_alias, module_source, true, start, end);
    }
    
    add_error("Invalid import statement syntax");
    return nullptr;
}

std::unique_ptr<ASTNode> Parser::parse_export_statement() {
    Position start = current_token().get_start();
    advance(); // consume 'export'
    
    if (match(TokenType::DEFAULT)) {
        // export default expression
        advance(); // consume 'default'
        
        auto default_export = parse_assignment_expression();
        if (!default_export) {
            add_error("Expected expression after 'export default'");
            return nullptr;
        }
        
        Position end = get_current_position();
        return std::make_unique<ExportStatement>(std::move(default_export), true, start, end);
    }
    
    if (match(TokenType::LEFT_BRACE)) {
        // export { name1, name2 as alias } [from "module"]
        advance(); // consume '{'
        
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
        advance(); // consume '}'
        
        // Check for re-export: export { name } from "module"
        if (match(TokenType::FROM)) {
            advance(); // consume 'from'
            
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
    
    // export declaration (function, var, etc.)
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
    
    // Allow both IDENTIFIER and DEFAULT tokens for import specifiers
    if (!match(TokenType::IDENTIFIER) && !match(TokenType::DEFAULT)) {
        add_error("Expected identifier or 'default' in import specifier");
        return nullptr;
    }
    
    std::string imported_name = current_token().get_value();
    std::string local_name = imported_name; // Default to same name
    advance();
    
    // Check for 'as' alias
    if (match(TokenType::IDENTIFIER) && current_token().get_value() == "as") {
        advance(); // consume 'as'
        
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
    std::string exported_name = local_name; // Default to same name
    advance();
    
    // Check for 'as' alias
    if (match(TokenType::IDENTIFIER) && current_token().get_value() == "as") {
        advance(); // consume 'as'
        
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
    // Removed all depth checking to achieve unlimited destructuring

    Position start = get_current_position();
    
    if (current_token().get_type() == TokenType::LEFT_BRACKET) {
        // Array destructuring: [a, b, c]
        advance(); // consume '['
        
        std::vector<std::unique_ptr<Identifier>> targets;
        std::vector<std::pair<size_t, std::unique_ptr<ASTNode>>> default_exprs; // index -> default expr
        
        while (!match(TokenType::RIGHT_BRACKET) && !at_end()) {
            if (current_token().get_type() == TokenType::ELLIPSIS) {
                // Handle rest element: [...rest]
                advance(); // consume '...'
                
                if (current_token().get_type() != TokenType::IDENTIFIER) {
                    add_error("Expected identifier after '...' in array destructuring");
                    return nullptr;
                }
                
                auto rest_id = std::make_unique<Identifier>(
                    "..." + current_token().get_value(), // Mark as rest with prefix
                    current_token().get_start(),
                    current_token().get_end()
                );
                targets.push_back(std::move(rest_id));
                advance();
                
                // Rest element must be last
                if (match(TokenType::COMMA)) {
                    add_error("Rest element must be last element in array destructuring");
                    return nullptr;
                }
                break; // End parsing after rest element
                
            } else if (current_token().get_type() == TokenType::IDENTIFIER) {
                auto id = std::make_unique<Identifier>(
                    current_token().get_value(),
                    current_token().get_start(),
                    current_token().get_end()
                );
                targets.push_back(std::move(id));
                advance();
                
                // Check for default value: [a = 5, b = 10]
                if (match(TokenType::ASSIGN)) {
                    advance(); // consume '='
                    auto default_expr = parse_assignment_expression();
                    if (!default_expr) {
                        add_error("Expected expression after '=' in array destructuring");
                        return nullptr;
                    }
                    // Store default value with the current target index
                    size_t target_index = targets.size() - 1;
                    default_exprs.emplace_back(target_index, std::move(default_expr));
                }
            } else if (current_token().get_type() == TokenType::LEFT_BRACKET) {
                // NESTED DESTRUCTURING FIX: Handle nested array destructuring [a, [b, c]]
                advance(); // consume opening '['
                
                // Parse nested identifiers and store them with special prefix
                std::string nested_vars = "";
                while (!match(TokenType::RIGHT_BRACKET) && !at_end()) {
                    if (current_token().get_type() == TokenType::IDENTIFIER) {
                        if (!nested_vars.empty()) nested_vars += ",";
                        nested_vars += current_token().get_value();
                        advance();
                    } else if (current_token().get_type() == TokenType::COMMA) {
                        advance(); // consume comma
                    } else {
                        // Skip unsupported tokens
                        advance();
                    }
                }
                
                if (!consume(TokenType::RIGHT_BRACKET)) {
                    add_error("Expected ']' in nested destructuring");
                    return nullptr;
                }
                
                
                // Create a special identifier that contains the nested variable names
                auto nested_placeholder = std::make_unique<Identifier>(
                    "__nested_vars:" + nested_vars, // Store the actual variable names
                    current_token().get_start(),
                    current_token().get_end()
                );
                targets.push_back(std::move(nested_placeholder));

                // Check for default value after nested destructuring: [[x, y] = defaultValue]
                if (match(TokenType::ASSIGN)) {
                    advance(); // consume '='
                    auto default_expr = parse_assignment_expression();
                    if (!default_expr) {
                        add_error("Expected expression after '=' in nested array destructuring");
                        return nullptr;
                    }
                    // Store default value with the current target index
                    size_t target_index = targets.size() - 1;
                    default_exprs.emplace_back(target_index, std::move(default_expr));
                }
            } else if (current_token().get_type() == TokenType::COMMA) {
                // Handle skipping elements: [a, , c]
                // Create a placeholder identifier for skipped element
                auto placeholder = std::make_unique<Identifier>(
                    "", // Empty name indicates skipped element
                    current_token().get_start(),
                    current_token().get_end()
                );
                targets.push_back(std::move(placeholder));
                // Don't advance here, let the comma handling below do it
            } else {
                add_error("Expected identifier or ',' in array destructuring");
                return nullptr;
            }
            
            if (match(TokenType::COMMA)) {
                advance(); // consume ','
            } else {
                // If no comma, we expect the closing bracket
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
        
        // Add default values for array destructuring
        for (auto& default_pair : default_exprs) {
            destructuring->add_default_value(default_pair.first, std::move(default_pair.second));
        }
        
        return std::move(destructuring);
        
    } else if (current_token().get_type() == TokenType::LEFT_BRACE) {
        // Object destructuring: {a, b, c}
        advance(); // consume '{'
        
        std::vector<std::unique_ptr<Identifier>> targets;
        std::vector<std::pair<std::string, std::string>> property_mappings; // original_name -> variable_name
        
        while (!match(TokenType::RIGHT_BRACE) && !at_end()) {
            if (current_token().get_type() == TokenType::ELLIPSIS) {
                // Handle object rest element: {...rest}
                advance(); // consume '...'
                
                if (current_token().get_type() != TokenType::IDENTIFIER) {
                    add_error("Expected identifier after '...' in object destructuring");
                    return nullptr;
                }
                
                auto rest_id = std::make_unique<Identifier>(
                    "..." + current_token().get_value(), // Mark as rest with prefix
                    current_token().get_start(),
                    current_token().get_end()
                );
                targets.push_back(std::move(rest_id));
                advance();
                
                // Rest element must be last
                if (match(TokenType::COMMA)) {
                    add_error("Rest element must be last element in object destructuring");
                    return nullptr;
                }
                break; // End parsing after rest element
                
            } else if (current_token().get_type() == TokenType::IDENTIFIER) {
                auto id = std::make_unique<Identifier>(
                    current_token().get_value(),
                    current_token().get_start(),
                    current_token().get_end()
                );
                targets.push_back(std::move(id));
                advance();
                
                // Check for property renaming: user: newName or nested: {prop}
                if (match(TokenType::COLON)) {
                    advance(); // consume ':'
                    
                    // Handle nested destructuring: {a: {b}} or renaming: {a: b}
                    if (match(TokenType::LEFT_BRACE)) {
                        // Parse nested object destructuring with optimized pattern handling
                        auto nested = parse_destructuring_pattern(depth + 1);
                        if (!nested) {
                            add_error("Invalid nested object destructuring");
                            return nullptr;
                        }

                        // Simplified pattern extraction for unlimited depth
                        std::string nested_vars = extract_nested_variable_names(nested.get());
                        printf("DEBUG: extracted nested_vars: '%s'\n", nested_vars.c_str());

                        // Get the original property name before replacing the target
                        std::string original_property_name = targets.back()->get_name();

                        // FIX: Keep the original target - don't replace with complex names
                        // The target should remain as the simple property name (e.g., 'a')
                        // Property mappings handle the complex navigation

                        // FIX: Extract the first property name from nested destructuring
                        // For {a: {b: {c}}}, extract "b" from the nested destructuring and create "b:__nested:c"
                        std::string proper_pattern = nested_vars;
                        if (auto nested_destructuring = dynamic_cast<DestructuringAssignment*>(nested.get())) {
                            // Try to get the property name from property mappings (this is the key we navigate to)
                            const auto& mappings = nested_destructuring->get_property_mappings();
                            printf("DEBUG: Found %zu property mappings in nested destructuring\n", mappings.size());

                            std::string property_name = "";
                            if (!mappings.empty()) {
                                // Get the first property name (this is what we navigate to)
                                property_name = mappings[0].property_name;
                                printf("DEBUG: First property mapping: '%s' -> '%s'\n",
                                       mappings[0].property_name.c_str(), mappings[0].variable_name.c_str());
                            } else {
                                // Fallback: try to extract from the identifier naming pattern
                                const auto& targets = nested_destructuring->get_targets();
                                if (!targets.empty()) {
                                    std::string first_target = targets[0]->get_name();
                                    printf("DEBUG: No property mappings, using target: '%s'\n", first_target.c_str());

                                    // If it's a simple identifier without __nested prefix, use it as property name
                                    if (first_target.find("__nested") == std::string::npos) {
                                        property_name = first_target;
                                    }
                                }
                            }

                            // Build proper navigation pattern: "property:__nested:rest"
                            if (!property_name.empty()) {
                                printf("DEBUG: Using property name: '%s'\n", property_name.c_str());
                                size_t nested_pos = nested_vars.find("__nested:");
                                if (nested_pos != std::string::npos && nested_pos + 9 < nested_vars.length()) {
                                    // FIX: For multi-level patterns like 'c:__nested:d',
                                    // build 'b:c:__nested:d' to preserve the complete navigation path
                                    proper_pattern = property_name + ":" + nested_vars;
                                    printf("DEBUG: Built complete navigation pattern: '%s'\n", proper_pattern.c_str());
                                } else {
                                    // FIX: For nested destructuring, always use __nested: prefix
                                    // For {a: {b}}, we need '__nested:b' to indicate nested extraction
                                    // For {a: {b: newName}}, we need 'b:newName' for renaming within nested object
                                    printf("DEBUG: Comparing property_name='%s' with nested_vars='%s'\n", property_name.c_str(), nested_vars.c_str());
                                    if (property_name == nested_vars) {
                                        // Simple nested access - use __nested: prefix
                                        proper_pattern = "__nested:" + nested_vars;
                                        printf("DEBUG: Built nested access pattern: '%s'\n", proper_pattern.c_str());
                                    } else {
                                        // Property renaming within nested object OR multi-level navigation
                                        proper_pattern = property_name + ":" + nested_vars;
                                        printf("DEBUG: Built nested renaming/navigation pattern: '%s'\n", proper_pattern.c_str());
                                    }
                                }
                            }
                        }
                        property_mappings.emplace_back(original_property_name, proper_pattern);

                    } else if (match(TokenType::LEFT_BRACKET)) {
                        // Parse nested array destructuring with optimized handling
                        auto nested = parse_destructuring_pattern(depth + 1);
                        if (!nested) {
                            add_error("Invalid nested array destructuring");
                            return nullptr;
                        }

                        //Simplified pattern extraction
                        std::string nested_vars = extract_nested_variable_names(nested.get());

                        // Get the original property name before replacing the target
                        std::string original_property_name = targets.back()->get_name();

                        // Create placeholder identifier
                        auto nested_id = std::make_unique<Identifier>(
                            "__nested_array:" + nested_vars,
                            nested->get_start(),
                            nested->get_end()
                        );

                        // Replace the last target
                        targets.pop_back();
                        targets.push_back(std::move(nested_id));

                        // Store the property mapping: original property -> nested array identifier
                        property_mappings.emplace_back(original_property_name, "__nested_array:" + nested_vars);
                    } else if (match(TokenType::IDENTIFIER)) {
                        // Property renaming: {oldName: newName}
                        std::string new_name = current_token().get_value();
                        Position new_pos = current_token().get_start();
                        Position new_end = current_token().get_end();
                        
                        // Get the original property name from the last target
                        std::string original_name = targets.back()->get_name();
                        
                        // Replace the last target with the new name
                        targets.pop_back();
                        auto new_id = std::make_unique<Identifier>(new_name, new_pos, new_end);
                        targets.push_back(std::move(new_id));
                        
                        // Store the property mapping: original property -> new variable name
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
                advance(); // consume ','
            } else {
                // If no comma, we expect the closing brace
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
        
        // Add property mappings for renaming
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
    
    advance(); // consume '...'
    
    // Parse the argument
    auto argument = parse_assignment_expression();
    if (!argument) {
        add_error("Expected expression after '...'");
        return nullptr;
    }
    
    Position end = get_current_position();
    return std::make_unique<SpreadElement>(std::move(argument), start, end);
}

//=============================================================================
// JSX Parsing Implementation
//=============================================================================

std::unique_ptr<ASTNode> Parser::parse_jsx_element() {
    Position start = current_token().get_start();
    
    if (!consume(TokenType::LESS_THAN)) {
        add_error("Expected '<' at start of JSX element");
        return nullptr;
    }
    
    // Parse tag name
    if (current_token().get_type() != TokenType::IDENTIFIER) {
        add_error("Expected JSX tag name");
        return nullptr;
    }
    
    std::string tag_name = current_token().get_value();
    advance();
    
    // Parse attributes
    std::vector<std::unique_ptr<ASTNode>> attributes;
    while (current_token().get_type() == TokenType::IDENTIFIER) {
        auto attr = parse_jsx_attribute();
        if (attr) {
            attributes.push_back(std::move(attr));
        }
    }
    
    // Check for self-closing tag
    if (match(TokenType::DIVIDE)) {
        advance(); // consume '/'
        if (!consume(TokenType::GREATER_THAN)) {
            add_error("Expected '>' after '/' in self-closing JSX tag");
            return nullptr;
        }
        Position end = get_current_position();
        return std::make_unique<JSXElement>(tag_name, std::move(attributes), 
                                            std::vector<std::unique_ptr<ASTNode>>(), true, start, end);
    }
    
    // Expect closing '>'
    if (!consume(TokenType::GREATER_THAN)) {
        add_error("Expected '>' after JSX opening tag");
        return nullptr;
    }
    
    // Parse children
    std::vector<std::unique_ptr<ASTNode>> children;
    while (current_token().get_type() != TokenType::EOF_TOKEN) {
        // Check for closing tag
        if (match(TokenType::LESS_THAN)) {
            // Peek ahead to see if it's a closing tag
            size_t saved_pos = current_token_index_;
            advance(); // consume '<'
            if (match(TokenType::DIVIDE)) {
                // This is a closing tag, restore position and break
                current_token_index_ = saved_pos;
                break;
            } else {
                // Not a closing tag, restore position and parse as nested element
                current_token_index_ = saved_pos;
            }
        }
        
        
        if (match(TokenType::LEFT_BRACE)) {
            // JSX expression
            auto expr = parse_jsx_expression();
            if (expr) {
                children.push_back(std::move(expr));
            }
        } else if (match(TokenType::LESS_THAN)) {
            // Nested JSX element
            auto nested = parse_jsx_element();
            if (nested) {
                children.push_back(std::move(nested));
            }
        } else {
            // JSX text
            auto text = parse_jsx_text();
            if (text) {
                children.push_back(std::move(text));
            }
        }
    }
    
    // Parse closing tag
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
    
    // Collect text until we hit JSX syntax
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
    
    // Trim whitespace
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
            // String literal value
            value = parse_string_literal();
        } else if (match(TokenType::LEFT_BRACE)) {
            // JSX expression value
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
    // Look for "use strict" directive at the beginning
    // Process any leading directives - they must all be string literals
    while (!at_end() && current_token().get_type() == TokenType::STRING) {
        std::string str_value = current_token().get_value();
        
        // Check if this is "use strict"
        if (str_value == "\"use strict\"" || str_value == "'use strict'") {
            options_.strict_mode = true;
            advance(); // consume the string
            
            // Expect semicolon after directive
            if (match(TokenType::SEMICOLON)) {
                advance();
            }
            
            // Continue processing other directives
            continue;
        }
        
        // This is some other directive, consume it
        advance();
        if (match(TokenType::SEMICOLON)) {
            advance();
        }
    }
}

// Parse async arrow function: async (params) => { body }
std::unique_ptr<ASTNode> Parser::parse_async_arrow_function(Position start) {
    std::vector<std::unique_ptr<Parameter>> params;
    bool has_non_simple_params = false;

    // Parameters must be in parentheses for async arrow functions
    if (!consume(TokenType::LEFT_PAREN)) {
        add_error("Expected '(' for async arrow function parameters");
        return nullptr;
    }

    // Parse parameters (similar to regular arrow function)
    while (!match(TokenType::RIGHT_PAREN) && !at_end()) {
        Position param_start = current_token().get_start();
        bool is_rest = false;

        // Check for rest parameter: ...param
        if (match(TokenType::ELLIPSIS)) {
            is_rest = true;
            has_non_simple_params = true;
            advance();
        }

        // Parse parameter name
        std::unique_ptr<Identifier> param_name = nullptr;
        if (current_token().get_type() == TokenType::IDENTIFIER) {
            param_name = std::make_unique<Identifier>(current_token().get_value(),
                                                      current_token().get_start(), current_token().get_end());
            advance();
        } else {
            add_error("Expected parameter name");
            return nullptr;
        }

        // Check for default parameter: param = value
        std::unique_ptr<ASTNode> default_value = nullptr;
        if (!is_rest && match(TokenType::ASSIGN)) {
            has_non_simple_params = true;
            advance(); // consume '='
            default_value = parse_assignment_expression();
            if (!default_value) {
                add_error("Invalid default parameter value");
                return nullptr;
            }
        }

        Position param_end = get_current_position();
        auto param = std::make_unique<Parameter>(std::move(param_name), std::move(default_value), is_rest, param_start, param_end);
        params.push_back(std::move(param));

        // Rest parameter must be last
        if (is_rest) {
            if (!match(TokenType::RIGHT_PAREN)) {
                add_error("Rest parameter must be last formal parameter");
                return nullptr;
            }
            break;
        }

        // Check for comma between parameters
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

    // Expect arrow: =>
    if (!consume(TokenType::ARROW)) {
        add_error("Expected '=>' for async arrow function");
        return nullptr;
    }

    // Parse function body
    std::unique_ptr<ASTNode> body = nullptr;
    if (match(TokenType::LEFT_BRACE)) {
        // Block body: async () => { statements }
        body = parse_block_statement();
    } else {
        // Expression body: async () => expression
        auto expr = parse_assignment_expression();
        if (!expr) {
            add_error("Expected function body");
            return nullptr;
        }

        // Wrap expression in return statement for async arrow functions
        Position ret_start = expr->get_start();
        Position ret_end = expr->get_end();
        auto return_stmt = std::make_unique<ReturnStatement>(std::move(expr), ret_start, ret_end);

        // Create block statement containing the return
        std::vector<std::unique_ptr<ASTNode>> statements;
        statements.push_back(std::move(return_stmt));
        body = std::make_unique<BlockStatement>(std::move(statements), ret_start, ret_end);
    }

    if (!body) {
        add_error("Expected async arrow function body");
        return nullptr;
    }

    Position end = get_current_position();

    // Create async arrow function expression
    // Note: We use AsyncFunctionExpression with null identifier for arrow functions
    return std::make_unique<AsyncFunctionExpression>(
        nullptr, // No identifier for arrow functions
        std::move(params),
        std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body.release())),
        start, end
    );
}

std::unique_ptr<ASTNode> Parser::parse_async_arrow_function_single_param(Position start) {
    std::vector<std::unique_ptr<Parameter>> params;

    // Parse single identifier parameter: async x => {}
    if (current_token().get_type() != TokenType::IDENTIFIER) {
        add_error("Expected identifier for async arrow function parameter");
        return nullptr;
    }

    // Create parameter from identifier
    auto param_name = std::make_unique<Identifier>(current_token().get_value(),
                                                   current_token().get_start(), current_token().get_end());
    Position param_end = current_token().get_end();
    advance(); // consume identifier

    // Single parameter with no default value
    auto param = std::make_unique<Parameter>(std::move(param_name), nullptr, false, start, param_end);
    params.push_back(std::move(param));

    // Expect arrow: =>
    if (!consume(TokenType::ARROW)) {
        add_error("Expected '=>' for async arrow function");
        return nullptr;
    }

    // Parse function body (same as normal async arrow function)
    std::unique_ptr<ASTNode> body = nullptr;
    if (match(TokenType::LEFT_BRACE)) {
        // Block body: async x => { statements }
        body = parse_block_statement();
    } else {
        // Expression body: async x => expression
        auto expr = parse_assignment_expression();
        if (!expr) {
            add_error("Expected function body");
            return nullptr;
        }

        // Wrap expression in return statement for async arrow functions
        Position ret_start = expr->get_start();
        Position ret_end = expr->get_end();
        auto return_stmt = std::make_unique<ReturnStatement>(std::move(expr), ret_start, ret_end);

        // Create block statement containing the return
        std::vector<std::unique_ptr<ASTNode>> statements;
        statements.push_back(std::move(return_stmt));
        body = std::make_unique<BlockStatement>(std::move(statements), ret_start, ret_end);
    }

    if (!body) {
        add_error("Expected async arrow function body");
        return nullptr;
    }

    Position end = get_current_position();

    // Create async arrow function expression
    return std::make_unique<AsyncFunctionExpression>(
        nullptr, // No identifier for arrow functions
        std::move(params),
        std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body.release())),
        start, end
    );
}

// Extract variable names from nested destructuring patterns
std::string Parser::extract_nested_variable_names(ASTNode* node) {
    if (!node) {
        return "";
    }

    // CRITICAL FIX: Generate proper nested patterns for infinite depth
    std::string result = generate_proper_nested_pattern(node, 0);
    return result;
}

std::string Parser::generate_proper_nested_pattern(ASTNode* node, int depth) {
    if (!node) return "";

    printf("DEBUG PARSER: generate_proper_nested_pattern called with depth %d, node type %d\n", depth, (int)node->get_type());

    if (node->get_type() == ASTNode::Type::IDENTIFIER) {
        // Base case: just a variable name
        auto* id = static_cast<Identifier*>(node);
        return id->get_name();
    }
    else if (node->get_type() == ASTNode::Type::OBJECT_LITERAL) {
        // Object literal: recurse into each property's value
        auto* obj = static_cast<ObjectLiteral*>(node);
        std::vector<std::string> nested_vars;

        for (const auto& prop : obj->get_properties()) {
            // Get the property name (key)
            std::string prop_name = "";
            if (prop->key && prop->key->get_type() == ASTNode::Type::IDENTIFIER) {
                prop_name = static_cast<Identifier*>(prop->key.get())->get_name();
            }

            // Generate pattern for the value with increased depth
            std::string value_pattern = generate_proper_nested_pattern(prop->value.get(), depth + 1);
            if (!value_pattern.empty()) {
                // FIX: Include property name in pattern for correct navigation
                std::string prefixed_pattern = value_pattern;
                if (depth > 0 && !prop_name.empty()) {
                    // Include property name with __nested: for proper navigation
                    prefixed_pattern = prop_name + ":__nested:" + value_pattern;
                } else if (!prop_name.empty()) {
                    // First level - just use property name
                    prefixed_pattern = prop_name + ":" + value_pattern;
                } else {
                    prefixed_pattern = value_pattern;
                }
                nested_vars.push_back(prefixed_pattern);
            }
        }

        // Join multiple variables
        std::string result;
        for (size_t i = 0; i < nested_vars.size(); ++i) {
            if (i > 0) result += ",";
            result += nested_vars[i];
        }
        return result;
    }
    else if (node->get_type() == ASTNode::Type::DESTRUCTURING_ASSIGNMENT) {
        // Destructuring assignment: use property mappings for complete patterns
        auto* destructuring = static_cast<DestructuringAssignment*>(node);
        const auto& mappings = destructuring->get_property_mappings();

        printf("DEBUG: Found %zu property mappings in nested destructuring\n", mappings.size());

        if (!mappings.empty()) {
            // Use property mappings for complete patterns
            std::vector<std::string> nested_vars;
            for (const auto& mapping : mappings) {
                // Return just the variable pattern, not the complete mapping
                printf("DEBUG: First property mapping: '%s' -> '%s'\n", mapping.property_name.c_str(), mapping.variable_name.c_str());
                nested_vars.push_back(mapping.variable_name);
                break; // For now, just take the first one for debugging
            }

            // Join multiple variables
            std::string result;
            for (size_t i = 0; i < nested_vars.size(); ++i) {
                if (i > 0) result += ",";
                result += nested_vars[i];
            }
            return result;
        } else {
            // No property mappings - use targets (for simple cases)
            printf("DEBUG: No property mappings, using target: '%s'\n",
                   !destructuring->get_targets().empty() ? destructuring->get_targets()[0]->get_name().c_str() : "none");
            std::vector<std::string> nested_vars;
            for (const auto& target : destructuring->get_targets()) {
                std::string target_pattern = generate_proper_nested_pattern(target.get(), depth);
                if (!target_pattern.empty()) {
                    nested_vars.push_back(target_pattern);
                }
            }

            // Join multiple variables
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
        // Handle nested destructuring assignment - extract variable names from targets
        auto* destructuring = static_cast<DestructuringAssignment*>(node);
        for (const auto& target : destructuring->get_targets()) {
            extract_variable_names_recursive(target.get(), names);
        }
    }
    else if (node->get_type() == ASTNode::Type::OBJECT_LITERAL) {
        auto* obj = static_cast<ObjectLiteral*>(node);
        for (const auto& prop : obj->get_properties()) {
            // In destructuring {a: {b}}, we want the variable names from the value side
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


} // namespace Quanta