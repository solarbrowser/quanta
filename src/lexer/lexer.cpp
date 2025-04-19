#include "lexer.h"
#include <sstream>

namespace quanta {

// LexerError implementation
LexerError::LexerError(const std::string& message, int line, int column)
    : std::runtime_error(message), line_(line), column_(column) {
}

int LexerError::get_line() const {
    return line_;
}

int LexerError::get_column() const {
    return column_;
}

// Lexer implementation
Lexer::Lexer(const std::string& source)
    : source_(source) {
}

std::vector<Token> Lexer::scan_tokens() {
    while (!is_at_end()) {
        // Beginning of the next lexeme
        start_ = current_;
        scan_token();
    }

    // Add EOF token
    tokens_.emplace_back(TokenType::END_OF_FILE, "", std::monostate{}, line_, column_);
    return tokens_;
}

void Lexer::scan_token() {
    char c = advance();
    
    switch (c) {
        // Single-character tokens
        case '(': add_token(TokenType::LEFT_PAREN); break;
        case ')': add_token(TokenType::RIGHT_PAREN); break;
        case '{': add_token(TokenType::LEFT_BRACE); break;
        case '}': add_token(TokenType::RIGHT_BRACE); break;
        case '[': add_token(TokenType::LEFT_BRACKET); break;
        case ']': add_token(TokenType::RIGHT_BRACKET); break;
        case ',': add_token(TokenType::COMMA); break;
        case '.': add_token(TokenType::DOT); break;
        case ';': add_token(TokenType::SEMICOLON); break;
        case ':': add_token(TokenType::COLON); break;
        
        // Operators with one or two characters
        case '+': 
            if (match('=')) {
                add_token(TokenType::PLUS_EQUAL);
            } else {
                add_token(TokenType::PLUS);
            }
            break;
        case '-': 
            if (match('=')) {
                add_token(TokenType::MINUS_EQUAL);
            } else {
                add_token(TokenType::MINUS);
            }
            break;
        case '*': 
            if (match('=')) {
                add_token(TokenType::STAR_EQUAL);
            } else {
                add_token(TokenType::STAR);
            }
            break;
        case '/': 
            if (match('/')) {
                // Comment goes until the end of the line
                while (peek() != '\n' && !is_at_end()) {
                    advance();
                }
            } else if (match('*')) {
                // Multi-line comment
                multi_line_comment();
            } else if (match('=')) {
                add_token(TokenType::SLASH_EQUAL);
            } else {
                add_token(TokenType::SLASH);
            }
            break;
        case '%': 
            if (match('=')) {
                add_token(TokenType::PERCENT_EQUAL);
            } else {
                add_token(TokenType::PERCENT);
            }
            break;
        
        // Comparison operators
        case '!': 
            add_token(match('=') ? TokenType::BANG_EQUAL : TokenType::BANG);
            break;
        case '=': 
            add_token(match('=') ? TokenType::EQUAL_EQUAL : TokenType::EQUAL);
            break;
        case '<': 
            add_token(match('=') ? TokenType::LESS_EQUAL : TokenType::LESS);
            break;
        case '>': 
            add_token(match('=') ? TokenType::GREATER_EQUAL : TokenType::GREATER);
            break;
        
        // Logical operators
        case '&': 
            if (match('&')) {
                add_token(TokenType::AND);
            } else {
                error(std::string("Unexpected character '") + c + "'");
            }
            break;
        case '|': 
            if (match('|')) {
                add_token(TokenType::OR);
            } else {
                error(std::string("Unexpected character '") + c + "'");
            }
            break;
        
        // Whitespace
        case ' ':
        case '\r':
        case '\t':
            // Ignore whitespace
            column_++;
            break;
        case '\n':
            // Increment line counter on newline
            line_++;
            column_ = 1;
            break;
        
        // String literals
        case '"': 
            string('"'); 
            break;
        case '\'': 
            string('\''); 
            break;
        case '`': 
            string('`'); 
            break;
        
        default:
            // Number literals
            if (is_digit(c)) {
                number();
            }
            // Identifiers and keywords
            else if (is_alpha(c)) {
                identifier();
            }
            else {
                error(std::string("Unexpected character '") + c + "'");
            }
            break;
    }
}

void Lexer::multi_line_comment() {
    // Keep consuming until we find */
    while (!is_at_end()) {
        char c = advance();
        if (c == '\n') {
            line_++;
            column_ = 1;
        } else if (c == '*' && peek() == '/') {
            // End of comment found
            advance();  // Consume the closing '/'
            break;
        }
    }
}

void Lexer::string(char quote) {
    while (peek() != quote && !is_at_end()) {
        if (peek() == '\n') {
            line_++;
            column_ = 1;
        }
        advance();
    }

    if (is_at_end()) {
        error("Unterminated string.");
        return;
    }

    // The closing quote
    advance();

    // Trim the quotes
    std::string value = source_.substr(start_ + 1, current_ - start_ - 2);
    add_token(TokenType::STRING, value);
}

void Lexer::number() {
    while (is_digit(peek())) {
        advance();
    }

    // Look for a decimal part
    if (peek() == '.' && is_digit(peek_next())) {
        // Consume the decimal point
        advance();

        // Consume decimal digits
        while (is_digit(peek())) {
            advance();
        }
    }

    // Parse the number value
    double value = std::stod(source_.substr(start_, current_ - start_));
    add_token(TokenType::NUMBER, value);
}

void Lexer::identifier() {
    while (is_alphanumeric(peek())) {
        advance();
    }

    // Check if it's a keyword
    std::string text = source_.substr(start_, current_ - start_);
    auto it = keywords.find(text);
    TokenType type = (it != keywords.end()) ? it->second : TokenType::IDENTIFIER;
    
    // For true/false/null/undefined literals, add the appropriate value
    LiteralValue literal = std::monostate{};
    
    if (type == TokenType::TRUE) {
        literal = true;
    } else if (type == TokenType::FALSE) {
        literal = false;
    } else if (type == TokenType::NULL_LITERAL) {
        literal = nullptr;
    } else if (type == TokenType::IDENTIFIER) {
        literal = text;
    }
    
    add_token(type, literal);
}

void Lexer::add_token(TokenType type) {
    add_token(type, std::monostate{});
}

void Lexer::add_token(TokenType type, const LiteralValue& literal) {
    std::string text = source_.substr(start_, current_ - start_);
    tokens_.emplace_back(type, text, literal, line_, column_ - text.length());
    column_ += text.length();
}

bool Lexer::match(char expected) {
    if (is_at_end()) return false;
    if (source_[current_] != expected) return false;

    current_++;
    return true;
}

char Lexer::peek() const {
    if (is_at_end()) return '\0';
    return source_[current_];
}

char Lexer::peek_next() const {
    if (current_ + 1 >= source_.length()) return '\0';
    return source_[current_ + 1];
}

bool Lexer::is_alpha(char c) const {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           c == '_' || c == '$';
}

bool Lexer::is_digit(char c) const {
    return c >= '0' && c <= '9';
}

bool Lexer::is_alphanumeric(char c) const {
    return is_alpha(c) || is_digit(c);
}

char Lexer::advance() {
    return source_[current_++];
}

bool Lexer::is_at_end() const {
    return current_ >= source_.length();
}

void Lexer::error(const std::string& message) {
    throw LexerError(message, line_, column_);
}

} // namespace quanta 