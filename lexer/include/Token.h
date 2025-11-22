/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_TOKEN_H
#define QUANTA_TOKEN_H

#include <string>
#include <vector>

namespace Quanta {

/**
 * JavaScript token types for ES2023+ specification
 */
enum class TokenType {
    // Literals
    EOF_TOKEN = 0,
    IDENTIFIER,
    NUMBER,
    STRING,
    TEMPLATE_LITERAL,
    BOOLEAN,
    NULL_LITERAL,
    BIGINT_LITERAL,
    UNDEFINED,
    
    // Keywords
    BREAK,
    CASE,
    CATCH,
    CLASS,
    CONST,
    CONTINUE,
    DEBUGGER,
    DEFAULT,
    DELETE,
    DO,
    ELSE,
    EXPORT,
    EXTENDS,
    FINALLY,
    FOR,
    FUNCTION,
    IF,
    IMPORT,
    IN,
    INSTANCEOF,
    LET,
    NEW,
    RETURN,
    SUPER,
    SWITCH,
    THIS,
    THROW,
    TRY,
    TYPEOF,
    VAR,
    VOID,
    WHILE,
    WITH,
    YIELD,
    
    // ES6+ Keywords
    ASYNC,
    AWAIT,
    FROM,
    OF,
    STATIC,
    
    // Operators
    PLUS,               // +
    MINUS,              // -
    MULTIPLY,           // *
    DIVIDE,             // /
    MODULO,             // %
    EXPONENT,           // **
    
    ASSIGN,             // =
    PLUS_ASSIGN,        // +=
    MINUS_ASSIGN,       // -=
    MULTIPLY_ASSIGN,    // *=
    DIVIDE_ASSIGN,      // /=
    MODULO_ASSIGN,      // %=
    EXPONENT_ASSIGN,    // **=
    
    INCREMENT,          // ++
    DECREMENT,          // --
    
    EQUAL,              // ==
    NOT_EQUAL,          // !=
    STRICT_EQUAL,       // ===
    STRICT_NOT_EQUAL,   // !==
    LESS_THAN,          // <
    GREATER_THAN,       // >
    LESS_EQUAL,         // <=
    GREATER_EQUAL,      // >=
    
    LOGICAL_AND,        // &&
    LOGICAL_OR,         // ||
    LOGICAL_NOT,        // !
    
    BITWISE_AND,        // &
    BITWISE_OR,         // |
    BITWISE_XOR,        // ^
    BITWISE_NOT,        // ~
    LEFT_SHIFT,         // <<
    RIGHT_SHIFT,        // >>
    UNSIGNED_RIGHT_SHIFT, // >>>
    
    BITWISE_AND_ASSIGN, // &=
    BITWISE_OR_ASSIGN,  // |=
    BITWISE_XOR_ASSIGN, // ^=
    LEFT_SHIFT_ASSIGN,  // <<=
    RIGHT_SHIFT_ASSIGN, // >>=
    UNSIGNED_RIGHT_SHIFT_ASSIGN, // >>>=
    
    // Punctuation
    LEFT_PAREN,         // (
    RIGHT_PAREN,        // )
    LEFT_BRACE,         // {
    RIGHT_BRACE,        // }
    LEFT_BRACKET,       // [
    RIGHT_BRACKET,      // ]
    
    SEMICOLON,          // ;
    COMMA,              // ,
    DOT,                // .
    COLON,              // :
    QUESTION,           // ?
    HASH,               // #
    
    // ES6+ Operators
    ARROW,              // =>
    ELLIPSIS,           // ...
    
    // ES2020+ Operators  
    OPTIONAL_CHAINING,  // ?.
    NULLISH_COALESCING, // ??
    NULLISH_ASSIGN,     // ??=
    LOGICAL_AND_ASSIGN, // &&=
    LOGICAL_OR_ASSIGN,  // ||=
    
    // Template literals
    TEMPLATE_START,     // `
    TEMPLATE_MIDDLE,    // `...${
    TEMPLATE_END,       // }...`
    
    // Special
    NEWLINE,
    WHITESPACE,
    COMMENT,
    REGEX,
    
    // JSX tokens
    JSX_ELEMENT_START,    // <
    JSX_ELEMENT_END,      // >
    JSX_SELF_CLOSE,       // />
    JSX_TEXT,             // Text content in JSX
    
    // Error
    INVALID
};

/**
 * Token position information
 */
struct Position {
    size_t line;
    size_t column;
    size_t offset;
    
    Position(size_t l = 1, size_t c = 1, size_t o = 0) 
        : line(l), column(c), offset(o) {}
    
    std::string to_string() const;
};

/**
 * JavaScript token
 */
class Token {
private:
    TokenType type_;
    std::string value_;
    Position start_;
    Position end_;
    double numeric_value_;
    bool has_numeric_value_;

public:
    // Constructors
    Token();
    Token(TokenType type, const Position& pos);
    Token(TokenType type, const std::string& value, const Position& start, const Position& end);
    Token(TokenType type, double numeric_value, const Position& start, const Position& end);
    
    // Accessors
    TokenType get_type() const { return type_; }
    const std::string& get_value() const { return value_; }
    const Position& get_start() const { return start_; }
    const Position& get_end() const { return end_; }
    
    double get_numeric_value() const { return numeric_value_; }
    bool has_numeric_value() const { return has_numeric_value_; }
    
    // Type checking
    bool is_keyword() const;
    bool is_operator() const;
    bool is_literal() const;
    bool is_punctuation() const;
    bool is_identifier() const { return type_ == TokenType::IDENTIFIER; }
    bool is_eof() const { return type_ == TokenType::EOF_TOKEN; }
    
    // Utility
    std::string to_string() const;
    std::string type_name() const;
    size_t length() const { return end_.offset - start_.offset; }
    
    // Static helpers
    static std::string token_type_name(TokenType type);
    static bool is_assignment_operator(TokenType type);
    static bool is_binary_operator(TokenType type);
    static bool is_unary_operator(TokenType type);
    static bool is_comparison_operator(TokenType type);
    static int get_precedence(TokenType type);
    static bool is_right_associative(TokenType type);
};

/**
 * Token sequence for efficient parsing
 */
class TokenSequence {
private:
    std::vector<Token> tokens_;
    size_t position_;

public:
    TokenSequence();
    explicit TokenSequence(std::vector<Token> tokens);
    
    // Navigation
    const Token& current() const;
    const Token& peek(size_t offset = 1) const;
    const Token& previous() const;
    void advance();
    void retreat();
    bool at_end() const;
    
    // Position
    size_t position() const { return position_; }
    void set_position(size_t pos);
    size_t size() const { return tokens_.size(); }
    
    // Access
    const Token& operator[](size_t index) const;
    void push_back(const Token& token);
    
    // Utility
    std::string to_string() const;
    
private:
    static const Token EOF_TOKEN_INSTANCE;
};

} // namespace Quanta

#endif // QUANTA_TOKEN_H