//<---------QUANTA JS ENGINE - LEXER HEADER--------->
// Stage 1: Core Engine & Runtime - Tokenizer (Lexer)
// Purpose: Convert JavaScript source code into tokens
// Max Lines: 5000 (Current: ~100)

#ifndef QUANTA_LEXER_H
#define QUANTA_LEXER_H

#include <string>
#include <vector>
#include "hash_workaround.h"

namespace Quanta {

//<---------TOKEN TYPES--------->
enum class TokenType {
    // Literals
    NUMBER,
    STRING,
    BOOLEAN,
    NULL_LITERAL,
    UNDEFINED,
    
    // Identifiers and Keywords
    IDENTIFIER,
    LET,
    CONST,
    VAR,
    FUNCTION,
    RETURN,
    IF,
    ELSE,
    FOR,
    WHILE,
    CLASS,
    
    // Operators
    PLUS,
    MINUS,
    MULTIPLY,
    DIVIDE,
    MODULO,
    ASSIGN,
    EQUALS,
    NOT_EQUALS,
    LESS_THAN,
    GREATER_THAN,
    
    // Punctuation
    SEMICOLON,
    COMMA,
    DOT,
    LPAREN,
    RPAREN,
    LBRACE,
    RBRACE,
    LBRACKET,
    RBRACKET,
    
    // Special
    NEWLINE,
    WHITESPACE,
    EOF_TOKEN,
    INVALID
};

//<---------TOKEN STRUCTURE--------->
struct Token {
    TokenType type;
    std::string value;
    size_t line;
    size_t column;
    size_t position;
    
    Token(TokenType t, const std::string& v, size_t l, size_t c, size_t p)
        : type(t), value(v), line(l), column(c), position(p) {}
};

//<---------LEXER CLASS--------->
class Lexer {
private:    std::string source;
    size_t position;
    size_t line;
    size_t column;
    SimpleMap<std::string, TokenType> keywords;
    
    void initKeywords();
    char currentChar();
    char peekChar(size_t offset = 1);
    void advance();
    void skipWhitespace();
    Token scanNumber();
    Token scanString(char quote);
    Token scanIdentifier();
    Token scanOperator();
    
public:
    explicit Lexer(const std::string& source);
    std::vector<Token> tokenize();
    Token nextToken();
    bool hasMoreTokens() const;
};

} // namespace Quanta

#endif // QUANTA_LEXER_H
