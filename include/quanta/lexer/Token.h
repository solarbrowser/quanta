/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_TOKEN_H
#define QUANTA_TOKEN_H

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Quanta {

enum class TokenType {
    EOF_TOKEN = 0,
    IDENTIFIER,
    NUMBER,
    STRING,
    TEMPLATE_LITERAL,
    BOOLEAN,
    NULL_LITERAL,
    BIGINT_LITERAL,
    UNDEFINED,
    
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
    ENUM,

    ASYNC,
    AWAIT,
    FROM,
    OF,
    STATIC,
    TARGET,
    
    PLUS,
    MINUS,
    MULTIPLY,
    DIVIDE,
    MODULO,
    EXPONENT,
    
    ASSIGN,
    PLUS_ASSIGN,
    MINUS_ASSIGN,
    MULTIPLY_ASSIGN,
    DIVIDE_ASSIGN,
    MODULO_ASSIGN,
    EXPONENT_ASSIGN,
    
    INCREMENT,
    DECREMENT,
    
    EQUAL,
    NOT_EQUAL,
    STRICT_EQUAL,
    STRICT_NOT_EQUAL,
    LESS_THAN,
    GREATER_THAN,
    LESS_EQUAL,
    GREATER_EQUAL,
    
    LOGICAL_AND,
    LOGICAL_OR,
    LOGICAL_NOT,
    
    BITWISE_AND,
    BITWISE_OR,
    BITWISE_XOR,
    BITWISE_NOT,
    LEFT_SHIFT,
    RIGHT_SHIFT,
    UNSIGNED_RIGHT_SHIFT,
    
    BITWISE_AND_ASSIGN,
    BITWISE_OR_ASSIGN,
    BITWISE_XOR_ASSIGN,
    LEFT_SHIFT_ASSIGN,
    RIGHT_SHIFT_ASSIGN,
    UNSIGNED_RIGHT_SHIFT_ASSIGN,
    
    LEFT_PAREN,
    RIGHT_PAREN,
    LEFT_BRACE,
    RIGHT_BRACE,
    LEFT_BRACKET,
    RIGHT_BRACKET,
    
    SEMICOLON,
    COMMA,
    DOT,
    COLON,
    QUESTION,
    HASH,
    AT,
    
    ARROW,
    ELLIPSIS,
    
    OPTIONAL_CHAINING,
    NULLISH_COALESCING,
    NULLISH_ASSIGN,
    LOGICAL_AND_ASSIGN,
    LOGICAL_OR_ASSIGN,
    
    TEMPLATE_START,
    TEMPLATE_MIDDLE,
    TEMPLATE_END,
    
    NEWLINE,
    WHITESPACE,
    COMMENT,
    REGEX,
    
    JSX_ELEMENT_START,
    JSX_ELEMENT_END,
    JSX_SELF_CLOSE,
    JSX_TEXT,
    
    INVALID
};

/**
 * Token position information
 */
struct Position {
    // 32-bit, not size_t: a token carries two of these, and a large script
    // turns into over a million tokens, so the eight bytes each field does
    // not need are paid a few million times over. The widths bound the
    // source at 4GB, which is not a source.
    uint32_t line;
    uint32_t column;
    uint32_t offset;

    Position(uint32_t l = 1, uint32_t c = 1, uint32_t o = 0)
        : line(l), column(c), offset(o) {}
    
    std::string to_string() const;
};

// A token does not carry its own text. Almost every token's text is bytes that
// are already in the source -- an identifier is its span, a string literal is
// its span minus the quotes -- so a token records where its text is instead of
// holding a private copy of it. The few tokens whose text is not in the source
// (a cooked string literal with escapes, a template's cooked/raw pair) keep it
// in a side table owned by the TokenSequence, and the same two fields address
// that table instead. Reading the text therefore needs the sequence the token
// came from: see TokenSequence::text_of.
class Token {
private:
    double numeric_value_;
    Position start_;
    Position end_;
    // Byte range in the source, or (index, 0) into the sequence's side table
    // when kValueIsOwned is set.
    uint32_t value_off_;
    uint32_t value_len_;
    TokenType type_;
    uint8_t flags_;

    enum Flag : uint8_t {
        kHasNumericValue     = 1u << 0,
        kEscapedKeyword      = 1u << 1,
        kStringHasEscapes    = 1u << 2,
        kStringHasLegacyOctal = 1u << 3,
        kValueIsOwned        = 1u << 4
    };

    void set_flag(Flag f, bool on) { flags_ = static_cast<uint8_t>(on ? (flags_ | f) : (flags_ & ~f)); }

public:
    Token();
    Token(TokenType type, const Position& pos);
    Token(TokenType type, const Position& start, const Position& end);

    TokenType get_type() const { return type_; }
    const Position& get_start() const { return start_; }
    const Position& get_end() const { return end_; }

    uint32_t value_offset() const { return value_off_; }
    uint32_t value_length() const { return value_len_; }
    bool value_is_owned() const { return (flags_ & kValueIsOwned) != 0; }
    void set_source_value(uint32_t offset, uint32_t length) {
        value_off_ = offset;
        value_len_ = length;
        set_flag(kValueIsOwned, false);
    }
    void set_owned_value(uint32_t index) {
        value_off_ = index;
        value_len_ = 0;
        set_flag(kValueIsOwned, true);
    }

    double get_numeric_value() const { return numeric_value_; }
    bool has_numeric_value() const { return (flags_ & kHasNumericValue) != 0; }
    void set_numeric_value(double v) { numeric_value_ = v; set_flag(kHasNumericValue, true); }
    bool has_escaped_keyword() const { return (flags_ & kEscapedKeyword) != 0; }
    void set_escaped_keyword(bool v) { set_flag(kEscapedKeyword, v); }
    bool string_has_escapes() const { return (flags_ & kStringHasEscapes) != 0; }
    void set_string_has_escapes(bool v) { set_flag(kStringHasEscapes, v); }
    bool string_has_legacy_octal() const { return (flags_ & kStringHasLegacyOctal) != 0; }
    void set_string_has_legacy_octal(bool v) { set_flag(kStringHasLegacyOctal, v); }

    bool is_keyword() const;
    bool is_operator() const;
    bool is_literal() const;
    bool is_punctuation() const;
    bool is_identifier() const { return type_ == TokenType::IDENTIFIER; }
    bool is_eof() const { return type_ == TokenType::EOF_TOKEN; }
    
    std::string to_string() const;
    std::string type_name() const;
    size_t length() const { return end_.offset - start_.offset; }
    
    static std::string token_type_name(TokenType type);
    static bool is_assignment_operator(TokenType type);
    static bool is_binary_operator(TokenType type);
    static bool is_unary_operator(TokenType type);
    static bool is_comparison_operator(TokenType type);
    static int get_precedence(TokenType type);
    static bool is_right_associative(TokenType type);
};


// The one place that knows how a token's two fields turn back into text. The
// lexer needs it while it is still building the sequence; TokenSequence needs
// it afterwards.
std::string_view token_value_text(const Token& token, const std::string& source,
                                  const std::vector<std::string>& owned_values);

// Owns the tokens together with the text they address: the source they were
// lexed from, and the side table of the few values that had to be rewritten.
// Holding the source here rather than beside the sequence is what makes an
// offset safe to store in a token -- a token cannot outlive its text, because
// the thing that holds the token holds the text as well.
class TokenSequence {
private:
    std::vector<Token> tokens_;
    std::shared_ptr<const std::string> source_;
    std::vector<std::string> owned_values_;
    size_t position_;

public:
    TokenSequence();
    // Only the lexer builds a populated sequence, and it always has all three
    // parts: there is deliberately no way to construct tokens without the text
    // they address.
    TokenSequence(std::vector<Token> tokens,
                  std::shared_ptr<const std::string> source,
                  std::vector<std::string> owned_values);

    // The text of a token, which is either a slice of the source or, for the
    // handful the lexer had to rewrite, an entry in the side table.
    std::string_view text_of(const Token& token) const;
    const std::string& source() const;

    const Token& current() const;
    const Token& peek(size_t offset = 1) const;
    const Token& previous() const;
    void advance();
    void retreat();
    bool at_end() const;
    
    size_t position() const { return position_; }
    void set_position(size_t pos);
    size_t size() const { return tokens_.size(); }
    
    const Token& operator[](size_t index) const;
    void push_back(const Token& token);
    
    std::string to_string() const;
    
private:
    static const Token EOF_TOKEN_INSTANCE;
};

}

#endif
