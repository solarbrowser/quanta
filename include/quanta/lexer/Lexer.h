/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_LEXER_H
#define QUANTA_LEXER_H

#include "quanta/lexer/Token.h"
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

namespace Quanta {

class Lexer {
public:
    struct LexerOptions {
        bool skip_whitespace = true;
        bool skip_comments = true;
        bool track_positions = true;
        bool allow_reserved_words = false;
        bool strict_mode = false;
        bool source_type_module = false;
    };

private:
    // Held by shared pointer so the token sequence this lexer produces can keep
    // the same bytes alive: its tokens address the source by offset, so they
    // are only readable for as long as it exists.
    std::shared_ptr<const std::string> source_ref_;
    size_t position_;
    Position current_position_;
    LexerOptions options_;
    std::vector<std::string> errors_;
    // Values that are not a slice of the source -- cooked string literals with
    // escapes, template literals, identifiers written with unicode escapes.
    std::vector<std::string> owned_values_;
    TokenType last_token_type_;
    // The tokens produced so far, for the two look-backs that decide whether a
    // `/` opens a regular expression. Reads size() and operator[], which the
    // sequence answers straight out of its blocks.
    const TokenSequence* tokens_so_far_ = nullptr;
    bool current_string_has_legacy_octal_ = false;

    const std::string& source() const { return *source_ref_; }

    static const std::unordered_map<std::string, TokenType> keywords_;
    
    static const std::unordered_map<char, TokenType> single_char_tokens_;

public:
    explicit Lexer(const std::string& source);
    Lexer(const std::string& source, const LexerOptions& options);
    // Shares the caller's buffer rather than copying it. Re-lexing one function
    // body out of a script must not copy the script to do it.
    Lexer(std::shared_ptr<const std::string> source, const LexerOptions& options);
    
    TokenSequence tokenize();
    // Tokenizes only [from.offset, end_offset) of the source. Offsets and
    // line/column stay absolute -- the tokens address the same buffer the
    // whole script does -- which is what lets one function body be lexed back
    // on demand instead of the token stream for the whole script being kept
    // for the life of the program.
    TokenSequence tokenize_range(const Position& from, size_t end_offset);
    Token next_token();
    
    Position get_position() const { return current_position_; }
    void reset(size_t position = 0);
    // reset() walks the source from the start to work out the line and column
    // of `position`. A caller that already knows them -- a deferred body
    // recorded its own start -- must not pay that walk once per body.
    void reset_to(const Position& at);
    
    const std::vector<std::string>& get_errors() const { return errors_; }
    bool has_errors() const { return !errors_.empty(); }
    
    bool at_end() const { return position_ >= source().length(); }
    size_t remaining() const { return source().length() - position_; }

    // The text of a token this lexer produced, resolved the same way
    // TokenSequence::text_of resolves it once the sequence is built.
    std::string_view text_of(const Token& token) const;

private:
    char current_char() const;
    char peek_char(size_t offset = 1) const;
    char advance();
    void skip_whitespace();
    void advance_position(char ch);
    
    Token create_token(TokenType type, const Position& start) const;
    Token create_token(TokenType type, const std::string& value, const Position& start);
    Token create_token(TokenType type, double numeric_value, const Position& start) const;
    
    Token read_identifier();
    Token read_number();
    Token read_string(char quote);
    Token read_template_literal();
    Token read_regex();
    Token read_single_line_comment();
    Token read_multi_line_comment();
    
    bool can_be_regex_literal() const;
    
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
    
    bool is_identifier_start(char ch) const;
    bool is_identifier_part(char ch) const;
    bool is_digit(char ch) const;
    bool is_hex_digit(char ch) const;
    bool is_binary_digit(char ch) const;
    bool is_octal_digit(char ch) const;
    bool is_whitespace(char ch) const;
    bool is_line_terminator(char ch) const;
    int utf8_whitespace_bytes() const;
    int utf8_line_terminator_bytes() const;
    bool is_regex_context() const;
    bool is_at_line_start() const;
    
    double parse_decimal_literal();
    double parse_hex_literal();
    double parse_binary_literal();
    double parse_octal_literal();
    double parse_legacy_octal_literal();
    double parse_exponent();
    
    std::string parse_string_literal(char quote);
    std::string parse_escape_sequence(bool in_template = false);
    std::string parse_unicode_escape();
    std::string parse_hex_escape();
    
    void add_error(const std::string& message);
    TokenType lookup_keyword(const std::string& identifier) const;
    bool is_reserved_word(const std::string& word) const;
};

}

#endif
