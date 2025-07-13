#include "Token.h"
#include <sstream>
#include <unordered_map>

namespace Quanta {

// Static token instance
const Token TokenSequence::EOF_TOKEN_INSTANCE = Token(TokenType::EOF_TOKEN, Position());

//=============================================================================
// Position Implementation
//=============================================================================

std::string Position::to_string() const {
    std::ostringstream oss;
    oss << line << ":" << column;
    return oss.str();
}

//=============================================================================
// Token Implementation
//=============================================================================

Token::Token() : type_(TokenType::EOF_TOKEN), numeric_value_(0), has_numeric_value_(false) {
}

Token::Token(TokenType type, const Position& pos) 
    : type_(type), start_(pos), end_(pos), numeric_value_(0), has_numeric_value_(false) {
}

Token::Token(TokenType type, const std::string& value, const Position& start, const Position& end)
    : type_(type), value_(value), start_(start), end_(end), numeric_value_(0), has_numeric_value_(false) {
}

Token::Token(TokenType type, double numeric_value, const Position& start, const Position& end)
    : type_(type), start_(start), end_(end), numeric_value_(numeric_value), has_numeric_value_(true) {
    // Convert numeric value to string
    std::ostringstream oss;
    oss << numeric_value;
    value_ = oss.str();
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
           type_ == TokenType::UNDEFINED ||
           type_ == TokenType::TEMPLATE_LITERAL;
}

bool Token::is_punctuation() const {
    return type_ >= TokenType::LEFT_PAREN && type_ <= TokenType::QUESTION;
}

std::string Token::to_string() const {
    std::ostringstream oss;
    oss << type_name() << "('" << value_ << "' at " << start_.to_string() << ")";
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
        {TokenType::UNDEFINED, "UNDEFINED"},
        
        // Keywords
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
        
        // Operators
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
        
        // Punctuation
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
        {TokenType::QUESTION, 3}, // Conditional operator
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
        {TokenType::EXPONENT, 14} // Right associative
    };
    
    auto it = precedence.find(type);
    return (it != precedence.end()) ? it->second : 0;
}

bool Token::is_right_associative(TokenType type) {
    return type == TokenType::EXPONENT ||
           is_assignment_operator(type);
}

//=============================================================================
// TokenSequence Implementation
//=============================================================================

TokenSequence::TokenSequence() : position_(0) {
}

TokenSequence::TokenSequence(std::vector<Token> tokens) 
    : tokens_(std::move(tokens)), position_(0) {
}

const Token& TokenSequence::current() const {
    if (position_ < tokens_.size()) {
        return tokens_[position_];
    }
    return EOF_TOKEN_INSTANCE;
}

const Token& TokenSequence::peek(size_t offset) const {
    size_t peek_pos = position_ + offset;
    if (peek_pos < tokens_.size()) {
        return tokens_[peek_pos];
    }
    return EOF_TOKEN_INSTANCE;
}

const Token& TokenSequence::previous() const {
    if (position_ > 0 && position_ - 1 < tokens_.size()) {
        return tokens_[position_ - 1];
    }
    return EOF_TOKEN_INSTANCE;
}

void TokenSequence::advance() {
    if (position_ < tokens_.size()) {
        position_++;
    }
}

void TokenSequence::retreat() {
    if (position_ > 0) {
        position_--;
    }
}

bool TokenSequence::at_end() const {
    return position_ >= tokens_.size() || current().is_eof();
}

void TokenSequence::set_position(size_t pos) {
    position_ = std::min(pos, tokens_.size());
}

const Token& TokenSequence::operator[](size_t index) const {
    if (index < tokens_.size()) {
        return tokens_[index];
    }
    return EOF_TOKEN_INSTANCE;
}

void TokenSequence::push_back(const Token& token) {
    tokens_.push_back(token);
}

std::string TokenSequence::to_string() const {
    std::ostringstream oss;
    oss << "TokenSequence[" << tokens_.size() << " tokens, pos=" << position_ << "]";
    return oss.str();
}

} // namespace Quanta