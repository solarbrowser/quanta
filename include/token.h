#ifndef QUANTA_TOKEN_H
#define QUANTA_TOKEN_H

#include <string>
#include <variant>
#include "token_type.h"

namespace quanta {

/**
 * Represents a literal value that can be of different types
 */
using LiteralValue = std::variant<std::monostate, std::string, double, bool, std::nullptr_t>;

/**
 * Token class for the Quanta JavaScript Engine
 */
class Token {
public:
    /**
     * Create a new token
     * @param type The token type
     * @param lexeme The actual string representation of the token
     * @param literal The literal value (for numbers, strings, booleans)
     * @param line The line number where the token appears
     * @param column The column number where the token starts
     */
    Token(TokenType type, const std::string& lexeme, const LiteralValue& literal, int line, int column);

    /**
     * Get the token type
     * @return The token type
     */
    TokenType get_type() const;

    /**
     * Get the lexeme
     * @return The lexeme
     */
    const std::string& get_lexeme() const;

    /**
     * Get the literal value
     * @return The literal value
     */
    const LiteralValue& get_literal() const;

    /**
     * Get the line number
     * @return The line number
     */
    int get_line() const;

    /**
     * Get the column number
     * @return The column number
     */
    int get_column() const;

    /**
     * Get the string representation of the token
     * @return The string representation
     */
    std::string to_string() const;

private:
    TokenType type_;
    std::string lexeme_;
    LiteralValue literal_;
    int line_;
    int column_;
};

} // namespace quanta

#endif // QUANTA_TOKEN_H 