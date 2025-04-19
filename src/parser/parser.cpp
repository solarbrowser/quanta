#include "parser.h"
#include <sstream>

namespace quanta {

// ParserError implementation
ParserError::ParserError(const std::string& message, int line, int column)
    : std::runtime_error(message), line_(line), column_(column) {
}

int ParserError::get_line() const {
    return line_;
}

int ParserError::get_column() const {
    return column_;
}

// Parser implementation
Parser::Parser(const std::vector<Token>& tokens)
    : tokens_(tokens) {
}

std::shared_ptr<Program> Parser::parse() {
    auto program = std::make_shared<Program>();
    
    try {
        while (!is_at_end()) {
            auto statement = declaration();
            if (statement) {  // Make sure we only add non-null statements
                program->body.push_back(statement);
            }
        }
    } catch (const ParserError& error) {
        // Try to synchronize and continue parsing
        synchronize();
    }
    
    return program;
}

// Statement parsing methods

std::shared_ptr<Statement> Parser::declaration() {
    try {
        if (match({TokenType::VAR, TokenType::LET, TokenType::CONST})) {
            return variable_declaration();
        }
        
        if (match({TokenType::FUNCTION})) {
            return function_declaration();
        }
        
        return statement();
    } catch (const ParserError& error) {
        synchronize();
        return nullptr;
    }
}

std::shared_ptr<VariableDeclaration> Parser::variable_declaration() {
    // The VAR, LET, or CONST token has already been consumed
    TokenType kind_token = previous().get_type();
    Token kind_token_obj = previous();
    
    VariableDeclaration::Kind kind;
    if (kind_token == TokenType::VAR) {
        kind = VariableDeclaration::Kind::Var;
    } else if (kind_token == TokenType::LET) {
        kind = VariableDeclaration::Kind::Let;
    } else {
        kind = VariableDeclaration::Kind::Const;
    }
    
    auto declaration = std::make_shared<VariableDeclaration>(kind);
    declaration->line = kind_token_obj.get_line();
    declaration->column = kind_token_obj.get_column();
    
    do {
        // Variable name
        Token name = consume(TokenType::IDENTIFIER, "Expected variable name.");
        
        // Initializer (optional for var and let, required for const)
        std::shared_ptr<Expression> initializer = nullptr;
        
        if (match({TokenType::EQUAL})) {
            initializer = expression();
        } else if (kind == VariableDeclaration::Kind::Const) {
            throw error("Const variables must be initialized.");
        }
        
        auto id = std::make_shared<Identifier>(name.get_lexeme());
        id->line = name.get_line();
        id->column = name.get_column();
        
        auto declarator = std::make_shared<VariableDeclarator>(id, initializer);
        declarator->line = name.get_line();
        declarator->column = name.get_column();
        
        declaration->declarations.push_back(declarator);
    } while (match({TokenType::COMMA}));
    
    consume(TokenType::SEMICOLON, "Expected ';' after variable declaration.");
    
    return declaration;
}

std::shared_ptr<FunctionDeclaration> Parser::function_declaration() {
    // The FUNCTION token has already been consumed
    Token name = consume(TokenType::IDENTIFIER, "Expected function name.");
    
    auto id = std::make_shared<Identifier>(name.get_lexeme());
    id->line = name.get_line();
    id->column = name.get_column();
    
    consume(TokenType::LEFT_PAREN, "Expected '(' after function name.");
    std::vector<std::shared_ptr<Identifier>> params = parse_formal_parameters();
    consume(TokenType::RIGHT_PAREN, "Expected ')' after function parameters.");
    
    consume(TokenType::LEFT_BRACE, "Expected '{' before function body.");
    std::shared_ptr<BlockStatement> body = block_statement();
    
    auto func = std::make_shared<FunctionDeclaration>(id, params, body);
    func->line = name.get_line();
    func->column = name.get_column();
    
    return func;
}

std::vector<std::shared_ptr<Identifier>> Parser::parse_formal_parameters() {
    std::vector<std::shared_ptr<Identifier>> params;
    
    if (!check(TokenType::RIGHT_PAREN)) {
        do {
            Token param = consume(TokenType::IDENTIFIER, "Expected parameter name.");
            
            auto id = std::make_shared<Identifier>(param.get_lexeme());
            id->line = param.get_line();
            id->column = param.get_column();
            
            params.push_back(id);
        } while (match({TokenType::COMMA}));
    }
    
    return params;
}

std::shared_ptr<Statement> Parser::statement() {
    if (match({TokenType::IF})) {
        return if_statement();
    }
    
    if (match({TokenType::WHILE})) {
        return while_statement();
    }
    
    if (match({TokenType::FOR})) {
        return for_statement();
    }
    
    if (match({TokenType::RETURN})) {
        return return_statement();
    }
    
    if (match({TokenType::LEFT_BRACE})) {
        return block_statement();
    }
    
    return expression_statement();
}

std::shared_ptr<Statement> Parser::if_statement() {
    // The IF token has already been consumed
    consume(TokenType::LEFT_PAREN, "Expected '(' after 'if'.");
    std::shared_ptr<Expression> condition = expression();
    consume(TokenType::RIGHT_PAREN, "Expected ')' after if condition.");
    
    std::shared_ptr<Statement> then_branch = statement();
    std::shared_ptr<Statement> else_branch = nullptr;
    
    if (match({TokenType::ELSE})) {
        else_branch = statement();
    }
    
    auto if_stmt = std::make_shared<IfStatement>(condition, then_branch, else_branch);
    if_stmt->line = condition->line;
    if_stmt->column = condition->column;
    
    return if_stmt;
}

std::shared_ptr<Statement> Parser::for_statement() {
    // The FOR token has already been consumed
    consume(TokenType::LEFT_PAREN, "Expected '(' after 'for'.");
    
    // Initializer
    std::variant<std::monostate, std::shared_ptr<VariableDeclaration>, std::shared_ptr<Expression>> initializer;
    
    if (match({TokenType::SEMICOLON})) {
        // No initializer
        initializer = std::monostate{};
    } else if (match({TokenType::VAR, TokenType::LET, TokenType::CONST})) {
        initializer = variable_declaration();
    } else {
        auto expr = std::make_shared<ExpressionStatement>(expression());
        consume(TokenType::SEMICOLON, "Expected ';' after loop initializer.");
        initializer = expr->expression;
    }
    
    // Condition
    std::shared_ptr<Expression> condition = nullptr;
    if (!check(TokenType::SEMICOLON)) {
        condition = expression();
    }
    consume(TokenType::SEMICOLON, "Expected ';' after loop condition.");
    
    // Increment
    std::shared_ptr<Expression> increment = nullptr;
    if (!check(TokenType::RIGHT_PAREN)) {
        increment = expression();
    }
    consume(TokenType::RIGHT_PAREN, "Expected ')' after for clauses.");
    
    // Body
    std::shared_ptr<Statement> body = statement();
    
    auto for_stmt = std::make_shared<ForStatement>(initializer, condition, increment, body);
    
    // Set line and column from the start of the for statement
    Token for_token = tokens_[current_ - 1]; // FOR token
    for_stmt->line = for_token.get_line();
    for_stmt->column = for_token.get_column();
    
    return for_stmt;
}

std::shared_ptr<Statement> Parser::while_statement() {
    // The WHILE token has already been consumed
    consume(TokenType::LEFT_PAREN, "Expected '(' after 'while'.");
    std::shared_ptr<Expression> condition = expression();
    consume(TokenType::RIGHT_PAREN, "Expected ')' after while condition.");
    
    std::shared_ptr<Statement> body = statement();
    
    auto while_stmt = std::make_shared<WhileStatement>(condition, body);
    while_stmt->line = condition->line;
    while_stmt->column = condition->column;
    
    return while_stmt;
}

std::shared_ptr<Statement> Parser::return_statement() {
    // The RETURN token has already been consumed
    Token return_token = previous();
    
    std::shared_ptr<Expression> value = nullptr;
    if (!check(TokenType::SEMICOLON)) {
        value = expression();
    }
    
    consume(TokenType::SEMICOLON, "Expected ';' after return value.");
    
    auto ret_stmt = std::make_shared<ReturnStatement>(value);
    ret_stmt->line = return_token.get_line();
    ret_stmt->column = return_token.get_column();
    
    return ret_stmt;
}

std::shared_ptr<BlockStatement> Parser::block_statement() {
    // The LEFT_BRACE token has already been consumed
    auto block = std::make_shared<BlockStatement>();
    
    while (!check(TokenType::RIGHT_BRACE) && !is_at_end()) {
        block->body.push_back(declaration());
    }
    
    consume(TokenType::RIGHT_BRACE, "Expected '}' after block.");
    
    return block;
}

std::shared_ptr<Statement> Parser::expression_statement() {
    std::shared_ptr<Expression> expr = expression();
    consume(TokenType::SEMICOLON, "Expected ';' after expression.");
    
    auto stmt = std::make_shared<ExpressionStatement>(expr);
    stmt->line = expr->line;
    stmt->column = expr->column;
    
    return stmt;
}

// Expression parsing methods

std::shared_ptr<Expression> Parser::expression() {
    return assignment();
}

std::shared_ptr<Expression> Parser::assignment() {
    std::shared_ptr<Expression> expr = logical_or();
    
    if (match({TokenType::EQUAL, TokenType::PLUS_EQUAL, TokenType::MINUS_EQUAL, 
               TokenType::STAR_EQUAL, TokenType::SLASH_EQUAL, TokenType::PERCENT_EQUAL})) {
        Token op = previous();
        std::shared_ptr<Expression> right = assignment();
        
        // Check if the left side is a valid assignment target
        if (dynamic_cast<Identifier*>(expr.get()) || dynamic_cast<MemberExpression*>(expr.get())) {
            auto assign = std::make_shared<AssignmentExpression>(op.get_lexeme(), expr, right);
            assign->line = op.get_line();
            assign->column = op.get_column();
            return assign;
        }
        
        throw error("Invalid assignment target.");
    }
    
    return expr;
}

std::shared_ptr<Expression> Parser::logical_or() {
    std::shared_ptr<Expression> expr = logical_and();
    
    while (match({TokenType::OR})) {
        Token op = previous();
        std::shared_ptr<Expression> right = logical_and();
        
        auto logical = std::make_shared<LogicalExpression>(op.get_lexeme(), expr, right);
        logical->line = op.get_line();
        logical->column = op.get_column();
        expr = logical;
    }
    
    return expr;
}

std::shared_ptr<Expression> Parser::logical_and() {
    std::shared_ptr<Expression> expr = equality();
    
    while (match({TokenType::AND})) {
        Token op = previous();
        std::shared_ptr<Expression> right = equality();
        
        auto logical = std::make_shared<LogicalExpression>(op.get_lexeme(), expr, right);
        logical->line = op.get_line();
        logical->column = op.get_column();
        expr = logical;
    }
    
    return expr;
}

std::shared_ptr<Expression> Parser::equality() {
    std::shared_ptr<Expression> expr = comparison();
    
    while (match({TokenType::BANG_EQUAL, TokenType::EQUAL_EQUAL})) {
        Token op = previous();
        std::shared_ptr<Expression> right = comparison();
        
        auto binary = std::make_shared<BinaryExpression>(op.get_lexeme(), expr, right);
        binary->line = op.get_line();
        binary->column = op.get_column();
        expr = binary;
    }
    
    return expr;
}

std::shared_ptr<Expression> Parser::comparison() {
    std::shared_ptr<Expression> expr = addition();
    
    while (match({TokenType::GREATER, TokenType::GREATER_EQUAL, TokenType::LESS, TokenType::LESS_EQUAL})) {
        Token op = previous();
        std::shared_ptr<Expression> right = addition();
        
        auto binary = std::make_shared<BinaryExpression>(op.get_lexeme(), expr, right);
        binary->line = op.get_line();
        binary->column = op.get_column();
        expr = binary;
    }
    
    return expr;
}

std::shared_ptr<Expression> Parser::addition() {
    std::shared_ptr<Expression> expr = multiplication();
    
    while (match({TokenType::PLUS, TokenType::MINUS})) {
        Token op = previous();
        std::shared_ptr<Expression> right = multiplication();
        
        auto binary = std::make_shared<BinaryExpression>(op.get_lexeme(), expr, right);
        binary->line = op.get_line();
        binary->column = op.get_column();
        expr = binary;
    }
    
    return expr;
}

std::shared_ptr<Expression> Parser::multiplication() {
    std::shared_ptr<Expression> expr = unary();
    
    while (match({TokenType::STAR, TokenType::SLASH, TokenType::PERCENT})) {
        Token op = previous();
        std::shared_ptr<Expression> right = unary();
        
        auto binary = std::make_shared<BinaryExpression>(op.get_lexeme(), expr, right);
        binary->line = op.get_line();
        binary->column = op.get_column();
        expr = binary;
    }
    
    return expr;
}

std::shared_ptr<Expression> Parser::unary() {
    if (match({TokenType::BANG, TokenType::MINUS})) {
        Token op = previous();
        std::shared_ptr<Expression> right = unary();
        
        auto unary_expr = std::make_shared<UnaryExpression>(op.get_lexeme(), right);
        unary_expr->line = op.get_line();
        unary_expr->column = op.get_column();
        return unary_expr;
    }
    
    return call();
}

std::shared_ptr<Expression> Parser::call() {
    std::shared_ptr<Expression> expr = primary();
    
    while (true) {
        if (match({TokenType::LEFT_PAREN})) {
            expr = finish_call(expr);
        } else if (match({TokenType::DOT})) {
            Token name = consume(TokenType::IDENTIFIER, "Expected property name after '.'.");
            
            auto property = std::make_shared<Identifier>(name.get_lexeme());
            property->line = name.get_line();
            property->column = name.get_column();
            
            auto member = std::make_shared<MemberExpression>(expr, property, false);
            member->line = name.get_line();
            member->column = name.get_column();
            expr = member;
        } else if (match({TokenType::LEFT_BRACKET})) {
            std::shared_ptr<Expression> property = expression();
            consume(TokenType::RIGHT_BRACKET, "Expected ']' after property access.");
            
            auto member = std::make_shared<MemberExpression>(expr, property, true);
            member->line = property->line;
            member->column = property->column;
            expr = member;
        } else {
            break;
        }
    }
    
    return expr;
}

std::shared_ptr<Expression> Parser::finish_call(std::shared_ptr<Expression> callee) {
    std::vector<std::shared_ptr<Expression>> arguments;
    
    if (!check(TokenType::RIGHT_PAREN)) {
        do {
            arguments.push_back(expression());
        } while (match({TokenType::COMMA}));
    }
    
    Token paren = consume(TokenType::RIGHT_PAREN, "Expected ')' after function arguments.");
    
    auto call_expr = std::make_shared<CallExpression>(callee);
    call_expr->arguments = std::move(arguments);
    call_expr->line = paren.get_line();
    call_expr->column = paren.get_column();
    
    return call_expr;
}

std::shared_ptr<Expression> Parser::primary() {
    if (match({TokenType::FALSE})) {
        Token token = previous();
        auto literal = std::make_shared<Literal>(false);
        literal->line = token.get_line();
        literal->column = token.get_column();
        return literal;
    }
    
    if (match({TokenType::TRUE})) {
        Token token = previous();
        auto literal = std::make_shared<Literal>(true);
        literal->line = token.get_line();
        literal->column = token.get_column();
        return literal;
    }
    
    if (match({TokenType::NULL_LITERAL})) {
        Token token = previous();
        auto literal = std::make_shared<Literal>(nullptr);
        literal->line = token.get_line();
        literal->column = token.get_column();
        return literal;
    }
    
    if (match({TokenType::NUMBER, TokenType::STRING})) {
        Token token = previous();
        auto literal = std::make_shared<Literal>(token.get_literal());
        literal->line = token.get_line();
        literal->column = token.get_column();
        return literal;
    }
    
    if (match({TokenType::THIS})) {
        Token token = previous();
        auto this_expr = std::make_shared<ThisExpression>();
        this_expr->line = token.get_line();
        this_expr->column = token.get_column();
        return this_expr;
    }
    
    if (match({TokenType::IDENTIFIER})) {
        Token token = previous();
        auto id = std::make_shared<Identifier>(token.get_lexeme());
        id->line = token.get_line();
        id->column = token.get_column();
        return id;
    }
    
    if (match({TokenType::LEFT_PAREN})) {
        std::shared_ptr<Expression> expr = expression();
        consume(TokenType::RIGHT_PAREN, "Expected ')' after expression.");
        return expr;
    }
    
    if (match({TokenType::FUNCTION})) {
        return function_expression();
    }
    
    if (match({TokenType::LEFT_BRACE})) {
        // Object literal
        auto obj_expr = std::make_shared<ObjectExpression>();
        
        if (!check(TokenType::RIGHT_BRACE)) {
            do {
                // Property key
                std::shared_ptr<Expression> key;
                if (match({TokenType::IDENTIFIER})) {
                    Token name = previous();
                    key = std::make_shared<Identifier>(name.get_lexeme());
                    key->line = name.get_line();
                    key->column = name.get_column();
                } else if (match({TokenType::STRING, TokenType::NUMBER})) {
                    Token name = previous();
                    key = std::make_shared<Literal>(name.get_literal());
                    key->line = name.get_line();
                    key->column = name.get_column();
                } else {
                    throw error("Expected property name.");
                }
                
                consume(TokenType::COLON, "Expected ':' after property name.");
                
                // Property value
                std::shared_ptr<Expression> value = expression();
                
                auto property = std::make_shared<Property>(key, value);
                property->line = key->line;
                property->column = key->column;
                
                obj_expr->properties.push_back(property);
            } while (match({TokenType::COMMA}) && !check(TokenType::RIGHT_BRACE));
        }
        
        consume(TokenType::RIGHT_BRACE, "Expected '}' after object literal.");
        
        return obj_expr;
    }
    
    if (match({TokenType::LEFT_BRACKET})) {
        // Array literal
        auto array_expr = std::make_shared<ArrayExpression>();
        Token bracket = previous();
        array_expr->line = bracket.get_line();
        array_expr->column = bracket.get_column();
        
        if (!check(TokenType::RIGHT_BRACKET)) {
            do {
                if (check(TokenType::COMMA)) {
                    // Handle trailing comma or holes in array
                    array_expr->elements.push_back(nullptr);
                } else {
                    array_expr->elements.push_back(expression());
                }
            } while (match({TokenType::COMMA}) && !check(TokenType::RIGHT_BRACKET));
        }
        
        consume(TokenType::RIGHT_BRACKET, "Expected ']' after array literal.");
        
        return array_expr;
    }
    
    throw error("Expected expression.");
}

std::shared_ptr<FunctionExpression> Parser::function_expression() {
    // The FUNCTION token has already been consumed
    
    // Optional function name for named function expressions
    std::shared_ptr<Identifier> name = nullptr;
    if (check(TokenType::IDENTIFIER)) {
        Token id = advance();
        name = std::make_shared<Identifier>(id.get_lexeme());
        name->line = id.get_line();
        name->column = id.get_column();
    }
    
    consume(TokenType::LEFT_PAREN, "Expected '(' after function keyword.");
    std::vector<std::shared_ptr<Identifier>> params = parse_formal_parameters();
    consume(TokenType::RIGHT_PAREN, "Expected ')' after function parameters.");
    
    consume(TokenType::LEFT_BRACE, "Expected '{' before function body.");
    std::shared_ptr<BlockStatement> body = block_statement();
    
    auto func = std::make_shared<FunctionExpression>(name, params, body);
    func->line = body->line;
    func->column = body->column;
    
    return func;
}

// Helper methods

bool Parser::match(const std::vector<TokenType>& types) {
    for (TokenType type : types) {
        if (check(type)) {
            advance();
            return true;
        }
    }
    
    return false;
}

bool Parser::check(TokenType type) const {
    if (is_at_end()) return false;
    return peek().get_type() == type;
}

Token Parser::advance() {
    if (!is_at_end()) current_++;
    return previous();
}

bool Parser::is_at_end() const {
    return peek().get_type() == TokenType::END_OF_FILE;
}

Token Parser::peek() const {
    return tokens_[current_];
}

Token Parser::previous() const {
    return tokens_[current_ - 1];
}

Token Parser::consume(TokenType type, const std::string& message) {
    if (check(type)) return advance();
    
    throw error(peek(), message);
}

// Error handling

void Parser::synchronize() {
    advance();
    
    while (!is_at_end()) {
        if (previous().get_type() == TokenType::SEMICOLON) return;
        
        switch (peek().get_type()) {
            case TokenType::FUNCTION:
            case TokenType::VAR:
            case TokenType::LET:
            case TokenType::CONST:
            case TokenType::FOR:
            case TokenType::IF:
            case TokenType::WHILE:
            case TokenType::RETURN:
                return;
            default:
                break;
        }
        
        advance();
    }
}

ParserError Parser::error(const std::string& message) {
    return error(peek(), message);
}

ParserError Parser::error(const Token& token, const std::string& message) {
    std::stringstream ss;
    ss << "[line " << token.get_line() << ", column " << token.get_column() << "] ";
    ss << "Error";
    
    if (token.get_type() == TokenType::END_OF_FILE) {
        ss << " at end";
    } else {
        ss << " at '" << token.get_lexeme() << "'";
    }
    
    ss << ": " << message;
    
    return ParserError(ss.str(), token.get_line(), token.get_column());
}

} // namespace quanta 