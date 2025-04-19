#include "token.h"
#include <sstream>

namespace quanta {

Token::Token(TokenType type, const std::string& lexeme, const LiteralValue& literal, int line, int column)
    : type_(type), lexeme_(lexeme), literal_(literal), line_(line), column_(column) {
}

TokenType Token::get_type() const {
    return type_;
}

const std::string& Token::get_lexeme() const {
    return lexeme_;
}

const LiteralValue& Token::get_literal() const {
    return literal_;
}

int Token::get_line() const {
    return line_;
}

int Token::get_column() const {
    return column_;
}

std::string Token::to_string() const {
    std::stringstream ss;
    ss << "Token(" << token_type_to_string(type_) << ", '" << lexeme_ << "', ";
    
    // Handle different literal types
    if (std::holds_alternative<std::monostate>(literal_)) {
        ss << "null";
    } else if (std::holds_alternative<std::string>(literal_)) {
        ss << "\"" << std::get<std::string>(literal_) << "\"";
    } else if (std::holds_alternative<double>(literal_)) {
        ss << std::get<double>(literal_);
    } else if (std::holds_alternative<bool>(literal_)) {
        ss << (std::get<bool>(literal_) ? "true" : "false");
    } else if (std::holds_alternative<std::nullptr_t>(literal_)) {
        ss << "null";
    }
    
    ss << ", line: " << line_ << ", col: " << column_ << ")";
    return ss.str();
}

} // namespace quanta 