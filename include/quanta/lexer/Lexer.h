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
        // Set when re-reading text that has already been parsed once. The
        // words ES5 reserves for strict mode are only errors where a name is
        // bound or read, which the first parse has already decided; refusing
        // them again here would reject an object literal that is legal in
        // strict code and was accepted the first time round.
        bool reparsing_accepted_source = false;
    };

private:
    // Held by shared pointer so the token sequence this lexer produces can keep
    // the same bytes alive: its tokens address the source by offset, so they
    // are only readable for as long as it exists.
    std::shared_ptr<const std::string> source_ref_;
    size_t position_;
    size_t stop_offset_ = static_cast<size_t>(-1);
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
    // For a sequence that pulls its tokens as they are asked for rather than
    // taking them all at once (see TokenSequence's streaming mode).
    const LexerOptions& options() const { return options_; }
    const std::vector<std::string>& owned_values() const { return owned_values_; }
    void set_last_token_type(TokenType t) { last_token_type_ = t; }
    // A leading "use strict" changes how the rest is lexed, and a streamed
    // sequence finds it as it pulls rather than in one pass up front.
    void enter_strict_mode() { options_.strict_mode = true; }
    std::string_view token_text(const Token& t) const { return text_of(t); }
    // The sequence being filled, which the regex/division decision looks back
    // into. Re-pointed on every pull, because a sequence is moved twice on its
    // way from here into the parser and only the live one may be read.
    void set_tokens_so_far(const TokenSequence* t) { tokens_so_far_ = t; }
    const std::shared_ptr<const std::string>& source_ref() const { return source_ref_; }
    const Position& current_position() const { return current_position_; }
    // Builds a sequence that lexes `source` on demand and owns the lexer
    // doing it. The sequence keeps the text alive, so both outlive the call.
    static TokenSequence stream(std::shared_ptr<const std::string> source,
                                const LexerOptions& options);
    // The same, for one body read back out of the source it was written in:
    // the tokens are pumped as the parse asks for them and let go of behind
    // it, so reading a large body back does not first lay the whole thing out.
    static TokenSequence stream_range(std::shared_ptr<const std::string> source,
                                      const Position& from, size_t end_offset,
                                      const LexerOptions& options);
    
    Position get_position() const { return current_position_; }
    void reset(size_t position = 0);
    // reset() walks the source from the start to work out the line and column
    // of `position`. A caller that already knows them -- a deferred body
    // recorded its own start -- must not pay that walk once per body.
    void reset_to(const Position& at);
    
    const std::vector<std::string>& get_errors() const { return errors_; }
    bool has_errors() const { return !errors_.empty(); }
    
    // A lexer reading one function body back stops where that body closes, so
    // the stream ends there instead of running to the end of the file.
    void set_stop_offset(size_t off) { stop_offset_ = off; }
    bool at_end() const { return position_ >= stop_offset_ || position_ >= source().length(); }
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
