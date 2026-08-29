/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/lexer/Token.h"
#include "quanta/lexer/Lexer.h"
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <unordered_map>

namespace Quanta {

const Token TokenSequence::EOF_TOKEN_INSTANCE = Token(TokenType::EOF_TOKEN, Position());


std::string Position::to_string() const {
    std::ostringstream oss;
    oss << line << ":" << column;
    return oss.str();
}


Token::Token()
    : numeric_value_(0), value_off_(0), value_len_(0), type_(TokenType::EOF_TOKEN), flags_(0) {
}

Token::Token(TokenType type, const Position& pos)
    : numeric_value_(0), start_(pos), end_(pos), value_off_(0), value_len_(0), type_(type), flags_(0) {
}

Token::Token(TokenType type, const Position& start, const Position& end)
    : numeric_value_(0), start_(start), end_(end), value_off_(0), value_len_(0), type_(type), flags_(0) {
}

bool Token::is_keyword() const {
    return type_ >= TokenType::BREAK && type_ <= TokenType::STATIC;
}

bool Token::is_operator() const {
    return type_ >= TokenType::PLUS && type_ <= TokenType::LOGICAL_OR_ASSIGN;
}

bool Token::is_literal() const {
    return type_ == TokenType::NUMBER || 
           type_ == TokenType::STRING || 
           type_ == TokenType::BOOLEAN || 
           type_ == TokenType::NULL_LITERAL ||
           type_ == TokenType::BIGINT_LITERAL ||
           type_ == TokenType::UNDEFINED ||
           type_ == TokenType::TEMPLATE_LITERAL;
}

bool Token::is_punctuation() const {
    return type_ >= TokenType::LEFT_PAREN && type_ <= TokenType::QUESTION;
}

std::string Token::to_string() const {
    std::ostringstream oss;
    oss << type_name() << "(at " << start_.to_string() << ")";
    return oss.str();
}

std::string Token::type_name() const {
    return token_type_name(type_);
}

std::string Token::token_type_name(TokenType type) {
    static const std::unordered_map<TokenType, std::string> names = {
        {TokenType::EOF_TOKEN, "EOF"},
        {TokenType::IDENTIFIER, "IDENTIFIER"},
        {TokenType::NUMBER, "NUMBER"},
        {TokenType::STRING, "STRING"},
        {TokenType::TEMPLATE_LITERAL, "TEMPLATE_LITERAL"},
        {TokenType::BOOLEAN, "BOOLEAN"},
        {TokenType::NULL_LITERAL, "NULL"},
        {TokenType::BIGINT_LITERAL, "BIGINT"},
        {TokenType::UNDEFINED, "UNDEFINED"},
        
        {TokenType::BREAK, "BREAK"},
        {TokenType::CASE, "CASE"},
        {TokenType::CATCH, "CATCH"},
        {TokenType::CLASS, "CLASS"},
        {TokenType::CONST, "CONST"},
        {TokenType::CONTINUE, "CONTINUE"},
        {TokenType::DEBUGGER, "DEBUGGER"},
        {TokenType::DEFAULT, "DEFAULT"},
        {TokenType::DELETE, "DELETE"},
        {TokenType::DO, "DO"},
        {TokenType::ELSE, "ELSE"},
        {TokenType::EXPORT, "EXPORT"},
        {TokenType::EXTENDS, "EXTENDS"},
        {TokenType::FINALLY, "FINALLY"},
        {TokenType::FOR, "FOR"},
        {TokenType::FUNCTION, "FUNCTION"},
        {TokenType::IF, "IF"},
        {TokenType::IMPORT, "IMPORT"},
        {TokenType::IN, "IN"},
        {TokenType::INSTANCEOF, "INSTANCEOF"},
        {TokenType::LET, "LET"},
        {TokenType::NEW, "NEW"},
        {TokenType::RETURN, "RETURN"},
        {TokenType::SUPER, "SUPER"},
        {TokenType::SWITCH, "SWITCH"},
        {TokenType::THIS, "THIS"},
        {TokenType::THROW, "THROW"},
        {TokenType::TRY, "TRY"},
        {TokenType::TYPEOF, "TYPEOF"},
        {TokenType::VAR, "VAR"},
        {TokenType::VOID, "VOID"},
        {TokenType::WHILE, "WHILE"},
        {TokenType::WITH, "WITH"},
        {TokenType::YIELD, "YIELD"},
        {TokenType::ASYNC, "ASYNC"},
        {TokenType::AWAIT, "AWAIT"},
        {TokenType::FROM, "FROM"},
        {TokenType::OF, "OF"},
        {TokenType::STATIC, "STATIC"},
        
        {TokenType::PLUS, "PLUS"},
        {TokenType::MINUS, "MINUS"},
        {TokenType::MULTIPLY, "MULTIPLY"},
        {TokenType::DIVIDE, "DIVIDE"},
        {TokenType::MODULO, "MODULO"},
        {TokenType::EXPONENT, "EXPONENT"},
        
        {TokenType::ASSIGN, "ASSIGN"},
        {TokenType::PLUS_ASSIGN, "PLUS_ASSIGN"},
        {TokenType::MINUS_ASSIGN, "MINUS_ASSIGN"},
        {TokenType::MULTIPLY_ASSIGN, "MULTIPLY_ASSIGN"},
        {TokenType::DIVIDE_ASSIGN, "DIVIDE_ASSIGN"},
        {TokenType::MODULO_ASSIGN, "MODULO_ASSIGN"},
        {TokenType::EXPONENT_ASSIGN, "EXPONENT_ASSIGN"},
        
        {TokenType::INCREMENT, "INCREMENT"},
        {TokenType::DECREMENT, "DECREMENT"},
        
        {TokenType::EQUAL, "EQUAL"},
        {TokenType::NOT_EQUAL, "NOT_EQUAL"},
        {TokenType::STRICT_EQUAL, "STRICT_EQUAL"},
        {TokenType::STRICT_NOT_EQUAL, "STRICT_NOT_EQUAL"},
        {TokenType::LESS_THAN, "LESS_THAN"},
        {TokenType::GREATER_THAN, "GREATER_THAN"},
        {TokenType::LESS_EQUAL, "LESS_EQUAL"},
        {TokenType::GREATER_EQUAL, "GREATER_EQUAL"},
        {TokenType::LOGICAL_AND, "LOGICAL_AND"},
        {TokenType::LOGICAL_OR, "LOGICAL_OR"},
        {TokenType::LOGICAL_NOT, "LOGICAL_NOT"},
        {TokenType::ARROW, "ARROW"},
        {TokenType::ELLIPSIS, "ELLIPSIS"},
        
        {TokenType::OPTIONAL_CHAINING, "OPTIONAL_CHAINING"},
        {TokenType::NULLISH_COALESCING, "NULLISH_COALESCING"},
        {TokenType::NULLISH_ASSIGN, "NULLISH_ASSIGN"},
        {TokenType::LOGICAL_AND_ASSIGN, "LOGICAL_AND_ASSIGN"},
        {TokenType::LOGICAL_OR_ASSIGN, "LOGICAL_OR_ASSIGN"},
        
        {TokenType::LEFT_PAREN, "LEFT_PAREN"},
        {TokenType::RIGHT_PAREN, "RIGHT_PAREN"},
        {TokenType::LEFT_BRACE, "LEFT_BRACE"},
        {TokenType::RIGHT_BRACE, "RIGHT_BRACE"},
        {TokenType::LEFT_BRACKET, "LEFT_BRACKET"},
        {TokenType::RIGHT_BRACKET, "RIGHT_BRACKET"},
        {TokenType::SEMICOLON, "SEMICOLON"},
        {TokenType::COMMA, "COMMA"},
        {TokenType::DOT, "DOT"},
        {TokenType::COLON, "COLON"},
        {TokenType::QUESTION, "QUESTION"},
        {TokenType::HASH, "HASH"},
        {TokenType::AT, "AT"},
        {TokenType::ARROW, "ARROW"},
        {TokenType::ELLIPSIS, "ELLIPSIS"},

        {TokenType::INVALID, "INVALID"}
    };
    
    auto it = names.find(type);
    return (it != names.end()) ? it->second : "UNKNOWN";
}

bool Token::is_assignment_operator(TokenType type) {
    return type == TokenType::ASSIGN ||
           type == TokenType::PLUS_ASSIGN ||
           type == TokenType::MINUS_ASSIGN ||
           type == TokenType::MULTIPLY_ASSIGN ||
           type == TokenType::DIVIDE_ASSIGN ||
           type == TokenType::MODULO_ASSIGN ||
           type == TokenType::EXPONENT_ASSIGN ||
           type == TokenType::BITWISE_AND_ASSIGN ||
           type == TokenType::BITWISE_OR_ASSIGN ||
           type == TokenType::BITWISE_XOR_ASSIGN ||
           type == TokenType::LEFT_SHIFT_ASSIGN ||
           type == TokenType::RIGHT_SHIFT_ASSIGN ||
           type == TokenType::UNSIGNED_RIGHT_SHIFT_ASSIGN ||
           type == TokenType::NULLISH_ASSIGN ||
           type == TokenType::LOGICAL_AND_ASSIGN ||
           type == TokenType::LOGICAL_OR_ASSIGN;
}

bool Token::is_binary_operator(TokenType type) {
    return type == TokenType::PLUS ||
           type == TokenType::MINUS ||
           type == TokenType::MULTIPLY ||
           type == TokenType::DIVIDE ||
           type == TokenType::MODULO ||
           type == TokenType::EXPONENT ||
           type == TokenType::EQUAL ||
           type == TokenType::NOT_EQUAL ||
           type == TokenType::STRICT_EQUAL ||
           type == TokenType::STRICT_NOT_EQUAL ||
           type == TokenType::LESS_THAN ||
           type == TokenType::GREATER_THAN ||
           type == TokenType::LESS_EQUAL ||
           type == TokenType::GREATER_EQUAL ||
           type == TokenType::LOGICAL_AND ||
           type == TokenType::LOGICAL_OR ||
           type == TokenType::BITWISE_AND ||
           type == TokenType::BITWISE_OR ||
           type == TokenType::BITWISE_XOR ||
           type == TokenType::LEFT_SHIFT ||
           type == TokenType::RIGHT_SHIFT ||
           type == TokenType::UNSIGNED_RIGHT_SHIFT ||
           type == TokenType::INSTANCEOF ||
           type == TokenType::IN;
}

bool Token::is_unary_operator(TokenType type) {
    return type == TokenType::PLUS ||
           type == TokenType::MINUS ||
           type == TokenType::LOGICAL_NOT ||
           type == TokenType::BITWISE_NOT ||
           type == TokenType::TYPEOF ||
           type == TokenType::VOID ||
           type == TokenType::DELETE ||
           type == TokenType::INCREMENT ||
           type == TokenType::DECREMENT;
}

bool Token::is_comparison_operator(TokenType type) {
    return type == TokenType::EQUAL ||
           type == TokenType::NOT_EQUAL ||
           type == TokenType::STRICT_EQUAL ||
           type == TokenType::STRICT_NOT_EQUAL ||
           type == TokenType::LESS_THAN ||
           type == TokenType::GREATER_THAN ||
           type == TokenType::LESS_EQUAL ||
           type == TokenType::GREATER_EQUAL;
}

int Token::get_precedence(TokenType type) {
    static const std::unordered_map<TokenType, int> precedence = {
        {TokenType::COMMA, 1},
        {TokenType::ASSIGN, 2},
        {TokenType::PLUS_ASSIGN, 2},
        {TokenType::MINUS_ASSIGN, 2},
        {TokenType::MULTIPLY_ASSIGN, 2},
        {TokenType::DIVIDE_ASSIGN, 2},
        {TokenType::MODULO_ASSIGN, 2},
        {TokenType::QUESTION, 3},
        {TokenType::LOGICAL_OR, 4},
        {TokenType::LOGICAL_AND, 5},
        {TokenType::BITWISE_OR, 6},
        {TokenType::BITWISE_XOR, 7},
        {TokenType::BITWISE_AND, 8},
        {TokenType::EQUAL, 9},
        {TokenType::NOT_EQUAL, 9},
        {TokenType::STRICT_EQUAL, 9},
        {TokenType::STRICT_NOT_EQUAL, 9},
        {TokenType::LESS_THAN, 10},
        {TokenType::GREATER_THAN, 10},
        {TokenType::LESS_EQUAL, 10},
        {TokenType::GREATER_EQUAL, 10},
        {TokenType::INSTANCEOF, 10},
        {TokenType::IN, 10},
        {TokenType::LEFT_SHIFT, 11},
        {TokenType::RIGHT_SHIFT, 11},
        {TokenType::UNSIGNED_RIGHT_SHIFT, 11},
        {TokenType::PLUS, 12},
        {TokenType::MINUS, 12},
        {TokenType::MULTIPLY, 13},
        {TokenType::DIVIDE, 13},
        {TokenType::MODULO, 13},
        {TokenType::EXPONENT, 14}
    };
    
    auto it = precedence.find(type);
    return (it != precedence.end()) ? it->second : 0;
}

bool Token::is_right_associative(TokenType type) {
    return type == TokenType::EXPONENT ||
           is_assignment_operator(type);
}


TokenSequence::TokenSequence() : position_(0) {
}

TokenSequence::TokenSequence(std::vector<Token> tokens,
                             std::shared_ptr<const std::string> source,
                             std::vector<std::string> owned_values)
    : source_(std::move(source)), owned_values_(std::move(owned_values)), position_(0) {
    for (const Token& t : tokens) push_back(t);
}

TokenSequence::TokenSequence(std::shared_ptr<const std::string> source)
    : source_(std::move(source)), position_(0) {
}

const std::string& TokenSequence::source() const {
    static const std::string empty;
    return source_ ? *source_ : empty;
}

std::string_view token_value_text(const Token& token, const std::string& source,
                                  const std::vector<std::string>& owned_values) {
    if (token.value_is_owned()) {
        size_t index = token.value_offset();
        if (index >= owned_values.size()) return std::string_view();
        return std::string_view(owned_values[index]);
    }
    if (token.value_length() == 0) return std::string_view();
    size_t offset = token.value_offset();
    size_t length = token.value_length();
    if (offset + length > source.size()) return std::string_view();
    return std::string_view(source.data() + offset, length);
}

std::string_view TokenSequence::text_of(const Token& token) const {
    return token_value_text(token, source(), owned_values_);
}

const Token& TokenSequence::current() const {
    if (position_ < size()) {
        return (*this)[position_];
    }
    return EOF_TOKEN_INSTANCE;
}

const Token& TokenSequence::peek(size_t offset) const {
    size_t peek_pos = position_ + offset;
    if (peek_pos < size()) {
        return (*this)[peek_pos];
    }
    return EOF_TOKEN_INSTANCE;
}

const Token& TokenSequence::previous() const {
    if (position_ > 0 && position_ - 1 < size()) {
        return (*this)[position_ - 1];
    }
    return EOF_TOKEN_INSTANCE;
}

void TokenSequence::advance() {
    if (position_ < size()) {
        position_++;
    }
}

void TokenSequence::retreat() {
    if (position_ > 0) {
        position_--;
    }
}

bool TokenSequence::at_end() const {
    return position_ >= size() || current().is_eof();
}

void TokenSequence::set_position(size_t pos) {
    position_ = std::min(pos, size());
}

const std::vector<std::string>* TokenSequence::lex_errors() const {
    return lexer_ ? &lexer_->get_errors() : nullptr;
}

void TokenSequence::stream_from(std::shared_ptr<Lexer> lexer) {
    lexer_ = std::move(lexer);
    eof_seen_ = false;
}

// Pulls tokens until `index` is one of them, or the file runs out. The
// filtering matches what a full tokenize does, so the two produce the same
// sequence for the same source.
void TokenSequence::pump_to(size_t index) {
    while (count_ <= index && !eof_seen_) {
        // A sequence is moved on its way into the parser, so the lexer is
        // pointed back at the live one here rather than once at the start.
        lexer_->set_tokens_so_far(this);
        Token token = lexer_->next_token();
        // A rewritten value (an escape in a string or a name) lives beside the
        // tokens rather than in the source, and the lexer only ever appends.
        const std::vector<std::string>& owned = lexer_->owned_values();
        for (size_t i = owned_values_.size(); i < owned.size(); i++) {
            owned_values_.push_back(owned[i]);
        }
        const TokenType type = token.get_type();
        if (type != TokenType::WHITESPACE && type != TokenType::COMMENT &&
            type != TokenType::NEWLINE) {
            lexer_->set_last_token_type(type);
        }
        if ((lexer_->options().skip_whitespace && type == TokenType::WHITESPACE) ||
            (lexer_->options().skip_comments && type == TokenType::COMMENT)) {
            if (lexer_->at_end()) break;
            continue;
        }
        // The directive prologue is read as it goes past: a leading
        // "use strict" decides how everything after it is lexed, and a full
        // tokenize finds it the same way.
        const bool is_first = (count_ == 0);
        push_back(token);
        if (is_first && !strict_directive_seen_ && type == TokenType::STRING) {
            strict_directive_seen_ = true;
            if (lexer_->token_text(token) == "use strict") lexer_->enter_strict_mode();
        }
        if (type == TokenType::EOF_TOKEN) { eof_seen_ = true; break; }
        if (lexer_->at_end()) {
            push_back(Token(TokenType::EOF_TOKEN, lexer_->current_position()));
            eof_seen_ = true;
            break;
        }
    }
}

// Hands back the blocks the parser can no longer reach.
void TokenSequence::release_behind() {
    release_check_at_ = cursor_ + kKeepBehind;
    if (cursor_ < kKeepBehind) return;
    const size_t keep_from = cursor_ - kKeepBehind;
    size_t block, slot;
    locate(keep_from, block, slot);
    for (size_t b = released_blocks_; b < block && b < blocks_.size(); b++) {
        blocks_[b].reset();
    }
    released_blocks_ = block;
}

const Token& TokenSequence::operator[](size_t index) const {
    if (lexer_ && index >= count_ && !eof_seen_) {
        const_cast<TokenSequence*>(this)->pump_to(index);
    }
    if (index >= count_) return EOF_TOKEN_INSTANCE;
    size_t block, slot;
    locate(index, block, slot);
    // A block handed back is one the parser had already moved well past.
    // Reading one would be reading freed memory, so the answer is that the
    // file ended -- but it means the window was too small, which is a bug in
    // the sizing rather than in the source, so a checking build says so.
    if (!blocks_[block]) {
#ifdef QUANTA_VALIDATE_BYTECODE
        std::fprintf(stderr, "[token] released block reached: index=%zu cursor=%zu\n",
                     index, cursor_);
        std::abort();
#endif
        return EOF_TOKEN_INSTANCE;
    }
    return blocks_[block][slot];
}

void TokenSequence::push_back(const Token& token) {
    size_t block, slot;
    locate(count_, block, slot);
    if (block == blocks_.size()) {
        blocks_.push_back(std::make_unique<Token[]>(block_capacity(block)));
    }
    blocks_[block][slot] = token;
    ++count_;
}

std::string TokenSequence::to_string() const {
    std::ostringstream oss;
    oss << "TokenSequence[" << size() << " tokens, pos=" << position_ << "]";
    return oss.str();
}

}
