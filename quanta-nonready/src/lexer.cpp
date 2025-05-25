//<---------QUANTA JS ENGINE - LEXER IMPLEMENTATION--------->
// Stage 1: Core Engine & Runtime - Tokenizer (Lexer)
// Purpose: Convert JavaScript source code into tokens
// Max Lines: 5000 (Current: ~300)

#include "../include/lexer.h"
#include <cctype>
#include <stdexcept>

namespace Quanta {

//<---------LEXER CONSTRUCTOR--------->
Lexer::Lexer(const std::string& source) 
    : source(source), position(0), line(1), column(1) {
    initKeywords();
}

//<---------KEYWORD INITIALIZATION--------->
void Lexer::initKeywords() {
    keywords["let"] = TokenType::LET;
    keywords["const"] = TokenType::CONST;
    keywords["var"] = TokenType::VAR;
    keywords["function"] = TokenType::FUNCTION;
    keywords["return"] = TokenType::RETURN;
    keywords["if"] = TokenType::IF;
    keywords["else"] = TokenType::ELSE;
    keywords["for"] = TokenType::FOR;
    keywords["while"] = TokenType::WHILE;
    keywords["class"] = TokenType::CLASS;
    keywords["true"] = TokenType::BOOLEAN;
    keywords["false"] = TokenType::BOOLEAN;
    keywords["null"] = TokenType::NULL_LITERAL;
    keywords["undefined"] = TokenType::UNDEFINED;
}

//<---------CHARACTER OPERATIONS--------->
char Lexer::currentChar() {
    if (position >= source.length()) return '\0';
    return source[position];
}

char Lexer::peekChar(size_t offset) {
    size_t pos = position + offset;
    if (pos >= source.length()) return '\0';
    return source[pos];
}

void Lexer::advance() {
    if (position < source.length()) {
        if (source[position] == '\n') {
            line++;
            column = 1;
        } else {
            column++;
        }
        position++;
    }
}

//<---------WHITESPACE HANDLING--------->
void Lexer::skipWhitespace() {
    while (std::isspace(currentChar()) && currentChar() != '\n') {
        advance();
    }
}

//<---------NUMBER SCANNING--------->
Token Lexer::scanNumber() {
    size_t startLine = line;
    size_t startColumn = column;
    size_t startPos = position;
    std::string value;
    
    while (std::isdigit(currentChar()) || currentChar() == '.') {
        value += currentChar();
        advance();
    }
    
    return Token(TokenType::NUMBER, value, startLine, startColumn, startPos);
}

//<---------STRING SCANNING--------->
Token Lexer::scanString(char quote) {
    size_t startLine = line;
    size_t startColumn = column;
    size_t startPos = position;
    std::string value;
    
    advance(); // Skip opening quote
    
    while (currentChar() != quote && currentChar() != '\0') {
        if (currentChar() == '\\') {
            advance();
            switch (currentChar()) {
                case 'n': value += '\n'; break;
                case 't': value += '\t'; break;
                case 'r': value += '\r'; break;
                case '\\': value += '\\'; break;
                case '"': value += '"'; break;
                case '\'': value += '\''; break;
                default: value += currentChar(); break;
            }
        } else {
            value += currentChar();
        }
        advance();
    }
    
    if (currentChar() == quote) {
        advance(); // Skip closing quote
    }
    
    return Token(TokenType::STRING, value, startLine, startColumn, startPos);
}

//<---------IDENTIFIER SCANNING--------->
Token Lexer::scanIdentifier() {
    size_t startLine = line;
    size_t startColumn = column;
    size_t startPos = position;
    std::string value;
    
    while (std::isalnum(currentChar()) || currentChar() == '_' || currentChar() == '$') {
        value += currentChar();
        advance();
    }
      TokenType type = TokenType::IDENTIFIER;
    TokenType* found = keywords.find(value);
    if (found) {
        type = *found;
    }
    
    return Token(type, value, startLine, startColumn, startPos);
}

//<---------OPERATOR SCANNING--------->
Token Lexer::scanOperator() {
    size_t startLine = line;
    size_t startColumn = column;
    size_t startPos = position;
    
    char ch = currentChar();
    advance();
    
    switch (ch) {
        case '+': return Token(TokenType::PLUS, "+", startLine, startColumn, startPos);
        case '-': return Token(TokenType::MINUS, "-", startLine, startColumn, startPos);
        case '*': return Token(TokenType::MULTIPLY, "*", startLine, startColumn, startPos);
        case '/': return Token(TokenType::DIVIDE, "/", startLine, startColumn, startPos);
        case '%': return Token(TokenType::MODULO, "%", startLine, startColumn, startPos);
        case '=': 
            if (currentChar() == '=') {
                advance();
                return Token(TokenType::EQUALS, "==", startLine, startColumn, startPos);
            }
            return Token(TokenType::ASSIGN, "=", startLine, startColumn, startPos);
        case '!':
            if (currentChar() == '=') {
                advance();
                return Token(TokenType::NOT_EQUALS, "!=", startLine, startColumn, startPos);
            }
            return Token(TokenType::INVALID, "!", startLine, startColumn, startPos);
        case '<': return Token(TokenType::LESS_THAN, "<", startLine, startColumn, startPos);
        case '>': return Token(TokenType::GREATER_THAN, ">", startLine, startColumn, startPos);
        default:
            return Token(TokenType::INVALID, std::string(1, ch), startLine, startColumn, startPos);
    }
}

//<---------MAIN TOKENIZATION--------->
std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;
    
    while (hasMoreTokens()) {
        Token token = nextToken();
        if (token.type != TokenType::WHITESPACE) {
            tokens.push_back(token);
        }
    }
    
    return tokens;
}

Token Lexer::nextToken() {
    skipWhitespace();
    
    char ch = currentChar();
    size_t startLine = line;
    size_t startColumn = column;
    size_t startPos = position;
    
    if (ch == '\0') {
        return Token(TokenType::EOF_TOKEN, "", startLine, startColumn, startPos);
    }
    
    // Numbers
    if (std::isdigit(ch)) {
        return scanNumber();
    }
    
    // Strings
    if (ch == '"' || ch == '\'') {
        return scanString(ch);
    }
    
    // Identifiers and keywords
    if (std::isalpha(ch) || ch == '_' || ch == '$') {
        return scanIdentifier();
    }
    
    // Operators
    if (ch == '+' || ch == '-' || ch == '*' || ch == '/' || ch == '%' || 
        ch == '=' || ch == '!' || ch == '<' || ch == '>') {
        return scanOperator();
    }
    
    // Single character tokens
    advance();
    switch (ch) {
        case '(': return Token(TokenType::LPAREN, "(", startLine, startColumn, startPos);
        case ')': return Token(TokenType::RPAREN, ")", startLine, startColumn, startPos);
        case '{': return Token(TokenType::LBRACE, "{", startLine, startColumn, startPos);
        case '}': return Token(TokenType::RBRACE, "}", startLine, startColumn, startPos);
        case '[': return Token(TokenType::LBRACKET, "[", startLine, startColumn, startPos);
        case ']': return Token(TokenType::RBRACKET, "]", startLine, startColumn, startPos);
        case ';': return Token(TokenType::SEMICOLON, ";", startLine, startColumn, startPos);
        case ',': return Token(TokenType::COMMA, ",", startLine, startColumn, startPos);
        case '.': return Token(TokenType::DOT, ".", startLine, startColumn, startPos);
        case '\n': return Token(TokenType::NEWLINE, "\n", startLine, startColumn, startPos);
        default:
            return Token(TokenType::INVALID, std::string(1, ch), startLine, startColumn, startPos);
    }
}

bool Lexer::hasMoreTokens() const {
    return position < source.length();
}

} // namespace Quanta
