#ifndef QUANTA_PARSER_H
#define QUANTA_PARSER_H

#include <vector>
#include <memory>
#include <stdexcept>
#include <string>
#include "token.h"
#include "ast.h"

namespace quanta {

/**
 * Error thrown during parsing
 */
class ParserError : public std::runtime_error {
public:
    ParserError(const std::string& message, int line, int column);
    int get_line() const;
    int get_column() const;

private:
    int line_;
    int column_;
};

/**
 * Parser class for the Quanta JavaScript Engine
 */
class Parser {
public:
    /**
     * Create a new parser
     * @param tokens Vector of tokens from the lexer
     */
    explicit Parser(const std::vector<Token>& tokens);
    
    /**
     * Parse the tokens into an AST
     * @return The root node of the AST (Program)
     */
    std::shared_ptr<Program> parse();

private:
    // Token sequence and current position
    std::vector<Token> tokens_;
    size_t current_ = 0;
    
    // Statement parsing methods
    std::shared_ptr<Statement> statement();
    std::shared_ptr<Statement> declaration();
    std::shared_ptr<VariableDeclaration> variable_declaration();
    std::shared_ptr<FunctionDeclaration> function_declaration();
    std::shared_ptr<Statement> if_statement();
    std::shared_ptr<Statement> for_statement();
    std::shared_ptr<Statement> while_statement();
    std::shared_ptr<Statement> return_statement();
    std::shared_ptr<BlockStatement> block_statement();
    std::shared_ptr<Statement> expression_statement();
    
    // Expression parsing methods (recursive descent with precedence climbing)
    std::shared_ptr<Expression> expression();
    std::shared_ptr<Expression> assignment();
    std::shared_ptr<Expression> logical_or();
    std::shared_ptr<Expression> logical_and();
    std::shared_ptr<Expression> equality();
    std::shared_ptr<Expression> comparison();
    std::shared_ptr<Expression> addition();
    std::shared_ptr<Expression> multiplication();
    std::shared_ptr<Expression> unary();
    std::shared_ptr<Expression> call();
    std::shared_ptr<Expression> primary();
    
    // Utility methods
    std::shared_ptr<FunctionExpression> function_expression();
    std::shared_ptr<Expression> finish_call(std::shared_ptr<Expression> callee);
    std::vector<std::shared_ptr<Identifier>> parse_formal_parameters();
    
    // Helper methods for token handling
    bool match(const std::vector<TokenType>& types);
    bool check(TokenType type) const;
    Token advance();
    bool is_at_end() const;
    Token peek() const;
    Token previous() const;
    Token consume(TokenType type, const std::string& message);
    
    // Error handling
    void synchronize();
    ParserError error(const std::string& message);
    ParserError error(const Token& token, const std::string& message);
};

} // namespace quanta

#endif // QUANTA_PARSER_H 