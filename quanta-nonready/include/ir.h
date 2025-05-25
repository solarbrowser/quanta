// Stage 3: JIT Compiler & Optimizer - Intermediate Representation
// Purpose: IR generation and optimization for JIT compilation
// Max Lines: 5000 (Current: ~300)

#ifndef QUANTA_IR_H
#define QUANTA_IR_H

#include "ast.h"
#include "env.h"
#include "error.h"
#include <vector>
#include <string>
#include <memory>
#include <unordered_map>

namespace Quanta {

//<---------IR INSTRUCTION TYPES--------->
enum class IROpcode {
    // Arithmetic operations
    ADD, SUB, MUL, DIV, MOD,
    
    // Comparison operations
    EQ, NE, LT, GT, LE, GE,
    
    // Logical operations
    AND, OR, NOT,
    
    // Memory operations
    LOAD_CONST, LOAD_VAR, STORE_VAR,
    LOAD_GLOBAL, STORE_GLOBAL,
    
    // Control flow
    JUMP, JUMP_IF_TRUE, JUMP_IF_FALSE,
    CALL, RETURN,
    
    // Object operations
    GET_PROPERTY, SET_PROPERTY,
    CREATE_OBJECT, CREATE_ARRAY,
    
    // Type operations
    TYPE_CHECK, TYPE_CONVERT,
    
    // Special operations
    NOP, HALT
};

//<---------IR OPERAND TYPES--------->
enum class IROperandType {
    IMMEDIATE,      // Immediate value (number, string, boolean)
    REGISTER,       // Virtual register
    VARIABLE,       // Variable name
    LABEL,          // Jump label
    FUNCTION        // Function reference
};

//<---------IR OPERAND--------->
struct IROperand {
    IROperandType type;
    union {
        double number;
        bool boolean;
        int reg;
        int label;
    } data;
    std::string stringValue; // For strings and variable names
    
    // Constructors
    IROperand(double num) : type(IROperandType::IMMEDIATE) { data.number = num; }
    IROperand(bool val) : type(IROperandType::IMMEDIATE) { data.boolean = val; }
    IROperand(const std::string& str) : type(IROperandType::IMMEDIATE), stringValue(str) {}
    
    static IROperand Register(int reg) {
        IROperand op(0.0);
        op.type = IROperandType::REGISTER;
        op.data.reg = reg;
        return op;
    }
    
    static IROperand Variable(const std::string& name) {
        IROperand op(name);
        op.type = IROperandType::VARIABLE;
        return op;
    }
    
    static IROperand Label(int label) {
        IROperand op(0.0);
        op.type = IROperandType::LABEL;
        op.data.label = label;
        return op;
    }
};

//<---------IR INSTRUCTION--------->
struct IRInstruction {
    IROpcode opcode;
    IROperand dest;       // Destination operand
    IROperand src1;       // Source operand 1
    IROperand src2;       // Source operand 2 (optional)
    size_t line = 0;      // Source line for debugging
    size_t column = 0;    // Source column for debugging
    
    IRInstruction(IROpcode op, IROperand d, IROperand s1, IROperand s2 = IROperand(0.0))
        : opcode(op), dest(d), src1(s1), src2(s2) {}
};

//<---------IR BASIC BLOCK--------->
class IRBasicBlock {
private:
    std::vector<IRInstruction> instructions;
    std::vector<IRBasicBlock*> successors;
    std::vector<IRBasicBlock*> predecessors;
    int blockId;
    bool isSealed = false;

public:
    IRBasicBlock(int id) : blockId(id) {}
    
    // Instruction management
    void addInstruction(const IRInstruction& inst);
    void addInstruction(IROpcode opcode, IROperand dest, IROperand src1, IROperand src2 = IROperand(0.0));
    const std::vector<IRInstruction>& getInstructions() const { return instructions; }
    
    // Control flow
    void addSuccessor(IRBasicBlock* block);
    void addPredecessor(IRBasicBlock* block);
    const std::vector<IRBasicBlock*>& getSuccessors() const { return successors; }
    const std::vector<IRBasicBlock*>& getPredecessors() const { return predecessors; }
    
    // Block properties
    int getId() const { return blockId; }
    void seal() { isSealed = true; }
    bool getIsSealed() const { return isSealed; }
    size_t size() const { return instructions.size(); }
    bool empty() const { return instructions.empty(); }
    
    // Debugging
    std::string toString() const;
};

//<---------IR FUNCTION--------->
class IRFunction {
private:
    std::vector<std::unique_ptr<IRBasicBlock>> blocks;
    std::unordered_map<std::string, int> variableMap;
    int nextRegisterId = 0;
    int nextBlockId = 0;
    int nextLabelId = 0;
    std::string functionName;

public:
    IRFunction(const std::string& name) : functionName(name) {}
    
    // Block management
    IRBasicBlock* createBlock();
    IRBasicBlock* getBlock(int id);
    const std::vector<std::unique_ptr<IRBasicBlock>>& getBlocks() const { return blocks; }
    
    // Register allocation
    int allocateRegister();
    int allocateLabel();
    
    // Variable mapping
    void mapVariable(const std::string& name, int reg);
    int getVariableRegister(const std::string& name);
    bool hasVariable(const std::string& name) const;
    
    // Function properties
    const std::string& getName() const { return functionName; }
    size_t getBlockCount() const { return blocks.size(); }
    size_t getTotalInstructions() const;
    
    // Debugging and optimization
    std::string toString() const;
    void optimize();
    bool verify() const;
};

//<---------IR GENERATOR--------->
class IRGenerator {
private:
    std::unique_ptr<IRFunction> currentFunction;
    IRBasicBlock* currentBlock = nullptr;
    ErrorHandler& errorHandler;
    
    // Context tracking
    std::vector<int> breakLabels;
    std::vector<int> continueLabels;

public:
    IRGenerator(ErrorHandler& errorHandler);
    
    // Main generation interface
    std::unique_ptr<IRFunction> generateIR(const ASTNode* node, const std::string& functionName);
      // Statement generation
    void generateStatement(const ASTNode* node);
    void generateVariableDeclaration(const VariableDeclarationNode* node);
    void generateExpressionStatement(const ExpressionStatementNode* node);
    void generateBlockStatement(const BlockStatementNode* node);
    
    // Expression generation
    int generateExpression(const ASTNode* node);
    int generateBinaryExpression(const BinaryExpressionNode* node);
    int generateUnaryExpression(const UnaryExpressionNode* node);
    int generateAssignmentExpression(const AssignmentExpressionNode* node);
    int generateIdentifier(const IdentifierNode* node);
    int generateNumberLiteral(const NumberLiteralNode* node);
    int generateStringLiteral(const StringLiteralNode* node);
    
    // Utility methods
    void setCurrentBlock(IRBasicBlock* block) { currentBlock = block; }
    IRBasicBlock* getCurrentBlock() const { return currentBlock; }
    void emitInstruction(IROpcode opcode, IROperand dest, IROperand src1, IROperand src2 = IROperand(0.0));
};

//<---------IR OPTIMIZER--------->
class IROptimizer {
public:
    // Optimization passes
    static void constantFolding(IRFunction& function);
    static void deadCodeElimination(IRFunction& function);
    static void commonSubexpressionElimination(IRFunction& function);
    static void loopOptimization(IRFunction& function);
    static void inlineSmallFunctions(IRFunction& function);
    
    // Control flow optimization
    static void removeEmptyBlocks(IRFunction& function);
    static void mergeBlocks(IRFunction& function);
    static void eliminateUnreachableCode(IRFunction& function);
    
    // Full optimization pipeline
    static void optimizeFunction(IRFunction& function, int optimizationLevel = 2);
};

//<---------UTILITY FUNCTIONS--------->
std::string opcodeToString(IROpcode opcode);
std::string operandToString(const IROperand& operand);
std::string instructionToString(const IRInstruction& instruction);
IROpcode binaryOperatorToOpcode(const std::string& op);
IROpcode unaryOperatorToOpcode(const std::string& op);

} // namespace Quanta

#endif // QUANTA_IR_H
