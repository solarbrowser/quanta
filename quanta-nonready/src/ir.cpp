// Stage 3: JIT Compiler & Optimizer - IR Implementation
// Purpose: Intermediate Representation generation and optimization
// Max Lines: 5000 (Current: ~500)

#include "../include/ir.h"
#include <iostream>
#include <sstream>
#include <algorithm>

namespace Quanta {

//<---------IR BASIC BLOCK IMPLEMENTATION--------->
void IRBasicBlock::addInstruction(const IRInstruction& inst) {
    instructions.push_back(inst);
}

void IRBasicBlock::addInstruction(IROpcode opcode, IROperand dest, IROperand src1, IROperand src2) {
    instructions.emplace_back(opcode, dest, src1, src2);
}

void IRBasicBlock::addSuccessor(IRBasicBlock* block) {
    if (std::find(successors.begin(), successors.end(), block) == successors.end()) {
        successors.push_back(block);
        block->addPredecessor(this);
    }
}

void IRBasicBlock::addPredecessor(IRBasicBlock* block) {
    if (std::find(predecessors.begin(), predecessors.end(), block) == predecessors.end()) {
        predecessors.push_back(block);
    }
}

std::string IRBasicBlock::toString() const {
    std::stringstream ss;
    ss << "Block " << blockId << ":\n";
    for (const auto& inst : instructions) {
        ss << "  " << instructionToString(inst) << "\n";
    }
    return ss.str();
}

//<---------IR FUNCTION IMPLEMENTATION--------->
IRBasicBlock* IRFunction::createBlock() {
    auto block = std::make_unique<IRBasicBlock>(nextBlockId++);
    IRBasicBlock* blockPtr = block.get();
    blocks.push_back(std::move(block));
    return blockPtr;
}

IRBasicBlock* IRFunction::getBlock(int id) {
    for (const auto& block : blocks) {
        if (block->getId() == id) {
            return block.get();
        }
    }
    return nullptr;
}

int IRFunction::allocateRegister() {
    return nextRegisterId++;
}

int IRFunction::allocateLabel() {
    return nextLabelId++;
}

void IRFunction::mapVariable(const std::string& name, int reg) {
    variableMap[name] = reg;
}

int IRFunction::getVariableRegister(const std::string& name) {
    auto it = variableMap.find(name);
    if (it != variableMap.end()) {
        return it->second;
    }
    return -1; // Variable not found
}

bool IRFunction::hasVariable(const std::string& name) const {
    return variableMap.find(name) != variableMap.end();
}

size_t IRFunction::getTotalInstructions() const {
    size_t total = 0;
    for (const auto& block : blocks) {
        total += block->size();
    }
    return total;
}

std::string IRFunction::toString() const {
    std::stringstream ss;
    ss << "Function " << functionName << ":\n";
    for (const auto& block : blocks) {
        ss << block->toString();
    }
    return ss.str();
}

bool IRFunction::verify() const {
    // Basic verification - check that all blocks are reachable
    // and no dangling references exist
    if (blocks.empty()) return false;
    
    // More verification logic would go here
    return true;
}

//<---------IR GENERATOR IMPLEMENTATION--------->
IRGenerator::IRGenerator(ErrorHandler& errorHandler) : errorHandler(errorHandler) {}

std::unique_ptr<IRFunction> IRGenerator::generateIR(const ASTNode* node, const std::string& functionName) {
    currentFunction = std::make_unique<IRFunction>(functionName);
    currentBlock = currentFunction->createBlock();
    
    try {
        generateStatement(node);
        
        // Add implicit return if needed
        if (currentBlock && (currentBlock->empty() || 
            currentBlock->getInstructions().back().opcode != IROpcode::RETURN)) {
            emitInstruction(IROpcode::RETURN, IROperand(0.0), IROperand(0.0));
        }
    } catch (const std::exception& e) {
        errorHandler.reportRuntimeError("IR generation failed: " + std::string(e.what()), 0, 0);
        return nullptr;
    }
    
    return std::move(currentFunction);
}

void IRGenerator::generateStatement(const ASTNode* node) {
    if (!node || !currentBlock) return;
    
    switch (node->type) {
        case ASTNodeType::PROGRAM: {
            const ProgramNode* program = static_cast<const ProgramNode*>(node);
            for (const auto& stmt : program->statements) {
                generateStatement(stmt.get());
            }
            break;
        }
        
        case ASTNodeType::VARIABLE_DECLARATION:
            generateVariableDeclaration(static_cast<const VariableDeclarationNode*>(node));
            break;
            
        case ASTNodeType::EXPRESSION_STATEMENT:
            generateExpressionStatement(static_cast<const ExpressionStatementNode*>(node));
            break;
            
        case ASTNodeType::BLOCK_STATEMENT:
            generateBlockStatement(static_cast<const BlockStatementNode*>(node));
            break;
            
        default:
            errorHandler.reportRuntimeError("Unsupported statement type in IR generation", node->line, node->column);
            break;
    }
}

void IRGenerator::generateVariableDeclaration(const VariableDeclarationNode* node) {
    int destReg = currentFunction->allocateRegister();
    currentFunction->mapVariable(node->name, destReg);
    
    if (node->initializer) {
        int valueReg = generateExpression(node->initializer.get());
        emitInstruction(IROpcode::STORE_VAR, IROperand::Register(destReg), IROperand::Register(valueReg));
    } else {
        // Initialize with undefined (null for now)
        emitInstruction(IROpcode::LOAD_CONST, IROperand::Register(destReg), IROperand(0.0));
    }
}

void IRGenerator::generateExpressionStatement(const ExpressionStatementNode* node) {
    generateExpression(node->expression.get());
}

void IRGenerator::generateBlockStatement(const BlockStatementNode* node) {
    for (const auto& stmt : node->statements) {
        generateStatement(stmt.get());
    }
}

int IRGenerator::generateExpression(const ASTNode* node) {
    if (!node) return -1;
    
    switch (node->type) {
        case ASTNodeType::BINARY_EXPRESSION:
            return generateBinaryExpression(static_cast<const BinaryExpressionNode*>(node));
            
        case ASTNodeType::UNARY_EXPRESSION:
            return generateUnaryExpression(static_cast<const UnaryExpressionNode*>(node));
            
        case ASTNodeType::ASSIGNMENT_EXPRESSION:
            return generateAssignmentExpression(static_cast<const AssignmentExpressionNode*>(node));
              case ASTNodeType::IDENTIFIER:
            return generateIdentifier(static_cast<const IdentifierNode*>(node));
            
        case ASTNodeType::NUMBER_LITERAL:
            return generateNumberLiteral(static_cast<const NumberLiteralNode*>(node));
            
        case ASTNodeType::STRING_LITERAL:
            return generateStringLiteral(static_cast<const StringLiteralNode*>(node));
            
        default:
            errorHandler.reportRuntimeError("Unsupported expression type in IR generation", node->line, node->column);
            return -1;
    }
}

int IRGenerator::generateBinaryExpression(const BinaryExpressionNode* node) {
    int leftReg = generateExpression(node->left.get());
    int rightReg = generateExpression(node->right.get());
    int resultReg = currentFunction->allocateRegister();
    
    IROpcode opcode = binaryOperatorToOpcode(node->operator_);
    emitInstruction(opcode, IROperand::Register(resultReg), 
                   IROperand::Register(leftReg), IROperand::Register(rightReg));
    
    return resultReg;
}

int IRGenerator::generateUnaryExpression(const UnaryExpressionNode* node) {
    int operandReg = generateExpression(node->operand.get());
    int resultReg = currentFunction->allocateRegister();
    
    IROpcode opcode = unaryOperatorToOpcode(node->operator_);
    emitInstruction(opcode, IROperand::Register(resultReg), IROperand::Register(operandReg));
    
    return resultReg;
}

int IRGenerator::generateAssignmentExpression(const AssignmentExpressionNode* node) {
    // For now, only handle identifier assignments
    if (node->left->type != ASTNodeType::IDENTIFIER) {
        errorHandler.reportRuntimeError("Complex assignment targets not supported in IR", node->line, node->column);
        return -1;
    }
    
    const IdentifierNode* id = static_cast<const IdentifierNode*>(node->left.get());
    int valueReg = generateExpression(node->right.get());
    
    // Get or create variable register
    int varReg = currentFunction->getVariableRegister(id->name);
    if (varReg == -1) {
        varReg = currentFunction->allocateRegister();
        currentFunction->mapVariable(id->name, varReg);
    }
    
    emitInstruction(IROpcode::STORE_VAR, IROperand::Register(varReg), IROperand::Register(valueReg));
    return valueReg;
}

int IRGenerator::generateIdentifier(const IdentifierNode* node) {
    int varReg = currentFunction->getVariableRegister(node->name);
    if (varReg == -1) {
        errorHandler.reportRuntimeError("Undefined variable in IR: " + node->name, node->line, node->column);
        return -1;
    }
    
    int resultReg = currentFunction->allocateRegister();
    emitInstruction(IROpcode::LOAD_VAR, IROperand::Register(resultReg), IROperand::Register(varReg));
    return resultReg;
}

int IRGenerator::generateNumberLiteral(const NumberLiteralNode* node) {
    int resultReg = currentFunction->allocateRegister();
    emitInstruction(IROpcode::LOAD_CONST, IROperand::Register(resultReg), IROperand(node->value));
    return resultReg;
}

int IRGenerator::generateStringLiteral(const StringLiteralNode* node) {
    int resultReg = currentFunction->allocateRegister();
    emitInstruction(IROpcode::LOAD_CONST, IROperand::Register(resultReg), IROperand(node->value));
    return resultReg;
}

void IRGenerator::emitInstruction(IROpcode opcode, IROperand dest, IROperand src1, IROperand src2) {
    if (currentBlock) {
        currentBlock->addInstruction(opcode, dest, src1, src2);
    }
}

//<---------IR OPTIMIZER IMPLEMENTATION--------->
void IROptimizer::constantFolding(IRFunction& function) {
    // Basic constant folding implementation
    for (const auto& block : function.getBlocks()) {
        auto& instructions = const_cast<std::vector<IRInstruction>&>(block->getInstructions());
        
        for (auto& inst : instructions) {
            // Example: fold ADD operations with constant operands
            if (inst.opcode == IROpcode::ADD && 
                inst.src1.type == IROperandType::IMMEDIATE && 
                inst.src2.type == IROperandType::IMMEDIATE) {
                
                double result = inst.src1.data.number + inst.src2.data.number;
                inst.opcode = IROpcode::LOAD_CONST;
                inst.src1 = IROperand(result);
                inst.src2 = IROperand(0.0); // Clear unused operand
            }
        }
    }
}

void IROptimizer::deadCodeElimination(IRFunction& function) {
    // Simple dead code elimination - mark used registers and remove unused instructions
    // This would be more complex in a full implementation
    for (const auto& block : function.getBlocks()) {
        // Implementation would go here
    }
}

void IROptimizer::optimizeFunction(IRFunction& function, int optimizationLevel) {
    if (optimizationLevel >= 1) {
        constantFolding(function);
        removeEmptyBlocks(function);
    }
    
    if (optimizationLevel >= 2) {
        deadCodeElimination(function);
        mergeBlocks(function);
    }
    
    if (optimizationLevel >= 3) {
        commonSubexpressionElimination(function);
        loopOptimization(function);
    }
}

void IROptimizer::removeEmptyBlocks(IRFunction& function) {
    // Remove blocks with no instructions or only NOP instructions
    // Implementation would be more complex in reality
}

void IROptimizer::mergeBlocks(IRFunction& function) {
    // Merge blocks that can be combined
    // Implementation would go here
}

void IROptimizer::eliminateUnreachableCode(IRFunction& function) {
    // Remove unreachable basic blocks
    // Implementation would go here
}

void IROptimizer::commonSubexpressionElimination(IRFunction& function) {
    // Eliminate common subexpressions
    // Implementation would go here
}

void IROptimizer::loopOptimization(IRFunction& function) {
    // Optimize loops (unrolling, invariant code motion, etc.)
    // Implementation would go here
}

void IROptimizer::inlineSmallFunctions(IRFunction& function) {
    // Inline small functions
    // Implementation would go here
}

//<---------UTILITY FUNCTIONS--------->
std::string opcodeToString(IROpcode opcode) {
    switch (opcode) {
        case IROpcode::ADD: return "ADD";
        case IROpcode::SUB: return "SUB";
        case IROpcode::MUL: return "MUL";
        case IROpcode::DIV: return "DIV";
        case IROpcode::MOD: return "MOD";
        case IROpcode::EQ: return "EQ";
        case IROpcode::NE: return "NE";
        case IROpcode::LT: return "LT";
        case IROpcode::GT: return "GT";
        case IROpcode::LE: return "LE";
        case IROpcode::GE: return "GE";
        case IROpcode::AND: return "AND";
        case IROpcode::OR: return "OR";
        case IROpcode::NOT: return "NOT";
        case IROpcode::LOAD_CONST: return "LOAD_CONST";
        case IROpcode::LOAD_VAR: return "LOAD_VAR";
        case IROpcode::STORE_VAR: return "STORE_VAR";
        case IROpcode::JUMP: return "JUMP";
        case IROpcode::JUMP_IF_TRUE: return "JUMP_IF_TRUE";
        case IROpcode::JUMP_IF_FALSE: return "JUMP_IF_FALSE";
        case IROpcode::CALL: return "CALL";
        case IROpcode::RETURN: return "RETURN";
        case IROpcode::NOP: return "NOP";
        case IROpcode::HALT: return "HALT";
        default: return "UNKNOWN";
    }
}

std::string operandToString(const IROperand& operand) {
    switch (operand.type) {
        case IROperandType::IMMEDIATE:
            if (!operand.stringValue.empty()) {
                return "\"" + operand.stringValue + "\"";
            } else {
                return std::to_string(operand.data.number);
            }
        case IROperandType::REGISTER:
            return "r" + std::to_string(operand.data.reg);
        case IROperandType::VARIABLE:
            return operand.stringValue;
        case IROperandType::LABEL:
            return "L" + std::to_string(operand.data.label);
        default:
            return "unknown";
    }
}

std::string instructionToString(const IRInstruction& instruction) {
    std::string result = opcodeToString(instruction.opcode);
    result += " " + operandToString(instruction.dest);
    result += ", " + operandToString(instruction.src1);
    if (instruction.src2.type != IROperandType::IMMEDIATE || instruction.src2.data.number != 0.0) {
        result += ", " + operandToString(instruction.src2);
    }
    return result;
}

IROpcode binaryOperatorToOpcode(const std::string& op) {
    if (op == "+") return IROpcode::ADD;
    if (op == "-") return IROpcode::SUB;
    if (op == "*") return IROpcode::MUL;
    if (op == "/") return IROpcode::DIV;
    if (op == "%") return IROpcode::MOD;
    if (op == "==") return IROpcode::EQ;
    if (op == "!=") return IROpcode::NE;
    if (op == "<") return IROpcode::LT;
    if (op == ">") return IROpcode::GT;
    if (op == "<=") return IROpcode::LE;
    if (op == ">=") return IROpcode::GE;
    return IROpcode::NOP; // Unknown operator
}

IROpcode unaryOperatorToOpcode(const std::string& op) {
    if (op == "-") return IROpcode::SUB; // Negate
    if (op == "+") return IROpcode::ADD; // Positive (no-op)
    if (op == "!") return IROpcode::NOT;
    return IROpcode::NOP; // Unknown operator
}

} // namespace Quanta
