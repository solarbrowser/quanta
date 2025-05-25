//<---------QUANTA JS ENGINE - PARSER HEADER--------->
// Stage 1: Core Engine & Runtime - Parser (ES6 grammar)
// Purpose: Convert tokens into Abstract Syntax Tree (AST)
// Max Lines: 5000 (Current: ~150)

#ifndef QUANTA_PARSER_H
#define QUANTA_PARSER_H

#include "lexer.h"
#include "ast.h"
#include "error.h"
#include <memory>

namespace Quanta {

//<---------PARSER CLASS--------->
class Parser {
private:
    std::vector<Token> tokens;
    size_t currentToken;
    ErrorHandler& errorHandler;
    
    Token& current();
    Token& peek(size_t offset = 1);
    bool match(TokenType type);
    bool check(TokenType type);
    void advance();
    void consume(TokenType type, const std::string& message);
    
    // Parsing methods (recursive descent)
    std::unique_ptr<ASTNode> parseStatement();
    std::unique_ptr<ASTNode> parseExpression();
    std::unique_ptr<ASTNode> parseAssignment();
    std::unique_ptr<ASTNode> parseLogicalOr();
    std::unique_ptr<ASTNode> parseLogicalAnd();
    std::unique_ptr<ASTNode> parseEquality();
    std::unique_ptr<ASTNode> parseComparison();
    std::unique_ptr<ASTNode> parseTerm();
    std::unique_ptr<ASTNode> parseFactor();
    std::unique_ptr<ASTNode> parseUnary();
    std::unique_ptr<ASTNode> parsePrimary();
    
    // Statement parsing
    std::unique_ptr<ASTNode> parseVariableDeclaration();
    std::unique_ptr<ASTNode> parseFunctionDeclaration();
    std::unique_ptr<ASTNode> parseBlockStatement();
    std::unique_ptr<ASTNode> parseIfStatement();
    std::unique_ptr<ASTNode> parseWhileStatement();
    std::unique_ptr<ASTNode> parseForStatement();
    std::unique_ptr<ASTNode> parseReturnStatement();
    std::unique_ptr<ASTNode> parseExpressionStatement();
    
public:
    Parser(const std::vector<Token>& tokens, ErrorHandler& errorHandler);
    std::unique_ptr<ProgramNode> parseProgram();
};

} // namespace Quanta

#endif // QUANTA_PARSER_H
