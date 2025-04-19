#include "token_type.h"

namespace quanta {

std::string token_type_to_string(TokenType type) {
    switch (type) {
        // Single-character tokens
        case TokenType::LEFT_PAREN: return "LEFT_PAREN";
        case TokenType::RIGHT_PAREN: return "RIGHT_PAREN";
        case TokenType::LEFT_BRACE: return "LEFT_BRACE";
        case TokenType::RIGHT_BRACE: return "RIGHT_BRACE";
        case TokenType::LEFT_BRACKET: return "LEFT_BRACKET";
        case TokenType::RIGHT_BRACKET: return "RIGHT_BRACKET";
        case TokenType::COMMA: return "COMMA";
        case TokenType::DOT: return "DOT";
        case TokenType::SEMICOLON: return "SEMICOLON";
        case TokenType::COLON: return "COLON";
        
        // Operators
        case TokenType::PLUS: return "PLUS";
        case TokenType::MINUS: return "MINUS";
        case TokenType::STAR: return "STAR";
        case TokenType::SLASH: return "SLASH";
        case TokenType::PERCENT: return "PERCENT";
        
        // Comparison operators
        case TokenType::EQUAL: return "EQUAL";
        case TokenType::EQUAL_EQUAL: return "EQUAL_EQUAL";
        case TokenType::BANG: return "BANG";
        case TokenType::BANG_EQUAL: return "BANG_EQUAL";
        case TokenType::GREATER: return "GREATER";
        case TokenType::GREATER_EQUAL: return "GREATER_EQUAL";
        case TokenType::LESS: return "LESS";
        case TokenType::LESS_EQUAL: return "LESS_EQUAL";
        
        // Logical operators
        case TokenType::AND: return "AND";
        case TokenType::OR: return "OR";
        
        // Assignment operators
        case TokenType::PLUS_EQUAL: return "PLUS_EQUAL";
        case TokenType::MINUS_EQUAL: return "MINUS_EQUAL";
        case TokenType::STAR_EQUAL: return "STAR_EQUAL";
        case TokenType::SLASH_EQUAL: return "SLASH_EQUAL";
        case TokenType::PERCENT_EQUAL: return "PERCENT_EQUAL";
        
        // Literals
        case TokenType::IDENTIFIER: return "IDENTIFIER";
        case TokenType::STRING: return "STRING";
        case TokenType::NUMBER: return "NUMBER";
        case TokenType::BOOLEAN: return "BOOLEAN";
        case TokenType::NULL_LITERAL: return "NULL";
        case TokenType::UNDEFINED: return "UNDEFINED";
        
        // Keywords
        case TokenType::VAR: return "VAR";
        case TokenType::LET: return "LET";
        case TokenType::CONST: return "CONST";
        case TokenType::IF: return "IF";
        case TokenType::ELSE: return "ELSE";
        case TokenType::WHILE: return "WHILE";
        case TokenType::FOR: return "FOR";
        case TokenType::FUNCTION: return "FUNCTION";
        case TokenType::RETURN: return "RETURN";
        case TokenType::TRUE: return "TRUE";
        case TokenType::FALSE: return "FALSE";
        case TokenType::THIS: return "THIS";
        case TokenType::NEW: return "NEW";
        
        // End of file
        case TokenType::END_OF_FILE: return "END_OF_FILE";
    }
    
    return "UNKNOWN";
}

// Define the keywords map
const std::unordered_map<std::string, TokenType> keywords = {
    {"var", TokenType::VAR},
    {"let", TokenType::LET},
    {"const", TokenType::CONST},
    {"if", TokenType::IF},
    {"else", TokenType::ELSE},
    {"while", TokenType::WHILE},
    {"for", TokenType::FOR},
    {"function", TokenType::FUNCTION},
    {"return", TokenType::RETURN},
    {"true", TokenType::TRUE},
    {"false", TokenType::FALSE},
    {"this", TokenType::THIS},
    {"new", TokenType::NEW},
    {"null", TokenType::NULL_LITERAL},
    {"undefined", TokenType::UNDEFINED}
};

} // namespace quanta 