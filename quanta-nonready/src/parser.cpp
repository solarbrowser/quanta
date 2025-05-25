//<---------QUANTA JS ENGINE - PARSER IMPLEMENTATION--------->
// Stage 1: Core Engine & Runtime - Parser (ES6 grammar)
// Purpose: Convert tokens into Abstract Syntax Tree (AST)
// Max Lines: 5000 (Current: ~400)

#include "../include/parser.h"
#include <stdexcept>

namespace Quanta {

//<---------PARSER CONSTRUCTOR--------->
Parser::Parser(const std::vector<Token>& tokens, ErrorHandler& errorHandler)
    : tokens(tokens), currentToken(0), errorHandler(errorHandler) {}

//<---------TOKEN NAVIGATION--------->
Token& Parser::current() {
    if (currentToken >= tokens.size()) {
        static Token eofToken(TokenType::EOF_TOKEN, "", 0, 0, 0);
        return eofToken;
    }
    return tokens[currentToken];
}

Token& Parser::peek(size_t offset) {
    size_t pos = currentToken + offset;
    if (pos >= tokens.size()) {
        static Token eofToken(TokenType::EOF_TOKEN, "", 0, 0, 0);
        return eofToken;
    }
    return tokens[pos];
}

bool Parser::match(TokenType type) {
    if (check(type)) {
        advance();
        return true;
    }
    return false;
}

bool Parser::check(TokenType type) {
    return current().type == type;
}

void Parser::advance() {
    if (currentToken < tokens.size()) {
        currentToken++;
    }
}

void Parser::consume(TokenType type, const std::string& message) {
    if (!check(type)) {
        errorHandler.reportSyntaxError(message, current().line, current().column);
        throw SyntaxException(message);
    }
    advance();
}

//<---------PROGRAM PARSING--------->
std::unique_ptr<ProgramNode> Parser::parseProgram() {
    auto program = std::make_unique<ProgramNode>();
    
    while (!check(TokenType::EOF_TOKEN)) {
        try {
            auto stmt = parseStatement();
            if (stmt) {
                program->statements.push_back(std::move(stmt));
            }
        } catch (const SyntaxException& e) {
            // Skip to next statement on error
            while (!check(TokenType::EOF_TOKEN) && !check(TokenType::SEMICOLON) && !check(TokenType::NEWLINE)) {
                advance();
            }
            if (match(TokenType::SEMICOLON) || match(TokenType::NEWLINE)) {
                // Continue parsing
            }
        }
    }
    
    return program;
}

//<---------STATEMENT PARSING--------->
std::unique_ptr<ASTNode> Parser::parseStatement() {
    // Skip newlines
    while (match(TokenType::NEWLINE)) {}
    
    if (check(TokenType::LET) || check(TokenType::CONST) || check(TokenType::VAR)) {
        return parseVariableDeclaration();
    }
    
    if (check(TokenType::FUNCTION)) {
        return parseFunctionDeclaration();
    }
    
    if (check(TokenType::LBRACE)) {
        return parseBlockStatement();
    }
    
    if (check(TokenType::IF)) {
        return parseIfStatement();
    }
    
    if (check(TokenType::WHILE)) {
        return parseWhileStatement();
    }
    
    if (check(TokenType::FOR)) {
        return parseForStatement();
    }
    
    if (check(TokenType::RETURN)) {
        return parseReturnStatement();
    }
    
    return parseExpressionStatement();
}

//<---------VARIABLE DECLARATION--------->
std::unique_ptr<ASTNode> Parser::parseVariableDeclaration() {
    Token kindToken = current();
    advance(); // consume let/const/var
    
    if (!check(TokenType::IDENTIFIER)) {
        errorHandler.reportSyntaxError("Expected identifier after " + kindToken.value, current().line, current().column);
        throw SyntaxException("Expected identifier");
    }
    
    std::string name = current().value;
    advance();
    
    std::unique_ptr<ASTNode> initializer = nullptr;
    if (match(TokenType::ASSIGN)) {
        initializer = parseExpression();
    }
    
    match(TokenType::SEMICOLON); // Optional semicolon
    
    return std::make_unique<VariableDeclarationNode>(kindToken.value, name, std::move(initializer));
}

//<---------FUNCTION DECLARATION--------->
std::unique_ptr<ASTNode> Parser::parseFunctionDeclaration() {
    // For now, just skip function parsing in Stage 1
    errorHandler.reportSyntaxError("Function declarations not implemented in Stage 1", current().line, current().column);
    throw SyntaxException("Function declarations not supported");
}

//<---------BLOCK STATEMENT--------->
std::unique_ptr<ASTNode> Parser::parseBlockStatement() {
    auto block = std::make_unique<BlockStatementNode>();
    
    consume(TokenType::LBRACE, "Expected '{'");
    
    while (!check(TokenType::RBRACE) && !check(TokenType::EOF_TOKEN)) {
        auto stmt = parseStatement();
        if (stmt) {
            block->statements.push_back(std::move(stmt));
        }
    }
      consume(TokenType::RBRACE, "Expected '}'");
    
    return block;
}

//<---------CONTROL FLOW STATEMENTS--------->
std::unique_ptr<ASTNode> Parser::parseIfStatement() {
    // Stage 1: Skip if statements for now
    errorHandler.reportSyntaxError("If statements not implemented in Stage 1", current().line, current().column);
    throw SyntaxException("If statements not supported");
}

std::unique_ptr<ASTNode> Parser::parseWhileStatement() {
    // Stage 1: Skip while statements for now
    errorHandler.reportSyntaxError("While statements not implemented in Stage 1", current().line, current().column);
    throw SyntaxException("While statements not supported");
}

std::unique_ptr<ASTNode> Parser::parseForStatement() {
    // Stage 1: Skip for statements for now
    errorHandler.reportSyntaxError("For statements not implemented in Stage 1", current().line, current().column);
    throw SyntaxException("For statements not supported");
}

std::unique_ptr<ASTNode> Parser::parseReturnStatement() {
    // Stage 1: Skip return statements for now
    errorHandler.reportSyntaxError("Return statements not implemented in Stage 1", current().line, current().column);
    throw SyntaxException("Return statements not supported");
}

//<---------EXPRESSION STATEMENT--------->
std::unique_ptr<ASTNode> Parser::parseExpressionStatement() {
    auto expr = parseExpression();
    match(TokenType::SEMICOLON); // Optional semicolon
    return std::make_unique<ExpressionStatementNode>(std::move(expr));
}

//<---------EXPRESSION PARSING--------->
std::unique_ptr<ASTNode> Parser::parseExpression() {
    return parseAssignment();
}

std::unique_ptr<ASTNode> Parser::parseAssignment() {
    auto expr = parseLogicalOr();
    
    if (match(TokenType::ASSIGN)) {
        auto value = parseAssignment();
        return std::make_unique<AssignmentExpressionNode>(std::move(expr), std::move(value));
    }
    
    return expr;
}

std::unique_ptr<ASTNode> Parser::parseLogicalOr() {
    // For Stage 1, skip to equality
    return parseEquality();
}

std::unique_ptr<ASTNode> Parser::parseLogicalAnd() {
    // For Stage 1, skip to equality
    return parseEquality();
}

std::unique_ptr<ASTNode> Parser::parseEquality() {
    auto expr = parseComparison();
    
    while (match(TokenType::EQUALS) || match(TokenType::NOT_EQUALS)) {
        Token op = tokens[currentToken - 1];
        auto right = parseComparison();
        expr = std::make_unique<BinaryExpressionNode>(std::move(expr), op.value, std::move(right));
    }
    
    return expr;
}

std::unique_ptr<ASTNode> Parser::parseComparison() {
    auto expr = parseTerm();
    
    while (match(TokenType::GREATER_THAN) || match(TokenType::LESS_THAN)) {
        Token op = tokens[currentToken - 1];
        auto right = parseTerm();
        expr = std::make_unique<BinaryExpressionNode>(std::move(expr), op.value, std::move(right));
    }
    
    return expr;
}

std::unique_ptr<ASTNode> Parser::parseTerm() {
    auto expr = parseFactor();
    
    while (match(TokenType::PLUS) || match(TokenType::MINUS)) {
        Token op = tokens[currentToken - 1];
        auto right = parseFactor();
        expr = std::make_unique<BinaryExpressionNode>(std::move(expr), op.value, std::move(right));
    }
    
    return expr;
}

std::unique_ptr<ASTNode> Parser::parseFactor() {
    auto expr = parseUnary();
    
    while (match(TokenType::MULTIPLY) || match(TokenType::DIVIDE) || match(TokenType::MODULO)) {
        Token op = tokens[currentToken - 1];
        auto right = parseUnary();
        expr = std::make_unique<BinaryExpressionNode>(std::move(expr), op.value, std::move(right));
    }
    
    return expr;
}

std::unique_ptr<ASTNode> Parser::parseUnary() {
    if (match(TokenType::MINUS) || match(TokenType::PLUS)) {
        Token op = tokens[currentToken - 1];
        auto expr = parseUnary();
        return std::make_unique<UnaryExpressionNode>(op.value, std::move(expr));
    }
    
    return parsePrimary();
}

std::unique_ptr<ASTNode> Parser::parsePrimary() {
    if (match(TokenType::NUMBER)) {
        double value = std::stod(tokens[currentToken - 1].value);
        return std::make_unique<NumberLiteralNode>(value);
    }
    
    if (match(TokenType::STRING)) {
        std::string value = tokens[currentToken - 1].value;
        return std::make_unique<StringLiteralNode>(value);
    }
    
    if (match(TokenType::BOOLEAN)) {
        // Handle boolean values - for now just return as string
        std::string value = tokens[currentToken - 1].value;
        return std::make_unique<StringLiteralNode>(value);
    }
    
    if (match(TokenType::IDENTIFIER)) {
        std::string name = tokens[currentToken - 1].value;
        return std::make_unique<IdentifierNode>(name);
    }
    
    if (match(TokenType::LPAREN)) {
        auto expr = parseExpression();
        consume(TokenType::RPAREN, "Expected ')' after expression");
        return expr;
    }
    
    errorHandler.reportSyntaxError("Unexpected token: " + current().value, current().line, current().column);
    throw SyntaxException("Unexpected token");
}

} // namespace Quanta
