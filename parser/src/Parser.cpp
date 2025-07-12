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
    // For Stage 2, no postfix operators yet
    return parse_call_expression();
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
           type == TokenType::DELETE;
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

} // namespace Quanta