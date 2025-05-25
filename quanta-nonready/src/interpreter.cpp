//<---------QUANTA JS ENGINE - INTERPRETER IMPLEMENTATION--------->
// Stage 2: Interpreter Engine - AST Walker & Execution
// Purpose: Execute JavaScript code by walking the AST
// Max Lines: 5000 (Current: ~500)

#include "../include/interpreter.h"
#include "../include/runtime_objects.h"
#include <iostream>
#include <sstream>
#include <cmath>

namespace Quanta {

//<---------INTERPRETER CONSTRUCTOR--------->
Interpreter::Interpreter(ScopeManager& scopeManager, ErrorHandler& errorHandler)
    : scopeManager(scopeManager), errorHandler(errorHandler) {
    setupBuiltins();
}

//<---------SETUP BUILT-IN OBJECTS--------->
void Interpreter::setupBuiltins() {
    console = std::make_shared<ConsoleObject>();
    math = std::make_shared<MathObject>();
    
    // Add built-ins to global scope
    scopeManager.defineVariable("console", JSValue(nullptr)); // Placeholder for now
    scopeManager.defineVariable("Math", JSValue(nullptr));    // Placeholder for now
}

//<---------MAIN INTERPRETATION METHOD--------->
ExecutionResult Interpreter::interpret(const ProgramNode* program) {
    ExecutionResult result;
    
    for (const auto& statement : program->statements) {
        try {
            result = executeStatement(statement.get());
            
            // Handle early returns, breaks, continues
            if (result.isReturn || result.isBreak || result.isContinue) {
                break;
            }
        } catch (const std::exception& e) {
            errorHandler.reportRuntimeError(e.what(), 0, 0);
            result.value = nullptr;
            break;
        }
    }
    
    return result;
}

//<---------STATEMENT EXECUTION--------->
ExecutionResult Interpreter::executeStatement(const ASTNode* node) {
    if (!node) {
        return ExecutionResult(nullptr);
    }
    
    switch (node->type) {
        case ASTNodeType::VARIABLE_DECLARATION:
            return executeVariableDeclaration(static_cast<const VariableDeclarationNode*>(node));
            
        case ASTNodeType::EXPRESSION_STATEMENT:
            return executeExpressionStatement(static_cast<const ExpressionStatementNode*>(node));
            
        case ASTNodeType::BLOCK_STATEMENT:
            return executeBlockStatement(static_cast<const BlockStatementNode*>(node));
            
        default:
            errorHandler.reportRuntimeError("Unknown statement type", node->line, node->column);
            return ExecutionResult(nullptr);
    }
}

//<---------VARIABLE DECLARATION EXECUTION--------->
ExecutionResult Interpreter::executeVariableDeclaration(const VariableDeclarationNode* node) {
    JSValue value = nullptr;
    
    if (node->initializer) {
        value = evaluateExpression(node->initializer.get());
    }
    
    bool isConst = (node->kind == "const");
    scopeManager.defineVariable(node->name, value, isConst);
    
    return ExecutionResult(value);
}

//<---------EXPRESSION STATEMENT EXECUTION--------->
ExecutionResult Interpreter::executeExpressionStatement(const ExpressionStatementNode* node) {
    JSValue value = evaluateExpression(node->expression.get());
    return ExecutionResult(value);
}

//<---------BLOCK STATEMENT EXECUTION--------->
ExecutionResult Interpreter::executeBlockStatement(const BlockStatementNode* node) {
    scopeManager.enterScope();
    
    ExecutionResult result;
    for (const auto& statement : node->statements) {
        result = executeStatement(statement.get());
        
        if (result.isReturn || result.isBreak || result.isContinue) {
            break;
        }
    }
    
    scopeManager.exitScope();
    return result;
}

//<---------EXPRESSION EVALUATION--------->
JSValue Interpreter::evaluateExpression(const ASTNode* node) {
    if (!node) {
        return nullptr;
    }
    
    switch (node->type) {
        case ASTNodeType::NUMBER_LITERAL:
            return static_cast<const NumberLiteralNode*>(node)->value;
            
        case ASTNodeType::STRING_LITERAL:
            return static_cast<const StringLiteralNode*>(node)->value;
            
        case ASTNodeType::BOOLEAN_LITERAL:
            // Note: We stored boolean as string in Stage 1, convert it
            return (static_cast<const StringLiteralNode*>(node)->value == "true");
            
        case ASTNodeType::NULL_LITERAL:
            return nullptr;
            
        case ASTNodeType::IDENTIFIER:
            return evaluateIdentifier(static_cast<const IdentifierNode*>(node));
            
        case ASTNodeType::BINARY_EXPRESSION:
            return evaluateBinaryExpression(static_cast<const BinaryExpressionNode*>(node));
            
        case ASTNodeType::UNARY_EXPRESSION:
            return evaluateUnaryExpression(static_cast<const UnaryExpressionNode*>(node));
            
        case ASTNodeType::ASSIGNMENT_EXPRESSION:
            return evaluateAssignmentExpression(static_cast<const AssignmentExpressionNode*>(node));
            
        default:
            errorHandler.reportRuntimeError("Unknown expression type", node->line, node->column);
            return nullptr;
    }
}

//<---------BINARY EXPRESSION EVALUATION--------->
JSValue Interpreter::evaluateBinaryExpression(const BinaryExpressionNode* node) {
    JSValue left = evaluateExpression(node->left.get());
    JSValue right = evaluateExpression(node->right.get());
    
    const std::string& op = node->operator_;
    
    // Arithmetic operations
    if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%") {
        return performArithmetic(left, op, right);
    }
    
    // Comparison operations
    if (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=") {
        return performComparison(left, op, right);
    }
    
    errorHandler.reportRuntimeError("Unknown binary operator: " + op, node->line, node->column);
    return nullptr;
}

//<---------UNARY EXPRESSION EVALUATION--------->
JSValue Interpreter::evaluateUnaryExpression(const UnaryExpressionNode* node) {
    JSValue operand = evaluateExpression(node->operand.get());
    const std::string& op = node->operator_;
    
    if (op == "-") {
        return -toNumber(operand);
    } else if (op == "+") {
        return toNumber(operand);
    } else if (op == "!") {
        return !toBoolean(operand);
    }
    
    errorHandler.reportRuntimeError("Unknown unary operator: " + op, node->line, node->column);
    return nullptr;
}

//<---------ASSIGNMENT EXPRESSION EVALUATION--------->
JSValue Interpreter::evaluateAssignmentExpression(const AssignmentExpressionNode* node) {
    // For now, only handle simple identifier assignments
    if (node->left->type != ASTNodeType::IDENTIFIER) {
        errorHandler.reportRuntimeError("Invalid assignment target", node->line, node->column);
        return nullptr;
    }
    
    const IdentifierNode* id = static_cast<const IdentifierNode*>(node->left.get());
    JSValue value = evaluateExpression(node->right.get());
    
    try {
        scopeManager.assignVariable(id->name, value);
        return value;
    } catch (const std::exception& e) {
        errorHandler.reportRuntimeError(e.what(), node->line, node->column);
        return nullptr;
    }
}

//<---------IDENTIFIER EVALUATION--------->
JSValue Interpreter::evaluateIdentifier(const IdentifierNode* node) {
    try {
        return scopeManager.getVariable(node->name);
    } catch (const std::exception& e) {
        errorHandler.reportReferenceError("Undefined variable: " + node->name, node->line, node->column);
        return nullptr;
    }
}

//<---------ARITHMETIC OPERATIONS--------->
JSValue Interpreter::performArithmetic(const JSValue& left, const std::string& op, const JSValue& right) {
    // Handle string concatenation for +
    if (op == "+") {
        // If either operand is a string, perform string concatenation
        if (std::holds_alternative<std::string>(left) || std::holds_alternative<std::string>(right)) {
            return toString(left) + toString(right);
        }
    }
    
    // Convert to numbers for arithmetic
    double leftNum = toNumber(left);
    double rightNum = toNumber(right);
    
    if (op == "+") return leftNum + rightNum;
    if (op == "-") return leftNum - rightNum;
    if (op == "*") return leftNum * rightNum;
    if (op == "/") {
        if (rightNum == 0) {
            return std::numeric_limits<double>::infinity();
        }
        return leftNum / rightNum;
    }
    if (op == "%") return std::fmod(leftNum, rightNum);
    
    return nullptr;
}

//<---------COMPARISON OPERATIONS--------->
JSValue Interpreter::performComparison(const JSValue& left, const std::string& op, const JSValue& right) {
    // For equality, handle different types
    if (op == "==") {
        // Simple equality (type coercion)
        if (left.index() == right.index()) {
            return left == right;
        }
        // Type coercion for numbers and strings
        return toNumber(left) == toNumber(right);
    }
    
    if (op == "!=") {
        return !std::get<bool>(performComparison(left, "==", right));
    }
    
    // For relational operators, convert to numbers
    double leftNum = toNumber(left);
    double rightNum = toNumber(right);
    
    if (op == "<") return leftNum < rightNum;
    if (op == ">") return leftNum > rightNum;
    if (op == "<=") return leftNum <= rightNum;
    if (op == ">=") return leftNum >= rightNum;
    
    return false;
}

//<---------TYPE CONVERSION UTILITIES--------->
double Interpreter::toNumber(const JSValue& value) {
    if (std::holds_alternative<double>(value)) {
        return std::get<double>(value);
    } else if (std::holds_alternative<bool>(value)) {
        return std::get<bool>(value) ? 1.0 : 0.0;
    } else if (std::holds_alternative<std::string>(value)) {
        try {
            return std::stod(std::get<std::string>(value));
        } catch (...) {
            return std::numeric_limits<double>::quiet_NaN();
        }
    }
    return 0.0; // null/undefined
}

std::string Interpreter::toString(const JSValue& value) {
    if (std::holds_alternative<std::string>(value)) {
        return std::get<std::string>(value);
    } else if (std::holds_alternative<double>(value)) {
        double num = std::get<double>(value);
        if (std::floor(num) == num) {
            return std::to_string(static_cast<long long>(num));
        }
        return std::to_string(num);
    } else if (std::holds_alternative<bool>(value)) {
        return std::get<bool>(value) ? "true" : "false";
    } else if (std::holds_alternative<std::nullptr_t>(value)) {
        return "null";
    }
    return "undefined";
}

bool Interpreter::toBoolean(const JSValue& value) {
    if (std::holds_alternative<bool>(value)) {
        return std::get<bool>(value);
    } else if (std::holds_alternative<double>(value)) {
        double num = std::get<double>(value);
        return num != 0.0 && !std::isnan(num);
    } else if (std::holds_alternative<std::string>(value)) {
        return !std::get<std::string>(value).empty();
    }
    return false; // null/undefined
}

//<---------EXPRESSION EVALUATION FROM STRING--------->
JSValue Interpreter::evaluateExpression(const std::string& expression) {
    // This is a simple wrapper - in a full implementation,
    // we'd need to tokenize and parse the expression
    // For now, return a placeholder
    return std::string("Expression: " + expression);
}

//<---------UTILITY FUNCTIONS--------->
std::string jsValueTypeToString(const JSValue& value) {
    if (std::holds_alternative<double>(value)) {
        return "number";
    } else if (std::holds_alternative<std::string>(value)) {
        return "string";
    } else if (std::holds_alternative<bool>(value)) {
        return "boolean";
    } else if (std::holds_alternative<std::nullptr_t>(value)) {
        return "null";
    }
    return "undefined";
}

void printJSValue(const JSValue& value) {
    if (std::holds_alternative<double>(value)) {
        double num = std::get<double>(value);
        if (std::floor(num) == num) {
            std::cout << static_cast<long long>(num);
        } else {
            std::cout << num;
        }
    } else if (std::holds_alternative<std::string>(value)) {
        std::cout << std::get<std::string>(value);
    } else if (std::holds_alternative<bool>(value)) {
        std::cout << (std::get<bool>(value) ? "true" : "false");
    } else if (std::holds_alternative<std::nullptr_t>(value)) {
        std::cout << "null";
    } else {
        std::cout << "undefined";
    }
}

} // namespace Quanta
