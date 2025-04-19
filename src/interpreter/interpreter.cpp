#include "interpreter.h"
#include "lexer.h"
#include "parser.h"
#include <sstream>
#include <iostream>
#include <cmath>
#include <functional>

namespace quanta {

// JSFunction implementation
JSFunction::JSFunction(std::shared_ptr<FunctionDeclaration> declaration, 
                           std::shared_ptr<Environment> closure)
    : declaration_(declaration), closure_(closure) {}

JSFunction::JSFunction(std::shared_ptr<FunctionExpression> expression, 
                       std::shared_ptr<Environment> closure)
    : expression_(expression), closure_(closure) {}

std::vector<std::shared_ptr<Identifier>> JSFunction::getParameters() const {
    if (declaration_) {
        return declaration_->params;
    } else if (expression_) {
        return expression_->params;
    }
    return {};
}

std::shared_ptr<BlockStatement> JSFunction::getBody() const {
    if (declaration_) {
        return declaration_->body;
    } else if (expression_) {
        return expression_->body;
    }
    return nullptr;
}

std::shared_ptr<Environment> JSFunction::getClosure() const {
    return closure_;
}

// RuntimeError implementation
RuntimeError::RuntimeError(const std::string& message, int line, int column)
    : std::runtime_error(message), line_(line), column_(column) {
}

int RuntimeError::get_line() const {
    return line_;
}

int RuntimeError::get_column() const {
    return column_;
}

// Environment implementation
Environment::Environment(std::shared_ptr<Environment> enclosing)
    : enclosing_(std::move(enclosing)) {
}

void Environment::define(const std::string& name, const JSValue& value) {
    values_[name] = value;
}

JSValue Environment::get(const std::string& name) {
    auto it = values_.find(name);
    if (it != values_.end()) {
        return it->second;
    }

    if (enclosing_) {
        return enclosing_->get(name);
    }

    std::string error = "Undefined variable '" + name + "'.";
    throw std::runtime_error(error);
}

void Environment::assign(const std::string& name, const JSValue& value) {
    auto it = values_.find(name);
    if (it != values_.end()) {
        it->second = value;
        return;
    }

    if (enclosing_) {
        enclosing_->assign(name, value);
        return;
    }

    std::string error = "Undefined variable '" + name + "'.";
    throw std::runtime_error(error);
}

bool Environment::contains(const std::string& name) const {
    if (values_.find(name) != values_.end()) {
        return true;
    }

    if (enclosing_) {
        return enclosing_->contains(name);
    }

    return false;
}

// Interpreter implementation
Interpreter::Interpreter() 
    : environment_(std::make_shared<Environment>()) {
    // Initialize global scope
    // Add console object with log method
    auto console_env = std::make_shared<Environment>(environment_);
    environment_->define("console", console_env);
}

JSValue Interpreter::interpret(const std::shared_ptr<Program>& program) {
    try {
        visit(*program);
        return last_value_;
    } catch (const RuntimeError& error) {
        std::stringstream ss;
        ss << "[line " << error.get_line() << ", column " << error.get_column() << "] ";
        ss << "Runtime Error: " << error.what();
        std::cerr << ss.str() << std::endl;
        return std::monostate{};
    } catch (const std::exception& error) {
        std::cerr << "Runtime Error: " << error.what() << std::endl;
        return std::monostate{};
    }
}

void Interpreter::interpret_statement(const std::shared_ptr<Statement>& statement) {
    if (statement) {
        statement->accept(*this);
    }
}

JSValue Interpreter::evaluate(const std::string& source) {
    try {
        // Stage 1: Lexical analysis
        Lexer lexer(source);
        std::vector<Token> tokens = lexer.scan_tokens();
        
        // Stage 2: Parsing
        Parser parser(tokens);
        std::shared_ptr<Program> program = parser.parse();
        
        // Stage 3: Interpretation
        return interpret(program);
    } catch (const LexerError& error) {
        std::cerr << "Lexer Error: " << error.what() << std::endl;
        return std::monostate{};
    } catch (const ParserError& error) {
        std::cerr << "Parser Error: " << error.what() << std::endl;
        return std::monostate{};
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << std::endl;
        return std::monostate{};
    }
}

JSValue Interpreter::evaluate(const std::shared_ptr<Expression>& expression) {
    if (expression) {
        expression->accept(*this);
        return last_value_;
    }
    return std::monostate{};
}

// Statement visitor methods
void Interpreter::visit(Program& stmt) {
    for (const auto& statement : stmt.body) {
        interpret_statement(statement);
    }
}

void Interpreter::visit(BlockStatement& stmt) {
    execute_block(stmt.body, std::make_shared<Environment>(environment_));
}

void Interpreter::execute_block(const std::vector<std::shared_ptr<Statement>>& statements,
                              std::shared_ptr<Environment> environment) {
    std::shared_ptr<Environment> previous = environment_;
    try {
        environment_ = environment;

        for (const auto& statement : statements) {
            interpret_statement(statement);
        }
    } catch (...) {
        environment_ = previous;
        throw;
    }

    environment_ = previous;
}

void Interpreter::visit(ExpressionStatement& stmt) {
    last_value_ = evaluate(stmt.expression);
    // Ensure the result gets propagated up properly for REPL use
}

void Interpreter::visit(VariableDeclaration& stmt) {
    for (const auto& declarator : stmt.declarations) {
        JSValue value = std::monostate{}; // undefined
        if (declarator->init) {
            value = evaluate(declarator->init);
        }
        environment_->define(declarator->id->name, value);
    }
    last_value_ = std::monostate{};
}

void Interpreter::visit(FunctionDeclaration& stmt) {
    // Create a function object and store it in the environment
    auto function = std::make_shared<JSFunction>(std::make_shared<FunctionDeclaration>(stmt), environment_);
    environment_->define(stmt.id->name, function);
    last_value_ = std::monostate{};
}

void Interpreter::visit(ReturnStatement& stmt) {
    JSValue value = std::monostate{};
    if (stmt.argument) {
        value = evaluate(stmt.argument);
    }
    
    throw ReturnException(value);
}

void Interpreter::visit(IfStatement& stmt) {
    if (is_truthy(evaluate(stmt.test))) {
        interpret_statement(stmt.consequent);
    } else if (stmt.alternate) {
        interpret_statement(stmt.alternate);
    }
}

void Interpreter::visit(WhileStatement& stmt) {
    while (is_truthy(evaluate(stmt.test))) {
        interpret_statement(stmt.body);
    }
}

void Interpreter::visit(ForStatement& stmt) {
    // Initialize
    if (std::holds_alternative<std::shared_ptr<VariableDeclaration>>(stmt.init)) {
        interpret_statement(std::get<std::shared_ptr<VariableDeclaration>>(stmt.init));
    } else if (std::holds_alternative<std::shared_ptr<Expression>>(stmt.init)) {
        evaluate(std::get<std::shared_ptr<Expression>>(stmt.init));
    }

    // Loop condition and body
    while (true) {
        // Test condition
        if (stmt.test && !is_truthy(evaluate(stmt.test))) {
            break;
        }

        // Execute body
        interpret_statement(stmt.body);

        // Update
        if (stmt.update) {
            evaluate(stmt.update);
        }
    }
}

// Expression visitor methods
void Interpreter::visit(Identifier& expr) {
    last_value_ = environment_->get(expr.name);
}

void Interpreter::visit(Literal& expr) {
    // Handle conversion from LiteralValue to JSValue for each variant type
    if (std::holds_alternative<std::monostate>(expr.value)) {
        last_value_ = std::monostate{};
    } else if (std::holds_alternative<std::string>(expr.value)) {
        last_value_ = std::get<std::string>(expr.value);
    } else if (std::holds_alternative<double>(expr.value)) {
        last_value_ = std::get<double>(expr.value);
    } else if (std::holds_alternative<bool>(expr.value)) {
        last_value_ = std::get<bool>(expr.value);
    } else if (std::holds_alternative<std::nullptr_t>(expr.value)) {
        last_value_ = nullptr;
    }
}

void Interpreter::visit(BinaryExpression& expr) {
    JSValue left = evaluate(expr.left);
    JSValue right = evaluate(expr.right);

    // Handle different operators
    if (expr.operator_ == "+") {
        // Handle addition (numbers or string concatenation)
        if (std::holds_alternative<double>(left) && std::holds_alternative<double>(right)) {
            last_value_ = std::get<double>(left) + std::get<double>(right);
        } else {
            // String concatenation
            last_value_ = value_to_string(left) + value_to_string(right);
        }
    } else if (expr.operator_ == "-") {
        checkNumberOperands(expr.operator_, left, right);
        last_value_ = std::get<double>(left) - std::get<double>(right);
    } else if (expr.operator_ == "*") {
        checkNumberOperands(expr.operator_, left, right);
        last_value_ = std::get<double>(left) * std::get<double>(right);
    } else if (expr.operator_ == "/") {
        checkNumberOperands(expr.operator_, left, right);
        double rightVal = std::get<double>(right);
        if (rightVal == 0) {
            throw RuntimeError("Division by zero.", expr.line, expr.column);
        }
        last_value_ = std::get<double>(left) / rightVal;
    } else if (expr.operator_ == "%") {
        checkNumberOperands(expr.operator_, left, right);
        last_value_ = std::fmod(std::get<double>(left), std::get<double>(right));
    } else if (expr.operator_ == ">") {
        checkNumberOperands(expr.operator_, left, right);
        last_value_ = std::get<double>(left) > std::get<double>(right);
    } else if (expr.operator_ == ">=") {
        checkNumberOperands(expr.operator_, left, right);
        last_value_ = std::get<double>(left) >= std::get<double>(right);
    } else if (expr.operator_ == "<") {
        checkNumberOperands(expr.operator_, left, right);
        last_value_ = std::get<double>(left) < std::get<double>(right);
    } else if (expr.operator_ == "<=") {
        checkNumberOperands(expr.operator_, left, right);
        last_value_ = std::get<double>(left) <= std::get<double>(right);
    } else if (expr.operator_ == "==") {
        last_value_ = is_equal(left, right);
    } else if (expr.operator_ == "!=") {
        last_value_ = !is_equal(left, right);
    } else {
        throw RuntimeError("Unknown binary operator: " + expr.operator_, expr.line, expr.column);
    }
}

void Interpreter::visit(LogicalExpression& expr) {
    JSValue left = evaluate(expr.left);

    if (expr.operator_ == "&&") {
        if (!is_truthy(left)) {
            last_value_ = left;
            return;
        }
    } else if (expr.operator_ == "||") {
        if (is_truthy(left)) {
            last_value_ = left;
            return;
        }
    }

    last_value_ = evaluate(expr.right);
}

void Interpreter::visit(UnaryExpression& expr) {
    JSValue right = evaluate(expr.argument);

    if (expr.operator_ == "-") {
        if (!std::holds_alternative<double>(right)) {
            throw RuntimeError("Operand must be a number.", expr.line, expr.column);
        }
        last_value_ = -std::get<double>(right);
    } else if (expr.operator_ == "!") {
        last_value_ = !is_truthy(right);
    } else {
        throw RuntimeError("Unknown unary operator: " + expr.operator_, expr.line, expr.column);
    }
}

void Interpreter::visit(AssignmentExpression& expr) {
    JSValue value = evaluate(expr.right);

    if (auto* id = dynamic_cast<Identifier*>(expr.left.get())) {
        if (expr.operator_ == "=") {
            environment_->assign(id->name, value);
        } else {
            // Handle compound assignment (+=, -=, etc.)
            JSValue currentValue = environment_->get(id->name);
            
            if (expr.operator_ == "+=") {
                if (std::holds_alternative<double>(currentValue) && std::holds_alternative<double>(value)) {
                    value = std::get<double>(currentValue) + std::get<double>(value);
                } else {
                    value = value_to_string(currentValue) + value_to_string(value);
                }
            } else if (expr.operator_ == "-=") {
                checkNumberOperands("-=", currentValue, value);
                value = std::get<double>(currentValue) - std::get<double>(value);
            } else if (expr.operator_ == "*=") {
                checkNumberOperands("*=", currentValue, value);
                value = std::get<double>(currentValue) * std::get<double>(value);
            } else if (expr.operator_ == "/=") {
                checkNumberOperands("/=", currentValue, value);
                value = std::get<double>(currentValue) / std::get<double>(value);
            } else if (expr.operator_ == "%=") {
                checkNumberOperands("%=", currentValue, value);
                value = std::fmod(std::get<double>(currentValue), std::get<double>(value));
            }
            
            environment_->assign(id->name, value);
        }
    } else if (auto* member = dynamic_cast<MemberExpression*>(expr.left.get())) {
        // We'll implement object property assignment later
        throw RuntimeError("Object property assignment not yet implemented.", expr.line, expr.column);
    } else {
        throw RuntimeError("Invalid assignment target.", expr.line, expr.column);
    }

    last_value_ = value;
}

void Interpreter::visit(CallExpression& expr) {
    JSValue callee = evaluate(expr.callee);
    
    // Handle console.log as a special case
    if (auto* member = dynamic_cast<MemberExpression*>(expr.callee.get())) {
        if (!member->computed) {
            if (auto* obj = dynamic_cast<Identifier*>(member->object.get())) {
                if (obj->name == "console") {
                    if (auto* prop = dynamic_cast<Identifier*>(member->property.get())) {
                        if (prop->name == "log") {
                            // Implement console.log
                            for (size_t i = 0; i < expr.arguments.size(); i++) {
                                if (i > 0) std::cout << " ";
                                std::cout << value_to_string(evaluate(expr.arguments[i]));
                            }
                            std::cout << std::endl;
                            last_value_ = std::monostate{};
                            return;
                        }
                    }
                }
            }
        }
    }
    
    // Handle function calls
    if (std::holds_alternative<std::shared_ptr<JSFunction>>(callee)) {
        auto function = std::get<std::shared_ptr<JSFunction>>(callee);
        
        // Evaluate arguments
        std::vector<JSValue> arguments;
        for (const auto& arg : expr.arguments) {
            arguments.push_back(evaluate(arg));
        }
        
        // Create a new environment for the function call
        auto functionEnv = std::make_shared<Environment>(function->getClosure());
        
        // Bind parameters to arguments
        auto params = function->getParameters();
        for (size_t i = 0; i < params.size() && i < arguments.size(); i++) {
            functionEnv->define(params[i]->name, arguments[i]);
        }
        
        // Execute the function body
        try {
            execute_block(function->getBody()->body, functionEnv);
            // If no return statement is encountered, return undefined
            last_value_ = std::monostate{};
        } catch (const ReturnException& returnValue) {
            // Handle return statement
            last_value_ = returnValue.getValue();
        }
        
        return;
    }
    
    throw RuntimeError("Can only call functions.", expr.line, expr.column);
}

void Interpreter::visit(MemberExpression& expr) {
    if (!expr.computed) {
        // Handle dot property access (obj.prop)
        JSValue object = evaluate(expr.object);
        
        // Handle special case for console object
        if (auto* obj = dynamic_cast<Identifier*>(expr.object.get())) {
            if (obj->name == "console" && 
                dynamic_cast<Identifier*>(expr.property.get()) != nullptr) {
                auto* prop = dynamic_cast<Identifier*>(expr.property.get());
                
                // Only support 'log' for now
                if (prop->name == "log") {
                    last_value_ = environment_->get("console");
                    return;
                }
            }
        }
        
        // For general object property access
        if (std::holds_alternative<std::shared_ptr<Environment>>(object)) {
            auto env = std::get<std::shared_ptr<Environment>>(object);
            auto* prop = dynamic_cast<Identifier*>(expr.property.get());
            
            if (prop && env->contains(prop->name)) {
                last_value_ = env->get(prop->name);
                return;
            }
        }
    }
    
    // If we reach here, it's not a supported member expression
    throw RuntimeError("Member expressions not fully implemented yet.", expr.line, expr.column);
}

void Interpreter::visit(FunctionExpression& expr) {
    // Create a function value and return it
    auto function = std::make_shared<JSFunction>(std::make_shared<FunctionExpression>(expr), environment_);
    last_value_ = function;
}

void Interpreter::visit(ObjectExpression& expr) {
    // Create a new environment for the object
    auto objEnv = std::make_shared<Environment>(environment_);
    
    // Define each property in the object's environment
    for (const auto& prop : expr.properties) {
        JSValue value = std::monostate{};
        if (prop->kind == Property::Kind::Init) {
            value = evaluate(prop->value);
        }
        
        // Get the property key (only support identifiers for now)
        if (auto* keyId = dynamic_cast<Identifier*>(prop->key.get())) {
            objEnv->define(keyId->name, value);
        }
    }
    
    last_value_ = objEnv;
}

void Interpreter::visit(ArrayExpression& expr) {
    // We'll implement array literals later
    last_value_ = std::monostate{};
}

void Interpreter::visit(ThisExpression& expr) {
    // We'll implement 'this' expressions later
    last_value_ = std::monostate{};
}

// Helper methods
bool Interpreter::is_truthy(const JSValue& value) {
    if (std::holds_alternative<std::monostate>(value)) return false;
    if (std::holds_alternative<std::nullptr_t>(value)) return false;
    if (std::holds_alternative<bool>(value)) return std::get<bool>(value);
    if (std::holds_alternative<double>(value)) return std::get<double>(value) != 0;
    if (std::holds_alternative<std::string>(value)) return !std::get<std::string>(value).empty();
    return true;
}

bool Interpreter::is_equal(const JSValue& a, const JSValue& b) {
    // If types are different, values are not equal
    if (a.index() != b.index()) return false;
    
    // Compare based on type
    if (std::holds_alternative<std::monostate>(a)) return true;
    if (std::holds_alternative<std::nullptr_t>(a)) return true;
    if (std::holds_alternative<bool>(a)) return std::get<bool>(a) == std::get<bool>(b);
    if (std::holds_alternative<double>(a)) return std::get<double>(a) == std::get<double>(b);
    if (std::holds_alternative<std::string>(a)) return std::get<std::string>(a) == std::get<std::string>(b);
    if (std::holds_alternative<std::shared_ptr<Environment>>(a)) 
        return std::get<std::shared_ptr<Environment>>(a) == std::get<std::shared_ptr<Environment>>(b);
    
    return false;
}

void Interpreter::checkNumberOperands(const std::string& op, const JSValue& left, const JSValue& right) {
    if (std::holds_alternative<double>(left) && std::holds_alternative<double>(right)) return;
    throw RuntimeError("Operands must be numbers.", 0, 0);
}

std::string Interpreter::value_to_string(const JSValue& value) {
    if (std::holds_alternative<std::monostate>(value)) return "undefined";
    if (std::holds_alternative<std::nullptr_t>(value)) return "null";
    if (std::holds_alternative<bool>(value)) return std::get<bool>(value) ? "true" : "false";
    if (std::holds_alternative<double>(value)) {
        std::string result = std::to_string(std::get<double>(value));
        // Remove trailing zeros
        if (result.find('.') != std::string::npos) {
            result = result.substr(0, result.find_last_not_of('0') + 1);
            if (result.back() == '.') result.pop_back();
        }
        return result;
    }
    if (std::holds_alternative<std::string>(value)) return std::get<std::string>(value);
    if (std::holds_alternative<std::shared_ptr<Environment>>(value)) return "[object Object]";
    if (std::holds_alternative<std::shared_ptr<JSFunction>>(value)) return "[function]";
    return "[unknown]";
}

} // namespace quanta
 