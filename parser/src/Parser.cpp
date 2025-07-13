#include "../include/Parser.h"
#include <algorithm>

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
}

Parser::Parser(TokenSequence tokens, const ParseOptions& options)
    : tokens_(std::move(tokens)), options_(options), current_token_index_(0) {
}

std::unique_ptr<Program> Parser::parse_program() {
    std::vector<std::unique_ptr<ASTNode>> statements;
    Position start = get_current_position();
    
    while (!at_end()) {
        try {
            auto statement = parse_statement();
            if (statement) {
                statements.push_back(std::move(statement));
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
            
        case TokenType::FUNCTION:
            return parse_function_declaration();
            
        case TokenType::RETURN:
            return parse_return_statement();
            
        case TokenType::TRY:
            return parse_try_statement();
            
        case TokenType::THROW:
            return parse_throw_statement();
            
        case TokenType::SWITCH:
            return parse_switch_statement();
            
        case TokenType::IMPORT:
            return parse_import_statement();
            
        case TokenType::EXPORT:
            return parse_export_statement();
            
        default:
            return parse_expression_statement();
    }
}

std::unique_ptr<ASTNode> Parser::parse_expression() {
    return parse_assignment_expression();
}

std::unique_ptr<ASTNode> Parser::parse_assignment_expression() {
    auto left = parse_conditional_expression();
    if (!left) return nullptr;
    
    if (is_assignment_operator(current_token().get_type())) {
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
    // For Stage 2, just delegate to logical OR
    return parse_logical_or_expression();
}

std::unique_ptr<ASTNode> Parser::parse_logical_or_expression() {
    return parse_binary_expression(
        [this]() { return parse_logical_and_expression(); },
        {TokenType::LOGICAL_OR}
    );
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
        {TokenType::LESS_THAN, TokenType::GREATER_THAN, TokenType::LESS_EQUAL, TokenType::GREATER_EQUAL}
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
    // Handle 'new' expression
    if (current_token().get_type() == TokenType::NEW) {
        Position start = current_token().get_start();
        advance(); // consume 'new'
        
        auto constructor = parse_member_expression();
        if (!constructor) {
            add_error("Expected constructor expression after 'new'");
            return nullptr;
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
        return std::make_unique<NewExpression>(std::move(constructor), std::move(arguments), start, end);
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
    auto expr = parse_member_expression();
    if (!expr) return nullptr;
    
    while (match(TokenType::LEFT_PAREN)) {
        Position start = expr->get_start();
        advance(); // consume '('
        
        std::vector<std::unique_ptr<ASTNode>> arguments;
        
        if (!match(TokenType::RIGHT_PAREN)) {
            do {
                auto arg = parse_assignment_expression();
                if (!arg) {
                    add_error("Expected argument in function call");
                    break;
                }
                arguments.push_back(std::move(arg));
            } while (consume_if_match(TokenType::COMMA));
        }
        
        if (!consume(TokenType::RIGHT_PAREN)) {
            add_error("Expected ')' after function arguments");
            return expr;
        }
        
        Position end = get_current_position();
        expr = std::make_unique<CallExpression>(std::move(expr), std::move(arguments), start, end);
    }
    
    return expr;
}

std::unique_ptr<ASTNode> Parser::parse_member_expression() {
    auto expr = parse_primary_expression();
    if (!expr) return nullptr;
    
    while (match(TokenType::DOT) || match(TokenType::LEFT_BRACKET)) {
        Position start = expr->get_start();
        
        if (match(TokenType::DOT)) {
            advance(); // consume '.'
            
            if (!match(TokenType::IDENTIFIER)) {
                add_error("Expected property name after '.'");
                return expr;
            }
            
            auto property = parse_identifier();
            if (!property) return expr;
            
            Position end = property->get_end();
            expr = std::make_unique<MemberExpression>(
                std::move(expr), std::move(property), false, start, end
            );
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
        case TokenType::UNDEFINED:
            return parse_undefined_literal();
        case TokenType::IDENTIFIER:
            return parse_identifier();
        case TokenType::LEFT_PAREN:
            return parse_parenthesized_expression();
        case TokenType::FUNCTION:
            return parse_function_expression();
        case TokenType::LEFT_BRACE:
            return parse_object_literal();
        case TokenType::LEFT_BRACKET:
            return parse_array_literal();
        default:
            add_error("Unexpected token: " + token.get_value());
            return nullptr;
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
    
    return std::make_unique<StringLiteral>(value, start, end);
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

//=============================================================================
// Statement Parsing Implementation
//=============================================================================

std::unique_ptr<ASTNode> Parser::parse_variable_declaration() {
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
    
    // Consume semicolon
    consume_if_match(TokenType::SEMICOLON);
    
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
    
    if (!consume(TokenType::LEFT_PAREN)) {
        add_error("Expected '(' after 'for'");
        return nullptr;
    }
    
    // Parse init (can be variable declaration or expression)
    std::unique_ptr<ASTNode> init = nullptr;
    if (!match(TokenType::SEMICOLON)) {
        if (match(TokenType::VAR) || match(TokenType::LET) || match(TokenType::CONST)) {
            init = parse_variable_declaration();
        } else {
            init = parse_expression();
        }
        if (!init) {
            add_error("Expected initialization in for loop");
            return nullptr;
        }
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

std::unique_ptr<ASTNode> Parser::parse_expression_statement() {
    auto expr = parse_expression();
    if (!expr) {
        return nullptr;
    }
    
    Position start = expr->get_start();
    Position end = expr->get_end();
    
    // Consume optional semicolon
    consume_if_match(TokenType::SEMICOLON);
    
    return std::make_unique<ExpressionStatement>(std::move(expr), start, end);
}

std::unique_ptr<ASTNode> Parser::parse_function_declaration() {
    Position start = get_current_position();
    
    if (!consume(TokenType::FUNCTION)) {
        add_error("Expected 'function'");
        return nullptr;
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
    
    std::vector<std::unique_ptr<Identifier>> params;
    while (!match(TokenType::RIGHT_PAREN) && !at_end()) {
        if (current_token().get_type() != TokenType::IDENTIFIER) {
            add_error("Expected parameter name");
            return nullptr;
        }
        
        auto param = std::make_unique<Identifier>(current_token().get_value(),
                                                current_token().get_start(), current_token().get_end());
        params.push_back(std::move(param));
        advance();
        
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
    
    Position end = get_current_position();
    return std::make_unique<FunctionDeclaration>(
        std::move(id), std::move(params), 
        std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body.release())),
        start, end
    );
}

std::unique_ptr<ASTNode> Parser::parse_function_expression() {
    Position start = get_current_position();
    
    if (!consume(TokenType::FUNCTION)) {
        add_error("Expected 'function'");
        return nullptr;
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
    
    std::vector<std::unique_ptr<Identifier>> params;
    while (!match(TokenType::RIGHT_PAREN) && !at_end()) {
        if (current_token().get_type() != TokenType::IDENTIFIER) {
            add_error("Expected parameter name");
            return nullptr;
        }
        
        auto param = std::make_unique<Identifier>(current_token().get_value(),
                                                current_token().get_start(), current_token().get_end());
        params.push_back(std::move(param));
        advance();
        
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
    
    Position end = get_current_position();
    return std::make_unique<FunctionExpression>(
        std::move(id), std::move(params),
        std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body.release())),
        start, end
    );
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
        } else {
            add_error("Expected property key");
            return nullptr;
        }
        
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
            // Parse element expression
            auto element = parse_assignment_expression();
            if (!element) {
                add_error("Expected array element");
                return nullptr;
            }
            elements.push_back(std::move(element));
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
        
        if (!match(TokenType::IDENTIFIER) || current_token().get_value() != "as") {
            add_error("Expected 'as' after '*' in import statement");
            return nullptr;
        }
        advance(); // consume 'as'
        
        if (!match(TokenType::IDENTIFIER)) {
            add_error("Expected identifier after 'as'");
            return nullptr;
        }
        std::string namespace_alias = current_token().get_value();
        advance();
        
        if (!match(TokenType::FROM)) {
            add_error("Expected 'from' in import statement");
            return nullptr;
        }
        advance(); // consume 'from'
        
        if (!match(TokenType::STRING)) {
            add_error("Expected string literal after 'from'");
            return nullptr;
        }
        std::string module_source = current_token().get_value();
        advance();
        
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
        
        if (!match(TokenType::FROM)) {
            add_error("Expected 'from' in import statement");
            return nullptr;
        }
        advance(); // consume 'from'
        
        if (!match(TokenType::STRING)) {
            add_error("Expected string literal after 'from'");
            return nullptr;
        }
        std::string module_source = current_token().get_value();
        advance();
        
        Position end = get_current_position();
        return std::make_unique<ImportStatement>(std::move(specifiers), module_source, start, end);
    }
    
    if (match(TokenType::IDENTIFIER)) {
        // import name from "module" (default import)
        std::string default_alias = current_token().get_value();
        advance();
        
        if (!match(TokenType::FROM)) {
            add_error("Expected 'from' in import statement");
            return nullptr;
        }
        advance(); // consume 'from'
        
        if (!match(TokenType::STRING)) {
            add_error("Expected string literal after 'from'");
            return nullptr;
        }
        std::string module_source = current_token().get_value();
        advance();
        
        Position end = get_current_position();
        return std::make_unique<ImportStatement>(default_alias, module_source, true, start, end);
    }
    
    add_error("Invalid import statement syntax");
    return nullptr;
}

std::unique_ptr<ASTNode> Parser::parse_export_statement() {
    Position start = current_token().get_start();
    advance(); // consume 'export'
    
    if (match(TokenType::IDENTIFIER) && current_token().get_value() == "default") {
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
    
    if (!match(TokenType::IDENTIFIER)) {
        add_error("Expected identifier in import specifier");
        return nullptr;
    }
    
    std::string imported_name = current_token().get_value();
    std::string local_name = imported_name; // Default to same name
    advance();
    
    // Check for 'as' alias
    if (match(TokenType::IDENTIFIER) && current_token().get_value() == "as") {
        advance(); // consume 'as'
        
        if (!match(TokenType::IDENTIFIER)) {
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
        
        if (!match(TokenType::IDENTIFIER)) {
            add_error("Expected identifier after 'as'");
            return nullptr;
        }
        exported_name = current_token().get_value();
        advance();
    }
    
    Position end = get_current_position();
    return std::make_unique<ExportSpecifier>(local_name, exported_name, start, end);
}

} // namespace Quanta