/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/lexer/Lexer.h"
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <iostream>
#include <unordered_set>

namespace Quanta {

const std::unordered_map<std::string, TokenType> Lexer::keywords_ = {
    {"break", TokenType::BREAK},
    {"case", TokenType::CASE},
    {"catch", TokenType::CATCH},
    {"class", TokenType::CLASS},
    {"const", TokenType::CONST},
    {"continue", TokenType::CONTINUE},
    {"debugger", TokenType::DEBUGGER},
    {"default", TokenType::DEFAULT},
    {"delete", TokenType::DELETE},
    {"do", TokenType::DO},
    {"else", TokenType::ELSE},
    {"export", TokenType::EXPORT},
    {"extends", TokenType::EXTENDS},
    {"finally", TokenType::FINALLY},
    {"for", TokenType::FOR},
    {"function", TokenType::FUNCTION},
    {"if", TokenType::IF},
    {"import", TokenType::IMPORT},
    {"in", TokenType::IN},
    {"instanceof", TokenType::INSTANCEOF},
    {"let", TokenType::LET},
    {"new", TokenType::NEW},
    {"return", TokenType::RETURN},
    {"super", TokenType::SUPER},
    {"switch", TokenType::SWITCH},
    {"this", TokenType::THIS},
    {"throw", TokenType::THROW},
    {"try", TokenType::TRY},
    {"typeof", TokenType::TYPEOF},
    {"var", TokenType::VAR},
    {"void", TokenType::VOID},
    {"while", TokenType::WHILE},
    {"with", TokenType::WITH},
    {"yield", TokenType::YIELD},
    {"async", TokenType::ASYNC},
    {"await", TokenType::AWAIT},
    {"from", TokenType::FROM},
    {"of", TokenType::OF},
    {"static", TokenType::STATIC},
    {"true", TokenType::BOOLEAN},
    {"false", TokenType::BOOLEAN},
    {"null", TokenType::NULL_LITERAL},
    {"undefined", TokenType::UNDEFINED},
    {"enum", TokenType::ENUM}
};

const std::unordered_map<char, TokenType> Lexer::single_char_tokens_ = {
    {'(', TokenType::LEFT_PAREN},
    {')', TokenType::RIGHT_PAREN},
    {'{', TokenType::LEFT_BRACE},
    {'}', TokenType::RIGHT_BRACE},
    {'[', TokenType::LEFT_BRACKET},
    {']', TokenType::RIGHT_BRACKET},
    {';', TokenType::SEMICOLON},
    {',', TokenType::COMMA},
    {':', TokenType::COLON},
    {'~', TokenType::BITWISE_NOT},
    {'#', TokenType::HASH}
};


Lexer::Lexer(const std::string& source)
    : source_(source), position_(0), current_position_(1, 1, 0), last_token_type_(TokenType::EOF_TOKEN) {
    options_.skip_whitespace = true;
    options_.skip_comments = true;
    options_.track_positions = true;
    options_.allow_reserved_words = false;
    options_.strict_mode = false;
    
    if (source_.size() >= 3 && 
        static_cast<unsigned char>(source_[0]) == 0xEF &&
        static_cast<unsigned char>(source_[1]) == 0xBB &&
        static_cast<unsigned char>(source_[2]) == 0xBF) {
        position_ = 3;
        current_position_.offset = 3;
    }
}

Lexer::Lexer(const std::string& source, const LexerOptions& options)
    : source_(source), position_(0), current_position_(1, 1, 0), options_(options), last_token_type_(TokenType::EOF_TOKEN) {
    if (source_.size() >= 3 && 
        static_cast<unsigned char>(source_[0]) == 0xEF &&
        static_cast<unsigned char>(source_[1]) == 0xBB &&
        static_cast<unsigned char>(source_[2]) == 0xBF) {
        position_ = 3;
        current_position_.offset = 3;
    }
}

TokenSequence Lexer::tokenize() {
    std::vector<Token> tokens;
    bool strict_mode_detected = false;
    
    while (!at_end()) {
        Token token = next_token();

        if (token.get_type() != TokenType::WHITESPACE &&
            token.get_type() != TokenType::COMMENT &&
            token.get_type() != TokenType::NEWLINE) {
            last_token_type_ = token.get_type();
        }

        if (!strict_mode_detected && tokens.empty() && 
            token.get_type() == TokenType::STRING && 
            token.get_value() == "use strict") {
            options_.strict_mode = true;
            strict_mode_detected = true;
        }
        
        if ((options_.skip_whitespace && token.get_type() == TokenType::WHITESPACE) ||
            (options_.skip_comments && token.get_type() == TokenType::COMMENT)) {
            continue;
        }
        
        tokens.push_back(token);
        
        if (token.get_type() == TokenType::EOF_TOKEN) {
            break;
        }
    }
    
    if (tokens.empty() || tokens.back().get_type() != TokenType::EOF_TOKEN) {
        tokens.emplace_back(TokenType::EOF_TOKEN, current_position_);
    }
    
    return TokenSequence(std::move(tokens));
}

Token Lexer::next_token() {
    if (at_end()) {
        return Token(TokenType::EOF_TOKEN, current_position_);
    }
    
    Position start = current_position_;
    char ch = current_char();
    
    if (is_whitespace(ch)) {
        if (options_.skip_whitespace) {
            skip_whitespace();
            return next_token();
        } else {
            skip_whitespace();
            return create_token(TokenType::WHITESPACE, start);
        }
    }
    
    if (is_line_terminator(ch)) {
        int ltb = utf8_line_terminator_bytes();
        if (ltb > 0) {
            for (int i = 0; i < ltb; i++) advance();
            // Multi-byte line terminators (U+2028, U+2029) must update line position
            current_position_.line++;
            current_position_.column = 1;
        } else {
            advance();
        }
        return create_token(TokenType::NEWLINE, start);
    }
    
    if (ch == '/') {
        char next = peek_char();
        if (next == '/') {
            return read_single_line_comment();
        } else if (next == '*') {
            return read_multi_line_comment();
        } else if (can_be_regex_literal()) {
            return read_regex();
        }
    }

    // HTML-style comment: <!-- treated as single line comment
    if (ch == '<' && peek_char() == '!' && peek_char(2) == '-' && peek_char(3) == '-') {
        return read_single_line_comment();
    }

    if (ch == '-' && peek_char() == '-' && peek_char(2) == '>') {
        if (current_position_.column == 1 || is_at_line_start()) {
            return read_single_line_comment();
        }
    }

    if (is_digit(ch) || (ch == '.' && is_digit(peek_char()))) {
        return read_number();
    }
    
    if (ch == '"' || ch == '\'') {
        return read_string(ch);
    }
    
    if (ch == '`') {
        return read_template_literal();
    }
    
    if (is_identifier_start(ch)) {
        return read_identifier();
    }
    
    auto single_it = single_char_tokens_.find(ch);
    if (single_it != single_char_tokens_.end()) {
        advance();
        return create_token(single_it->second, start);
    }
    
    return read_operator();
}

void Lexer::reset(size_t position) {
    position_ = std::min(position, source_.length());
    current_position_ = Position(1, 1, position_);
    
    for (size_t i = 0; i < position_; ++i) {
        if (source_[i] == '\n') {
            current_position_.line++;
            current_position_.column = 1;
        } else if (source_[i] == '\r') {
            if (i + 1 < source_.length() && source_[i + 1] == '\n') {
                continue;
            } else {
                current_position_.line++;
                current_position_.column = 1;
            }
        } else {
            current_position_.column++;
        }
    }
}

char Lexer::current_char() const {
    if (at_end()) return '\0';
    return source_[position_];
}

char Lexer::peek_char(size_t offset) const {
    size_t peek_pos = position_ + offset;
    if (peek_pos >= source_.length()) return '\0';
    return source_[peek_pos];
}

char Lexer::advance() {
    if (at_end()) return '\0';
    
    char ch = source_[position_++];
    advance_position(ch);
    return ch;
}

void Lexer::skip_whitespace() {
    while (!at_end()) {
        char ch = current_char();
        if (ch == ' ' || ch == '\t' || ch == '\v' || ch == '\f' || ch == '\r') {
            advance();
            continue;
        }
        int wb = utf8_whitespace_bytes();
        if (wb > 0) {
            for (int i = 0; i < wb; i++) advance();
            continue;
        }
        break;
    }
}

void Lexer::advance_position(char ch) {
    current_position_.offset = position_;
    
    if (ch == '\n') {
        current_position_.line++;
        current_position_.column = 1;
    } else if (ch == '\r') {
        if (position_ < source_.length() && source_[position_] == '\n') {
            current_position_.column++;
        } else {
            current_position_.line++;
            current_position_.column = 1;
        }
    } else {
        current_position_.column++;
    }
}

Token Lexer::create_token(TokenType type, const Position& start) const {
    return Token(type, start);
}

Token Lexer::create_token(TokenType type, const std::string& value, const Position& start) const {
    return Token(type, value, start, current_position_);
}

Token Lexer::create_token(TokenType type, double numeric_value, const Position& start) const {
    return Token(type, numeric_value, start, current_position_);
}

static bool is_unicode_id_start(uint32_t cp);
static bool is_invalid_id_start_cp(uint32_t cp);
static bool is_invalid_id_continue_cp(uint32_t cp);

Token Lexer::read_identifier() {
    Position start = current_position_;
    std::string value;
    bool contains_unicode_escapes = false;
    
    char first = current_char();
    if (std::isdigit(first)) {
        add_error("Invalid identifier: identifier cannot start with a digit");
        return create_token(TokenType::INVALID, value, start);
    }
    
    if (current_char() == '\\' && peek_char() == 'u') {
        contains_unicode_escapes = true;
        advance();
        advance();
        
        if (current_char() == '{') {
            advance();
            std::string hex_digits;
            while (!at_end() && current_char() != '}' && hex_digits.length() < 6) {
                if (is_hex_digit(current_char())) {
                    hex_digits += current_char();
                    advance();
                } else {
                    add_error("Invalid unicode escape sequence in identifier");
                    return create_token(TokenType::INVALID, value, start);
                }
            }
            if (current_char() != '}') {
                add_error("Invalid unicode escape sequence in identifier");
                return create_token(TokenType::INVALID, value, start);
            }
            advance();
            
            if (hex_digits == "61") value += 'a';
            else if (hex_digits == "6C") value += 'l';
            else if (hex_digits == "73") value += 's';
            else if (hex_digits == "65") value += 'e';
            else if (hex_digits == "6F") value += 'o';
            else {
                unsigned long codepoint = std::strtoul(hex_digits.c_str(), nullptr, 16);
                if (is_invalid_id_start_cp((uint32_t)codepoint)) {
                    add_error("Invalid unicode escape sequence in identifier");
                    return create_token(TokenType::INVALID, value, start);
                }
                if (codepoint <= 0x7F) {
                    value += static_cast<char>(codepoint);
                } else if (codepoint <= 0x7FF) {
                    value += static_cast<char>(0xC0 | (codepoint >> 6));
                    value += static_cast<char>(0x80 | (codepoint & 0x3F));
                } else if (codepoint <= 0xFFFF) {
                    value += static_cast<char>(0xE0 | (codepoint >> 12));
                    value += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                    value += static_cast<char>(0x80 | (codepoint & 0x3F));
                } else if (codepoint <= 0x10FFFF) {
                    value += static_cast<char>(0xF0 | (codepoint >> 18));
                    value += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
                    value += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                    value += static_cast<char>(0x80 | (codepoint & 0x3F));
                } else {
                    add_error("Invalid unicode codepoint in identifier");
                    return create_token(TokenType::INVALID, value, start);
                }
            }
        } else {
            std::string hex_digits;
            for (int i = 0; i < 4 && !at_end(); i++) {
                if (is_hex_digit(current_char())) {
                    hex_digits += current_char();
                    advance();
                } else {
                    add_error("Invalid unicode escape sequence in identifier");
                    return create_token(TokenType::INVALID, value, start);
                }
            }
            
            if (hex_digits == "0061") value += 'a';
            else if (hex_digits == "006C") value += 'l';
            else if (hex_digits == "0073") value += 's';
            else if (hex_digits == "0065") value += 'e';
            else {
                unsigned long codepoint = std::strtoul(hex_digits.c_str(), nullptr, 16);
                if (is_invalid_id_start_cp((uint32_t)codepoint)) {
                    add_error("Invalid unicode escape sequence in identifier");
                    return create_token(TokenType::INVALID, value, start);
                }
                if (codepoint <= 0x7F) {
                    value += static_cast<char>(codepoint);
                } else if (codepoint <= 0x7FF) {
                    value += static_cast<char>(0xC0 | (codepoint >> 6));
                    value += static_cast<char>(0x80 | (codepoint & 0x3F));
                } else if (codepoint <= 0xFFFF) {
                    value += static_cast<char>(0xE0 | (codepoint >> 12));
                    value += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                    value += static_cast<char>(0x80 | (codepoint & 0x3F));
                } else {
                    add_error("Invalid unicode codepoint in identifier");
                    return create_token(TokenType::INVALID, value, start);
                }
            }
        }
    } else {
        value += advance();
    }
    
    while (!at_end() && (is_identifier_part(current_char()) || 
                        (current_char() == '\\' && peek_char() == 'u'))) {
        if (current_char() == '\\' && peek_char() == 'u') {
            contains_unicode_escapes = true;
            advance();
            advance();
            
            if (current_char() == '{') {
                advance();
                std::string hex_digits;
                while (!at_end() && current_char() != '}' && hex_digits.length() < 6) {
                    if (is_hex_digit(current_char())) {
                        hex_digits += current_char();
                        advance();
                    } else {
                        add_error("Invalid unicode escape sequence in identifier");
                        return create_token(TokenType::INVALID, value, start);
                    }
                }
                if (current_char() != '}') {
                    add_error("Invalid unicode escape sequence in identifier");
                    return create_token(TokenType::INVALID, value, start);
                }
                advance();
                
                unsigned long codepoint = std::strtoul(hex_digits.c_str(), nullptr, 16);
                if (is_invalid_id_continue_cp((uint32_t)codepoint)) {
                    add_error("Invalid unicode escape sequence in identifier");
                    return create_token(TokenType::INVALID, value, start);
                }
                if (codepoint <= 0x7F) {
                    value += static_cast<char>(codepoint);
                } else if (codepoint <= 0x7FF) {
                    value += static_cast<char>(0xC0 | (codepoint >> 6));
                    value += static_cast<char>(0x80 | (codepoint & 0x3F));
                } else if (codepoint <= 0xFFFF) {
                    value += static_cast<char>(0xE0 | (codepoint >> 12));
                    value += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                    value += static_cast<char>(0x80 | (codepoint & 0x3F));
                } else if (codepoint <= 0x10FFFF) {
                    value += static_cast<char>(0xF0 | (codepoint >> 18));
                    value += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
                    value += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                    value += static_cast<char>(0x80 | (codepoint & 0x3F));
                } else {
                    add_error("Invalid unicode codepoint in identifier");
                    return create_token(TokenType::INVALID, value, start);
                }
            } else {
                std::string hex_digits;
                for (int i = 0; i < 4 && !at_end(); i++) {
                    if (is_hex_digit(current_char())) {
                        hex_digits += current_char();
                        advance();
                    } else {
                        add_error("Invalid unicode escape sequence in identifier");
                        return create_token(TokenType::INVALID, value, start);
                    }
                }

                unsigned long codepoint = std::strtoul(hex_digits.c_str(), nullptr, 16);
                if (is_invalid_id_continue_cp((uint32_t)codepoint)) {
                    add_error("Invalid unicode escape sequence in identifier");
                    return create_token(TokenType::INVALID, value, start);
                }
                if (codepoint <= 0x7F) {
                    value += static_cast<char>(codepoint);
                } else if (codepoint <= 0x7FF) {
                    value += static_cast<char>(0xC0 | (codepoint >> 6));
                    value += static_cast<char>(0x80 | (codepoint & 0x3F));
                } else if (codepoint <= 0xFFFF) {
                    value += static_cast<char>(0xE0 | (codepoint >> 12));
                    value += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                    value += static_cast<char>(0x80 | (codepoint & 0x3F));
                } else {
                    add_error("Invalid unicode codepoint in identifier");
                    return create_token(TokenType::INVALID, value, start);
                }
            }
        } else {
            value += advance();
        }
    }
    
    TokenType type = lookup_keyword(value);

    if (contains_unicode_escapes && type != TokenType::IDENTIFIER) {
        Token tok = create_token(TokenType::IDENTIFIER, value, start);
        tok.set_escaped_keyword(true);
        return tok;
    }

    if (options_.strict_mode && type == TokenType::IDENTIFIER && is_reserved_word(value)) {
        add_error("SyntaxError: Unexpected reserved word '" + value + "' in strict mode");
        return create_token(TokenType::INVALID, value, start);
    }

    // ES5 7.6.1.2: Future reserved words in strict mode
    if (options_.strict_mode && type == TokenType::IDENTIFIER) {
        static const std::unordered_set<std::string> strict_reserved = {
            "implements", "interface", "package", "private", "protected", "public"
        };
        if (strict_reserved.count(value)) {
            add_error("SyntaxError: '" + value + "' is a reserved word in strict mode");
            return create_token(TokenType::INVALID, value, start);
        }
    }

    return create_token(type, value, start);
}

Token Lexer::read_number() {
    Position start = current_position_;
    size_t start_pos = position_;
    double value = 0.0;
    
    if (current_char() == '0') {
        char next = peek_char();
        if (next == 'x' || next == 'X') {
            advance();
            advance();
            if (at_end() || !is_hex_digit(current_char())) {
                add_error("SyntaxError: Invalid hex literal - missing digits");
                return create_token(TokenType::INVALID, start);
            }
            value = parse_hex_literal();
        } else if (next == 'b' || next == 'B') {
            advance();
            advance();
            if (at_end() || !is_binary_digit(current_char())) {
                add_error("SyntaxError: Invalid binary literal - missing digits");
                return create_token(TokenType::INVALID, start);
            }
            size_t error_count_before = errors_.size();
            value = parse_binary_literal();
            if (errors_.size() > error_count_before) {
                return create_token(TokenType::INVALID, start);
            }
        } else if (next == 'o' || next == 'O') {
            advance();
            advance();
            if (at_end() || !is_octal_digit(current_char())) {
                add_error("SyntaxError: Invalid octal literal - missing digits");
                return create_token(TokenType::INVALID, start);
            }
            value = parse_octal_literal();
        } else if (std::isdigit(next)) {
            if (options_.strict_mode) {
                // Skip past the entire octal literal to avoid infinite loop
                advance(); // past '0'
                while (!at_end() && is_octal_digit(current_char())) {
                    advance();
                }
                add_error("SyntaxError: Octal literals are not allowed in strict mode");
                return create_token(TokenType::INVALID, start);
            }
            value = parse_legacy_octal_literal();
        } else if (next == '_') {
            add_error("SyntaxError: Numeric separator cannot appear after leading zero");
            return create_token(TokenType::INVALID, start);
        } else {
            value = parse_decimal_literal();
        }
    } else {
        value = parse_decimal_literal();
    }
    
    if (!at_end() && current_char() == 'n') {
        advance();
        size_t length = position_ - start_pos - 1;
        std::string bigint_str = source_.substr(start_pos, length);
        // BigInt cannot have exponent or decimal point
        if (bigint_str.find('e') != std::string::npos ||
            bigint_str.find('E') != std::string::npos ||
            bigint_str.find('.') != std::string::npos) {
            add_error("SyntaxError: Invalid BigInt literal");
            return create_token(TokenType::INVALID, start);
        }
        // BigInt cannot have legacy octal prefix (0<digit>)
        if (bigint_str.size() >= 2 && bigint_str[0] == '0' && std::isdigit((unsigned char)bigint_str[1])) {
            add_error("SyntaxError: Invalid BigInt literal");
            return create_token(TokenType::INVALID, start);
        }
        return create_token(TokenType::BIGINT_LITERAL, bigint_str, start);
    }

    // Numeric literal must not be immediately followed by an identifier start
    if (!at_end() && (std::isdigit((unsigned char)current_char()) || is_identifier_start(current_char()))) {
        add_error("SyntaxError: Numeric literal must not be immediately followed by a decimal digit or identifier start");
        return create_token(TokenType::INVALID, start);
    }

    return create_token(TokenType::NUMBER, value, start);
}

Token Lexer::read_string(char quote) {
    Position start = current_position_;
    advance();
    
    std::string value = parse_string_literal(quote);
    
    if (at_end() || current_char() != quote) {
        add_error("Unterminated string literal");
        return create_token(TokenType::INVALID, start);
    }
    
    advance();
    return create_token(TokenType::STRING, value, start);
}

Token Lexer::read_template_literal() {
    Position start = current_position_;
    advance();

    // ES2018: track cooked validity per text segment, not globally.
    // Expression markers ${...} are preserved in full_cooked so the parser can
    // find them even when a prior text segment has invalid escapes.
    std::string full_cooked;  // final cooked output (with expression markers)
    std::string full_raw;     // final raw output (with expression markers)
    std::string seg_cooked;   // accumulated cooked chars for current text segment
    bool seg_valid = true;    // is current text segment's cooked value valid?

    while (!at_end() && current_char() != '`') {
        if (current_char() == '$' && peek_char() == '{') {
            // End of text segment: commit it, then pass expression through verbatim
            if (!seg_valid) full_cooked += "\x01";
            else full_cooked += seg_cooked;
            seg_cooked.clear();
            seg_valid = true;

            char c1 = advance(); // '$'
            char c2 = advance(); // '{'
            full_cooked += c1; full_raw += c1;
            full_cooked += c2; full_raw += c2;

            int brace_count = 1;
            while (!at_end() && brace_count > 0) {
                char ch = advance();
                full_cooked += ch; full_raw += ch;
                if (ch == '{') brace_count++;
                else if (ch == '}') brace_count--;
            }
        } else if (current_char() == '\\') {
            size_t raw_start = position_;
            size_t error_count_before = errors_.size();
            std::string cooked_char = parse_escape_sequence();
            full_raw += source_.substr(raw_start, position_ - raw_start);
            if (errors_.size() > error_count_before) {
                errors_.resize(error_count_before);
                seg_valid = false;
            } else if (seg_valid) {
                seg_cooked += cooked_char;
            }
        } else if (current_char() == '\r') {
            // ES6: Normalize CR and CRLF to LF in template literals
            advance();
            if (!at_end() && current_char() == '\n') advance();
            if (seg_valid) seg_cooked += '\n';
            full_raw += '\n';
        } else {
            char ch = advance();
            if (seg_valid) seg_cooked += ch;
            full_raw += ch;
        }
    }

    if (at_end()) {
        add_error("Unterminated template literal");
        return create_token(TokenType::INVALID, start);
    }

    advance(); // skip closing backtick

    // Commit the final text segment
    if (!seg_valid) full_cooked += "\x01";
    else full_cooked += seg_cooked;

    // Encode: 4-byte big-endian cooked length, then cooked bytes, then raw bytes.
    // Using a length prefix avoids conflicts when cooked contains null chars (\0 escapes).
    uint32_t cooked_len = static_cast<uint32_t>(full_cooked.size());
    std::string token_value(4, '\0');
    token_value[0] = static_cast<char>((cooked_len >> 24) & 0xFF);
    token_value[1] = static_cast<char>((cooked_len >> 16) & 0xFF);
    token_value[2] = static_cast<char>((cooked_len >> 8) & 0xFF);
    token_value[3] = static_cast<char>(cooked_len & 0xFF);
    token_value += full_cooked;
    token_value += full_raw;
    return create_token(TokenType::TEMPLATE_LITERAL, token_value, start);
}

Token Lexer::read_single_line_comment() {
    Position start = current_position_;
    advance();
    advance();
    
    std::string value;
    while (!at_end() && !is_line_terminator(current_char())) {
        value += advance();
    }
    
    return create_token(TokenType::COMMENT, value, start);
}

Token Lexer::read_multi_line_comment() {
    Position start = current_position_;
    advance();
    advance();
    
    std::string value;
    while (!at_end()) {
        if (current_char() == '*' && peek_char() == '/') {
            advance();
            advance();
            break;
        }
        value += advance();
    }
    
    return create_token(TokenType::COMMENT, value, start);
}

Token Lexer::read_operator() {
    Position start = current_position_;
    char ch = current_char();
    
    switch (ch) {
        case '+':
            advance();
            if (current_char() == '+') {
                advance();
                return create_token(TokenType::INCREMENT, start);
            } else if (current_char() == '=') {
                advance();
                return create_token(TokenType::PLUS_ASSIGN, start);
            }
            return create_token(TokenType::PLUS, start);
            
        case '-':
            advance();
            if (current_char() == '-') {
                advance();
                return create_token(TokenType::DECREMENT, start);
            } else if (current_char() == '=') {
                advance();
                return create_token(TokenType::MINUS_ASSIGN, start);
            }
            return create_token(TokenType::MINUS, start);
            
        case '*':
            advance();
            if (current_char() == '*') {
                advance();
                if (current_char() == '=') {
                    advance();
                    return create_token(TokenType::EXPONENT_ASSIGN, start);
                }
                return create_token(TokenType::EXPONENT, start);
            } else if (current_char() == '=') {
                advance();
                return create_token(TokenType::MULTIPLY_ASSIGN, start);
            }
            return create_token(TokenType::MULTIPLY, start);
            
        case '/':
            if (is_regex_context()) {
                return read_regex();
            }

            advance();
            if (current_char() == '=') {
                advance();
                return create_token(TokenType::DIVIDE_ASSIGN, start);
            }
            return create_token(TokenType::DIVIDE, start);
            
        case '%':
            advance();
            if (current_char() == '=') {
                advance();
                return create_token(TokenType::MODULO_ASSIGN, start);
            }
            return create_token(TokenType::MODULO, start);
            
        case '=':
            advance();
            if (current_char() == '=') {
                advance();
                if (current_char() == '=') {
                    advance();
                    return create_token(TokenType::STRICT_EQUAL, start);
                }
                return create_token(TokenType::EQUAL, start);
            } else if (current_char() == '>') {
                advance();
                return create_token(TokenType::ARROW, start);
            }
            return create_token(TokenType::ASSIGN, start);
            
        case '!':
            advance();
            if (current_char() == '=') {
                advance();
                if (current_char() == '=') {
                    advance();
                    return create_token(TokenType::STRICT_NOT_EQUAL, start);
                }
                return create_token(TokenType::NOT_EQUAL, start);
            }
            return create_token(TokenType::LOGICAL_NOT, start);
            
        case '<':
            advance();
            if (current_char() == '=') {
                advance();
                return create_token(TokenType::LESS_EQUAL, start);
            } else if (current_char() == '<') {
                advance();
                if (current_char() == '=') {
                    advance();
                    return create_token(TokenType::LEFT_SHIFT_ASSIGN, start);
                }
                return create_token(TokenType::LEFT_SHIFT, start);
            }
            return create_token(TokenType::LESS_THAN, start);
            
        case '>':
            advance();
            if (current_char() == '=') {
                advance();
                return create_token(TokenType::GREATER_EQUAL, start);
            } else if (current_char() == '>') {
                advance();
                if (current_char() == '>') {
                    advance();
                    if (current_char() == '=') {
                        advance();
                        return create_token(TokenType::UNSIGNED_RIGHT_SHIFT_ASSIGN, start);
                    }
                    return create_token(TokenType::UNSIGNED_RIGHT_SHIFT, start);
                } else if (current_char() == '=') {
                    advance();
                    return create_token(TokenType::RIGHT_SHIFT_ASSIGN, start);
                }
                return create_token(TokenType::RIGHT_SHIFT, start);
            }
            return create_token(TokenType::GREATER_THAN, start);
            
        case '&':
            advance();
            if (current_char() == '&') {
                advance();
                if (current_char() == '=') {
                    advance();
                    return create_token(TokenType::LOGICAL_AND_ASSIGN, start);
                }
                return create_token(TokenType::LOGICAL_AND, start);
            } else if (current_char() == '=') {
                advance();
                return create_token(TokenType::BITWISE_AND_ASSIGN, start);
            }
            return create_token(TokenType::BITWISE_AND, start);
            
        case '|':
            advance();
            if (current_char() == '|') {
                advance();
                if (current_char() == '=') {
                    advance();
                    return create_token(TokenType::LOGICAL_OR_ASSIGN, start);
                }
                return create_token(TokenType::LOGICAL_OR, start);
            } else if (current_char() == '=') {
                advance();
                return create_token(TokenType::BITWISE_OR_ASSIGN, start);
            }
            return create_token(TokenType::BITWISE_OR, start);
            
        case '^':
            advance();
            if (current_char() == '=') {
                advance();
                return create_token(TokenType::BITWISE_XOR_ASSIGN, start);
            }
            return create_token(TokenType::BITWISE_XOR, start);
            
        case '.':
            advance();
            if (current_char() == '.' && peek_char() == '.') {
                advance();
                advance();
                return create_token(TokenType::ELLIPSIS, start);
            }
            return create_token(TokenType::DOT, start);
            
        case '?':
            advance();
            if (current_char() == '.') {
                advance();
                return create_token(TokenType::OPTIONAL_CHAINING, start);
            } else if (current_char() == '?') {
                advance();
                if (current_char() == '=') {
                    advance();
                    return create_token(TokenType::NULLISH_ASSIGN, start);
                }
                return create_token(TokenType::NULLISH_COALESCING, start);
            }
            return create_token(TokenType::QUESTION, start);
            
        default:
            advance();
            add_error("Unexpected character: " + std::string(1, ch));
            return create_token(TokenType::INVALID, start);
    }
}

static uint32_t decode_utf8_at(const std::string& src, size_t pos) {
    unsigned char c0 = static_cast<unsigned char>(src[pos]);
    if (c0 < 0x80) return c0;
    if (c0 < 0xC2) return 0xFFFD;
    if (c0 < 0xE0) {
        if (pos + 1 >= src.size()) return 0xFFFD;
        return ((c0 & 0x1F) << 6) | (static_cast<unsigned char>(src[pos + 1]) & 0x3F);
    }
    if (c0 < 0xF0) {
        if (pos + 2 >= src.size()) return 0xFFFD;
        return ((c0 & 0x0F) << 12)
             | ((static_cast<unsigned char>(src[pos + 1]) & 0x3F) << 6)
             | (static_cast<unsigned char>(src[pos + 2]) & 0x3F);
    }
    if (c0 < 0xF8) {
        if (pos + 3 >= src.size()) return 0xFFFD;
        return ((c0 & 0x07) << 18)
             | ((static_cast<unsigned char>(src[pos + 1]) & 0x3F) << 12)
             | ((static_cast<unsigned char>(src[pos + 2]) & 0x3F) << 6)
             | (static_cast<unsigned char>(src[pos + 3]) & 0x3F);
    }
    return 0xFFFD;
}

static bool is_unicode_id_start(uint32_t cp) {
    if (cp > 0xFFFF) return true;
    if (cp >= 0x00C0 && cp < 0x2000) return true;
    if (cp < 0x2070) return false;
    if (cp == 0x2071 || cp == 0x207F) return true;
    if (cp >= 0x2090 && cp <= 0x209C) return true;
    if (cp < 0x2100) return false;
    if (cp == 0x2102 || cp == 0x2107) return true;
    if (cp >= 0x210A && cp <= 0x2113) return true;
    if (cp == 0x2115) return true;
    if (cp >= 0x2119 && cp <= 0x211D) return true;
    if (cp == 0x2124 || cp == 0x2126 || cp == 0x2128) return true;
    if (cp >= 0x212A && cp <= 0x2139) return true;
    if (cp >= 0x213C && cp <= 0x213F) return true;
    if (cp >= 0x2145 && cp <= 0x2149) return true;
    if (cp == 0x214E) return true;
    if (cp < 0x2160) return false;
    if (cp <= 0x2188) return true;
    if (cp < 0x2C00) return false;
    if (cp <= 0x2DFF) return true;
    if (cp < 0x2E80) return false;
    return true;
}

static bool is_invalid_id_start_cp(uint32_t cp) {
    if (cp == 0x0A || cp == 0x0D || cp == 0x2028 || cp == 0x2029) return true;
    if (cp == 0x200C || cp == 0x200D) return true;
    if (cp == 0x2E2F) return true;
    if (cp < 0x80) return !(std::isalpha((int)cp) || cp == '_' || cp == '$');
    return false;
}

static bool is_invalid_id_continue_cp(uint32_t cp) {
    if (cp == 0x0A || cp == 0x0D || cp == 0x2028 || cp == 0x2029) return true;
    if (cp == 0x2E2F) return true;
    if (cp < 0x80) return !(std::isalnum((int)cp) || cp == '_' || cp == '$');
    return false;
}

bool Lexer::is_identifier_start(char ch) const {
    unsigned char uch = static_cast<unsigned char>(ch);
    if (std::isalpha(ch) || ch == '_' || ch == '$' || ch == '\\') {
        return true;
    }
    if (uch >= 0x80) {
        if (utf8_whitespace_bytes() > 0) return false;
        if (utf8_line_terminator_bytes() > 0) return false;
        uint32_t cp = decode_utf8_at(source_, position_);
        return is_unicode_id_start(cp);
    }
    return false;
}

bool Lexer::is_identifier_part(char ch) const {
    unsigned char uch = static_cast<unsigned char>(ch);
    if (std::isalnum(ch) || ch == '_' || ch == '$') {
        return true;
    }
    if (uch >= 0x80) {
        if (utf8_whitespace_bytes() > 0) return false;
        if (utf8_line_terminator_bytes() > 0) return false;
        return true;
    }
    return false;
}

bool Lexer::is_digit(char ch) const {
    return ch >= '0' && ch <= '9';
}

bool Lexer::is_hex_digit(char ch) const {
    return is_digit(ch) || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
}

bool Lexer::is_binary_digit(char ch) const {
    return ch == '0' || ch == '1';
}

bool Lexer::is_octal_digit(char ch) const {
    return ch >= '0' && ch <= '7';
}

bool Lexer::is_regex_context() const {
    switch (last_token_type_) {
        case TokenType::ASSIGN:
        case TokenType::PLUS_ASSIGN:
        case TokenType::MINUS_ASSIGN:
        case TokenType::MULTIPLY_ASSIGN:
        case TokenType::DIVIDE_ASSIGN:
        case TokenType::MODULO_ASSIGN:
        case TokenType::LEFT_PAREN:
        case TokenType::LEFT_BRACKET:
        case TokenType::LEFT_BRACE:
        case TokenType::COMMA:
        case TokenType::SEMICOLON:
        case TokenType::COLON:
        case TokenType::QUESTION:
        case TokenType::LOGICAL_NOT:
        case TokenType::BITWISE_AND:
        case TokenType::BITWISE_OR:
        case TokenType::BITWISE_XOR:
        case TokenType::PLUS:
        case TokenType::MINUS:
        case TokenType::MULTIPLY:
        case TokenType::DIVIDE:
        case TokenType::MODULO:
        case TokenType::LESS_THAN:
        case TokenType::GREATER_THAN:
        case TokenType::EQUAL:
        case TokenType::NOT_EQUAL:
        case TokenType::STRICT_EQUAL:
        case TokenType::STRICT_NOT_EQUAL:

        case TokenType::RETURN:
        case TokenType::THROW:
        case TokenType::NEW:
        case TokenType::TYPEOF:
        case TokenType::VOID:
        case TokenType::DELETE:
        case TokenType::IN:
        case TokenType::INSTANCEOF:

        case TokenType::EOF_TOKEN:
            return true;

        default:
            return false;
    }
}

int Lexer::utf8_whitespace_bytes() const {
    if (position_ >= source_.length()) return 0;
    unsigned char b1 = static_cast<unsigned char>(source_[position_]);
    if (b1 == 0xC2 && position_ + 1 < source_.length()) {
        unsigned char b2 = static_cast<unsigned char>(source_[position_ + 1]);
        if (b2 == 0xA0) return 2;
    }
    if (b1 == 0xE2 && position_ + 2 < source_.length()) {
        unsigned char b2 = static_cast<unsigned char>(source_[position_ + 1]);
        unsigned char b3 = static_cast<unsigned char>(source_[position_ + 2]);
        if (b2 == 0x80 && b3 >= 0x80 && b3 <= 0x8B) return 3;
        if (b2 == 0x80 && b3 == 0xAF) return 3;
        if (b2 == 0x81 && b3 == 0x9F) return 3;
    }
    if (b1 == 0xE3 && position_ + 2 < source_.length()) {
        unsigned char b2 = static_cast<unsigned char>(source_[position_ + 1]);
        unsigned char b3 = static_cast<unsigned char>(source_[position_ + 2]);
        if (b2 == 0x80 && b3 == 0x80) return 3;
    }
    if (b1 == 0xEF && position_ + 2 < source_.length()) {
        unsigned char b2 = static_cast<unsigned char>(source_[position_ + 1]);
        unsigned char b3 = static_cast<unsigned char>(source_[position_ + 2]);
        if (b2 == 0xBB && b3 == 0xBF) return 3;
    }
    return 0;
}

int Lexer::utf8_line_terminator_bytes() const {
    if (position_ >= source_.length()) return 0;
    unsigned char b1 = static_cast<unsigned char>(source_[position_]);
    if (b1 == 0xE2 && position_ + 2 < source_.length()) {
        unsigned char b2 = static_cast<unsigned char>(source_[position_ + 1]);
        unsigned char b3 = static_cast<unsigned char>(source_[position_ + 2]);
        if (b2 == 0x80 && (b3 == 0xA8 || b3 == 0xA9)) return 3;
    }
    return 0;
}

bool Lexer::is_whitespace(char ch) const {
    if (ch == ' ' || ch == '\t' || ch == '\v' || ch == '\f' || ch == '\r') {
        return true;
    }
    if (utf8_whitespace_bytes() > 0) return true;
    return false;
}

bool Lexer::is_line_terminator(char ch) const {
    if (ch == '\n' || ch == '\r') return true;
    if (utf8_line_terminator_bytes() > 0) return true;
    return false;
}

double Lexer::parse_decimal_literal() {
    std::string number_str;

    while (!at_end() && (is_digit(current_char()) || current_char() == '_')) {
        if (current_char() == '_') {
            advance();
            if (at_end() || !is_digit(current_char())) {
                add_error("SyntaxError: Invalid numeric separator");
                return 0.0;
            }
        } else {
            number_str += advance();
        }
    }

    if (!at_end() && current_char() == '.') {
        number_str += advance();
        if (!at_end() && current_char() == '_') {
            add_error("SyntaxError: Invalid numeric separator");
            return 0.0;
        }
        while (!at_end() && (is_digit(current_char()) || current_char() == '_')) {
            if (current_char() == '_') {
                advance();
                if (at_end() || !is_digit(current_char())) {
                    add_error("SyntaxError: Invalid numeric separator");
                    return 0.0;
                }
            } else {
                number_str += advance();
            }
        }
    }

    if (!at_end() && (current_char() == 'e' || current_char() == 'E')) {
        number_str += advance();
        if (!at_end() && (current_char() == '+' || current_char() == '-')) {
            number_str += advance();
        }
        while (!at_end() && (is_digit(current_char()) || current_char() == '_')) {
            if (current_char() == '_') {
                advance();
                if (at_end() || !is_digit(current_char())) {
                    add_error("SyntaxError: Invalid numeric separator");
                    return 0.0;
                }
            } else {
                number_str += advance();
            }
        }
    }

    return std::strtod(number_str.c_str(), nullptr);
}

double Lexer::parse_hex_literal() {
    double value = 0.0;
    while (!at_end() && (is_hex_digit(current_char()) || current_char() == '_')) {
        if (current_char() == '_') {
            advance();
            if (at_end() || !is_hex_digit(current_char())) {
                add_error("SyntaxError: Invalid numeric separator");
                return 0.0;
            }
            continue;
        }
        char ch = advance();
        int digit_value;
        if (is_digit(ch)) {
            digit_value = ch - '0';
        } else if (ch >= 'a' && ch <= 'f') {
            digit_value = ch - 'a' + 10;
        } else {
            digit_value = ch - 'A' + 10;
        }
        value = value * 16 + digit_value;
    }
    return value;
}

double Lexer::parse_binary_literal() {
    double value = 0.0;
    while (!at_end()) {
        if (current_char() == '_') {
            advance();
            if (at_end() || !is_binary_digit(current_char())) {
                add_error("SyntaxError: Invalid numeric separator");
                return 0.0;
            }
            continue;
        } else if (is_binary_digit(current_char())) {
            char ch = advance();
            value = value * 2 + (ch - '0');
        } else if (std::isdigit(current_char())) {
            add_error("SyntaxError: Invalid digit in binary literal");
            return 0.0;
        } else {
            break;
        }
    }
    return value;
}

double Lexer::parse_octal_literal() {
    double value = 0.0;
    while (!at_end() && (is_octal_digit(current_char()) || current_char() == '_')) {
        if (current_char() == '_') {
            advance();
            if (at_end() || !is_octal_digit(current_char())) {
                add_error("SyntaxError: Invalid numeric separator");
                return 0.0;
            }
            continue;
        }
        char ch = advance();
        value = value * 8 + (ch - '0');
    }
    return value;
}

double Lexer::parse_legacy_octal_literal() {
    double value = 0.0;
    advance();

    while (!at_end() && is_octal_digit(current_char())) {
        char ch = advance();
        value = value * 8 + (ch - '0');
    }
    return value;
}

std::string Lexer::parse_string_literal(char quote) {
    std::string value;

    while (!at_end() && current_char() != quote) {
        char ch = current_char();
        if (ch == '\n' || ch == '\r') {
            add_error("SyntaxError: Unterminated string literal");
            return value;
        }
        if (ch == '\\') {
            value += parse_escape_sequence();
        } else {
            value += advance();
        }
    }

    return value;
}

std::string Lexer::parse_escape_sequence() {
    advance();

    if (at_end()) {
        add_error("Unexpected end of input in escape sequence");
        return "\\";
    }

    char ch = current_char();

    // ES1: Octal escape sequences \0-\377 (up to 3 octal digits)
    if (ch >= '0' && ch <= '7') {
        // ES5: Octal escapes not allowed in strict mode (except \0 not followed by digit)
        if (options_.strict_mode) {
            if (ch != '0' || (peek_char(1) >= '0' && peek_char(1) <= '7')) {
                add_error("SyntaxError: Octal escape sequences are not allowed in strict mode");
            }
        }
        int octal_value = 0;
        int digit_count = 0;

        // Read up to 3 octal digits
        while (digit_count < 3 && !at_end() && current_char() >= '0' && current_char() <= '7') {
            octal_value = octal_value * 8 + (current_char() - '0');
            advance();
            digit_count++;

            // Stop if value would exceed 255 (max \377)
            if (octal_value > 255) {
                position_--;
                octal_value /= 8;
                break;
            }
        }

        return std::string(1, static_cast<char>(octal_value));
    }

    advance();
    switch (ch) {
        case 'n': return "\n";
        case 't': return "\t";
        case 'r': return "\r";
        case 'b': return "\b";
        case 'f': return "\f";
        case 'v': return "\v";
        case '\\': return "\\";
        case '\'': return "'";
        case '"': return "\"";
        case 'x': return parse_hex_escape();
        case 'u': return parse_unicode_escape();
        default: return std::string(1, ch);
    }
}

std::string Lexer::parse_hex_escape() {
    if (remaining() < 2) {
        add_error("Invalid hex escape sequence");
        return "";
    }
    
    char high = advance();
    char low = advance();
    
    if (!is_hex_digit(high) || !is_hex_digit(low)) {
        add_error("Invalid hex escape sequence");
        return "";
    }
    
    int value = 0;
    if (is_digit(high)) value += (high - '0') * 16;
    else if (high >= 'a' && high <= 'f') value += (high - 'a' + 10) * 16;
    else value += (high - 'A' + 10) * 16;
    
    if (is_digit(low)) value += low - '0';
    else if (low >= 'a' && low <= 'f') value += low - 'a' + 10;
    else value += low - 'A' + 10;
    
    if (value <= 0x7F) {
        return std::string(1, static_cast<char>(value));
    }
    // Encode as 2-byte UTF-8 (0x80-0xFF range)
    std::string utf8;
    utf8 += static_cast<char>(0xC0 | (value >> 6));
    utf8 += static_cast<char>(0x80 | (value & 0x3F));
    return utf8;
}

std::string Lexer::parse_unicode_escape() {
    // Check for \u{X...} format (ES6 extended unicode escapes)
    if (current_char() == '{') {
        advance(); // skip '{'
        std::string hex_digits;
        while (!at_end() && current_char() != '}') {
            char ch = current_char();
            if (!is_hex_digit(ch)) {
                add_error("Invalid unicode escape sequence");
                return "";
            }
            hex_digits += ch;
            advance();
        }

        if (current_char() != '}') {
            add_error("Invalid unicode escape sequence: missing '}'");
            return "";
        }
        advance(); // skip '}'

        if (hex_digits.empty() || hex_digits.length() > 6) {
            add_error("Invalid unicode escape sequence: invalid length");
            return "";
        }

        // Parse hex value
        uint32_t codepoint = 0;
        for (char c : hex_digits) {
            codepoint *= 16;
            if (c >= '0' && c <= '9') codepoint += c - '0';
            else if (c >= 'a' && c <= 'f') codepoint += c - 'a' + 10;
            else codepoint += c - 'A' + 10;
        }

        if (codepoint > 0x10FFFF) {
            add_error("Invalid unicode escape sequence: codepoint out of range");
            return "";
        }

        // Encode to UTF-8
        std::string result;
        if (codepoint < 0x80) {
            result += static_cast<char>(codepoint);
        } else if (codepoint < 0x800) {
            result += static_cast<char>(0xC0 | (codepoint >> 6));
            result += static_cast<char>(0x80 | (codepoint & 0x3F));
        } else if (codepoint < 0x10000) {
            result += static_cast<char>(0xE0 | (codepoint >> 12));
            result += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
            result += static_cast<char>(0x80 | (codepoint & 0x3F));
        } else {
            result += static_cast<char>(0xF0 | (codepoint >> 18));
            result += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
            result += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
            result += static_cast<char>(0x80 | (codepoint & 0x3F));
        }
        return result;
    }

    // Standard \uXXXX format (4 hex digits)
    if (remaining() < 4) {
        add_error("Invalid unicode escape sequence");
        return "";
    }

    char digits[4];
    for (int i = 0; i < 4; i++) {
        digits[i] = advance();
        if (!is_hex_digit(digits[i])) {
            add_error("Invalid unicode escape sequence");
            return "";
        }
    }

    // Parse 4 hex digits to get code unit
    uint16_t code_unit = 0;
    for (int i = 0; i < 4; i++) {
        code_unit *= 16;
        char c = digits[i];
        if (c >= '0' && c <= '9') code_unit += c - '0';
        else if (c >= 'a' && c <= 'f') code_unit += c - 'a' + 10;
        else code_unit += c - 'A' + 10;
    }

    // Check for surrogate pair: high surrogate followed by \uXXXX low surrogate
    if (code_unit >= 0xD800 && code_unit <= 0xDBFF) {
        // High surrogate - look ahead for \uXXXX low surrogate
        if (!at_end() && current_char() == '\\' && peek_char() == 'u') {
            size_t saved_pos = position_;
            Position saved_current = current_position_;
            advance(); // skip '\'
            advance(); // skip 'u'
            if (remaining() >= 4 && current_char() != '{') {
                char ld[4];
                bool valid = true;
                for (int i = 0; i < 4; i++) {
                    ld[i] = current_char();
                    if (!is_hex_digit(ld[i])) { valid = false; break; }
                    advance();
                }
                if (valid) {
                    uint16_t low_unit = 0;
                    for (int i = 0; i < 4; i++) {
                        low_unit *= 16;
                        char lc = ld[i];
                        if (lc >= '0' && lc <= '9') low_unit += lc - '0';
                        else if (lc >= 'a' && lc <= 'f') low_unit += lc - 'a' + 10;
                        else low_unit += lc - 'A' + 10;
                    }
                    if (low_unit >= 0xDC00 && low_unit <= 0xDFFF) {
                        // Valid surrogate pair - combine to single codepoint
                        uint32_t codepoint = 0x10000 + ((static_cast<uint32_t>(code_unit) - 0xD800) << 10) + (low_unit - 0xDC00);
                        std::string result;
                        result += static_cast<char>(0xF0 | (codepoint >> 18));
                        result += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
                        result += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                        result += static_cast<char>(0x80 | (codepoint & 0x3F));
                        return result;
                    }
                }
                // Not a valid low surrogate, restore position
                position_ = saved_pos;
                current_position_ = saved_current;
            } else {
                position_ = saved_pos;
                current_position_ = saved_current;
            }
        }
    }

    // Convert UTF-16 code unit to UTF-8
    std::string result;
    if (code_unit < 0x80) {
        result += static_cast<char>(code_unit);
    } else if (code_unit < 0x800) {
        result += static_cast<char>(0xC0 | (code_unit >> 6));
        result += static_cast<char>(0x80 | (code_unit & 0x3F));
    } else {
        result += static_cast<char>(0xE0 | (code_unit >> 12));
        result += static_cast<char>(0x80 | ((code_unit >> 6) & 0x3F));
        result += static_cast<char>(0x80 | (code_unit & 0x3F));
    }

    return result;
}

void Lexer::add_error(const std::string& message) {
    if (message.find("SyntaxError:") == 0) {
        errors_.push_back(message);
    } else {
        std::string error = "Lexer error at " + current_position_.to_string() + ": " + message;
        errors_.push_back(error);
    }
}

TokenType Lexer::lookup_keyword(const std::string& identifier) const {
    auto it = keywords_.find(identifier);
    return (it != keywords_.end()) ? it->second : TokenType::IDENTIFIER;
}

bool Lexer::is_reserved_word(const std::string& word) const {
    return keywords_.find(word) != keywords_.end();
}

bool Lexer::can_be_regex_literal() const {
    
    if (position_ == 0) return true;
    
    size_t pos = position_ - 1;
    while (pos > 0 && (is_whitespace(source_[pos]) || is_line_terminator(source_[pos]))) {
        pos--;
    }
    
    if (pos == 0) return true;
    
    char prev_char = source_[pos];
    
    return prev_char == '=' || prev_char == '(' || prev_char == '[' || 
           prev_char == '{' || prev_char == ',' || prev_char == ';' || 
           prev_char == ':' || prev_char == '!' || prev_char == '&' || 
           prev_char == '|' || prev_char == '?' || prev_char == '+' || 
           prev_char == '-' || prev_char == '*' || prev_char == '%' || 
           prev_char == '<' || prev_char == '>' || prev_char == '^' || 
           prev_char == '~';
}

Token Lexer::read_regex() {
    Position start = current_position_;
    advance();
    
    std::string pattern;
    
    while (!at_end() && current_char() != '/') {
        char ch = current_char();
        
        if (ch == '\\') {
            pattern += ch;
            advance();
            if (!at_end()) {
                pattern += current_char();
                advance();
            }
        } else if (ch == '\n' || ch == '\r') {
            add_error("Unterminated regex literal");
            return create_token(TokenType::INVALID, start);
        } else {
            pattern += ch;
            advance();
        }
    }
    
    if (at_end()) {
        add_error("Unterminated regex literal");
        return create_token(TokenType::INVALID, start);
    }
    
    advance();
    
    std::string flags;
    while (!at_end() && is_identifier_part(current_char())) {
        char flag = current_char();
        if (flag == 'g' || flag == 'i' || flag == 'm' || 
            flag == 's' || flag == 'u' || flag == 'y') {
            flags += flag;
            advance();
        } else {
            break;
        }
    }
    
    std::string regex_value = "/" + pattern + "/" + flags;
    return create_token(TokenType::REGEX, regex_value, start);
}

bool Lexer::is_at_line_start() const {
    if (current_position_.column == 1) {
        return true;
    }

    size_t check_pos = position_;
    while (check_pos > 0) {
        check_pos--;
        char ch = source_[check_pos];
        if (ch == '\n' || ch == '\r') {
            return true;  
        }
        if (!is_whitespace(ch)) {
            return false;  
        }
    }

    return true;
}

}
