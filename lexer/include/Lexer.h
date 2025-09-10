/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_LEXER_H
#define QUANTA_LEXER_H

#include "Token.h"
#include <string>
#include <vector>
#include <unordered_map>

namespace Quanta {

/**
 * High-performance JavaScript lexer/tokenizer
 * Supports ES2023+ specification
 */
class Lexer {
public:
    struct LexerOptions {
        bool skip_whitespace = true;
        bool skip_comments = true;
        bool track_positions = true;
        bool allow_reserved_words = false;
        bool strict_mode = false;
    };

private:
    std::string source_;
    size_t position_;
    Position current_position_;
    LexerOptions options_;
    std::vector<std::string> errors_;
    
    // Keywords mapping
    static const std::unordered_map<std::string, TokenType> keywords_;
    
    // Character classification
    static const std::unordered_map<char, TokenType> single_char_tokens_;

public:
    // Constructor
    explicit Lexer(const std::string& source);
    Lexer(const std::string& source, const LexerOptions& options);
    
    // Tokenization
    TokenSequence tokenize();
    Token next_token();
    
    // Position management
    Position get_position() const { return current_position_; }
    void reset(size_t position = 0);
    
    // Error handling
    const std::vector<std::string>& get_errors() const { return errors_; }
    bool has_errors() const { return !errors_.empty(); }
    
    // Utility
    bool at_end() const { return position_ >= source_.length(); }
    size_t remaining() const { return source_.length() - position_; }

private:
    // Character access
    char current_char() const;
    char peek_char(size_t offset = 1) const;
    char advance();
    void skip_whitespace();
    void advance_position(char ch);
    
    // Token creation
    Token create_token(TokenType type, const Position& start) const;
    Token create_token(TokenType type, const std::string& value, const Position& start) const;
    Token create_token(TokenType type, double numeric_value, const Position& start) const;
    
    // Specific token parsing
    Token read_identifier();
    Token read_number();
    Token read_string(char quote);
    Token read_template_literal();
    Token read_regex();
    Token read_single_line_comment();
    Token read_multi_line_comment();
    
    // Regex literal detection
    bool can_be_regex_literal() const;
    
    // Operator parsing
    Token read_operator();
    Token read_assignment_or_equality();
    Token read_logical_or();
    Token read_logical_and();
    Token read_bitwise_or();
    Token read_bitwise_and();
    Token read_bitwise_xor();
    Token read_shift_operators();
    Token read_arithmetic_operators();
    Token read_increment_decrement();
    Token read_comparison_operators();
    
    // Character classification
    bool is_identifier_start(char ch) const;
    bool is_identifier_part(char ch) const;
    bool is_digit(char ch) const;
    bool is_hex_digit(char ch) const;
    bool is_binary_digit(char ch) const;
    bool is_octal_digit(char ch) const;
    bool is_whitespace(char ch) const;
    bool is_line_terminator(char ch) const;
    
    // Number parsing helpers
    double parse_decimal_literal();
    double parse_hex_literal();
    double parse_binary_literal();
    double parse_octal_literal();
    double parse_legacy_octal_literal();
    double parse_exponent();
    
    // String parsing helpers
    std::string parse_string_literal(char quote);
    std::string parse_escape_sequence();
    std::string parse_unicode_escape();
    std::string parse_hex_escape();
    
    // Utility
    void add_error(const std::string& message);
    TokenType lookup_keyword(const std::string& identifier) const;
    bool is_reserved_word(const std::string& word) const;
};

} // namespace Quanta

#endif // QUANTA_LEXER_H