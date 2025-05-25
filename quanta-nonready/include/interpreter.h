//<---------QUANTA JS ENGINE - INTERPRETER HEADER--------->
// Stage 2: Interpreter Engine - AST Walker & Execution
// Purpose: Execute JavaScript code by walking the AST
// Max Lines: 5000 (Current: ~150)

#ifndef QUANTA_INTERPRETER_H
#define QUANTA_INTERPRETER_H

#include "ast.h"
#include "env.h"
#include "error.h"
#include "runtime_objects.h"
#include <memory>
#include <vector>

namespace Quanta {

//<---------EXECUTION RESULT--------->
struct ExecutionResult {
    JSValue value;
    bool isReturn;
    bool isBreak;
    bool isContinue;
    
    ExecutionResult(const JSValue& val = nullptr) 
        : value(val), isReturn(false), isBreak(false), isContinue(false) {}
    
    ExecutionResult(const JSValue& val, bool ret) 
        : value(val), isReturn(ret), isBreak(false), isContinue(false) {}
};

//<---------INTERPRETER CLASS--------->
class Interpreter {
private:
    ScopeManager& scopeManager;
    ErrorHandler& errorHandler;
    
    // Built-in objects
    std::shared_ptr<ConsoleObject> console;
    std::shared_ptr<MathObject> math;
    
    // Helper methods
    JSValue evaluateExpression(const ASTNode* node);
    ExecutionResult executeStatement(const ASTNode* node);
    
    // Expression evaluation
    JSValue evaluateBinaryExpression(const BinaryExpressionNode* node);
    JSValue evaluateUnaryExpression(const UnaryExpressionNode* node);
    JSValue evaluateAssignmentExpression(const AssignmentExpressionNode* node);
    JSValue evaluateIdentifier(const IdentifierNode* node);
    JSValue evaluateLiteral(const ASTNode* node);
    
    // Statement execution
    ExecutionResult executeVariableDeclaration(const VariableDeclarationNode* node);
    ExecutionResult executeExpressionStatement(const ExpressionStatementNode* node);
    ExecutionResult executeBlockStatement(const BlockStatementNode* node);
    
    // Type conversion utilities
    double toNumber(const JSValue& value);
    std::string toString(const JSValue& value);
    bool toBoolean(const JSValue& value);
    
    // Arithmetic operations
    JSValue performArithmetic(const JSValue& left, const std::string& op, const JSValue& right);
    JSValue performComparison(const JSValue& left, const std::string& op, const JSValue& right);
    
public:
    Interpreter(ScopeManager& scopeManager, ErrorHandler& errorHandler);
    
    // Main execution methods
    ExecutionResult interpret(const ProgramNode* program);
    JSValue evaluateExpression(const std::string& expression);
    
    // Built-in object access
    void setupBuiltins();
    ConsoleObject& getConsole() { return *console; }
    MathObject& getMath() { return *math; }
};

//<---------UTILITY FUNCTIONS--------->
std::string jsValueTypeToString(const JSValue& value);
void printJSValue(const JSValue& value);

} // namespace Quanta

#endif // QUANTA_INTERPRETER_H
