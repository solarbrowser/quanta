#ifndef QUANTA_LEXER_H
#define QUANTA_LEXER_H

#include <string>
#include <vector>
#include <stdexcept>
#include "token.h"

namespace quanta {

/**
 * Error thrown during lexical analysis
 */
class LexerError : public std::runtime_error {
public:
    LexerError(const std::string& message, int line, int column);
    int get_line() const;
    int get_column() const;

private:
    int line_;
    int column_;
};

/**
 * Lexer class for the Quanta JavaScript Engine
 */
class Lexer {
public:
    /**
     * Create a new lexer
     * @param source JavaScript source code
     */
    explicit Lexer(const std::string& source);
    
    /**
     * Scan all tokens from the source code
     * @return Vector of tokens
     */
    std::vector<Token> scan_tokens();

private:
    // Source code and position trackers
    std::string source_;
    std::vector<Token> tokens_;
    size_t start_ = 0;
    size_t current_ = 0;
    int line_ = 1;
    int column_ = 1;
    
    // Scanning methods
    void scan_token();
    void multi_line_comment();
    void string(char quote);
    void number();
    void identifier();
    
    // Helper methods
    void add_token(TokenType type);
    void add_token(TokenType type, const LiteralValue& literal);
    bool match(char expected);
    char peek() const;
    char peek_next() const;
    bool is_alpha(char c) const;
    bool is_digit(char c) const;
    bool is_alphanumeric(char c) const;
    char advance();
    bool is_at_end() const;
    
    // Error handling
    void error(const std::string& message);
};

} // namespace quanta

#endif // QUANTA_LEXER_H 