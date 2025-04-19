#ifndef QUANTA_TOKEN_TYPE_H
#define QUANTA_TOKEN_TYPE_H

#include <string>
#include <unordered_map>

namespace quanta {

/**
 * Enumeration of all token types in the Quanta JavaScript Engine
 */
enum class TokenType {
    // Single-character tokens
    LEFT_PAREN, RIGHT_PAREN,
    LEFT_BRACE, RIGHT_BRACE,
    LEFT_BRACKET, RIGHT_BRACKET,
    COMMA, DOT, SEMICOLON, COLON,
    
    // Operators
    PLUS, MINUS, STAR, SLASH, PERCENT,
    
    // Comparison operators
    EQUAL, EQUAL_EQUAL,
    BANG, BANG_EQUAL,
    GREATER, GREATER_EQUAL,
    LESS, LESS_EQUAL,
    
    // Logical operators
    AND, OR,
    
    // Assignment operators
    PLUS_EQUAL, MINUS_EQUAL, STAR_EQUAL, SLASH_EQUAL, PERCENT_EQUAL,
    
    // Literals
    IDENTIFIER, STRING, NUMBER, BOOLEAN, NULL_LITERAL, UNDEFINED,
    
    // Keywords
    VAR, LET, CONST,
    IF, ELSE,
    WHILE, FOR,
    FUNCTION, RETURN,
    TRUE, FALSE,
    THIS, NEW,
    
    // End of file
    END_OF_FILE
};

/**
 * Get the string representation of a token type
 * @param type The token type
 * @return The string representation
 */
std::string token_type_to_string(TokenType type);

/**
 * Map of keyword strings to their corresponding token types
 */
extern const std::unordered_map<std::string, TokenType> keywords;

} // namespace quanta

#endif // QUANTA_TOKEN_TYPE_H 