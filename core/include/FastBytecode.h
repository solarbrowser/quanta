/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "Value.h"
#include "Context.h"
#include <vector>
#include <cstdint>
#include <memory>
#include <regex>

namespace Quanta {

//=============================================================================
// ULTRA-FAST BYTECODE VM - No AST, Direct Execution
//=============================================================================

enum class FastOp : uint8_t {
    // Ultra-fast operations
    LOAD_NUMBER = 0x01,     // Load immediate number
    LOAD_VAR = 0x02,        // Load variable by register
    STORE_VAR = 0x03,       // Store to variable
    
    // Mathematical operations (optimized)
    FAST_ADD = 0x10,        // r[a] = r[b] + r[c]
    FAST_SUB = 0x11,        // r[a] = r[b] - r[c]
    FAST_MUL = 0x12,        // r[a] = r[b] * r[c]
    FAST_DIV = 0x13,        // r[a] = r[b] / r[c]
    
    // Control flow
    FAST_JUMP = 0x20,       // Unconditional jump
    FAST_LOOP = 0x21,       // Optimized loop instruction
    FAST_CALL = 0x22,       // Function call
    FAST_RETURN = 0x23,     // Return value
    
    // Special optimizations
    MATH_LOOP_SUM = 0x30,   // Ultra-fast mathematical sum loop
    NATIVE_EXEC = 0x31,     // Execute native C++ code
};

struct FastInstruction {
    FastOp op;
    uint32_t a, b, c;       // Register operands
    double immediate;       // Immediate value for numbers
    
    FastInstruction(FastOp op, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0, double imm = 0.0)
        : op(op), a(a), b(b), c(c), immediate(imm) {}
};

class FastBytecodeVM {
private:
    std::vector<double> registers_;     // Register file (all doubles for speed)
    std::vector<FastInstruction> code_; // Bytecode instructions
    uint32_t pc_;                       // Program counter
    
public:
    FastBytecodeVM();
    ~FastBytecodeVM();
    
    // Compile JavaScript directly to fast bytecode (skip AST)
    bool compile_direct(const std::string& source);
    
    // Execute bytecode with ultra-fast VM
    Value execute_fast();
    
    // Add optimized instruction
    void emit(FastOp op, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0, double imm = 0.0);
    
    // Get register count
    uint32_t get_register_count() const { return static_cast<uint32_t>(registers_.size()); }
    
private:
    // Pattern recognition for ultra-fast compilation
    bool is_simple_math_loop(const std::string& source);
    void compile_math_loop_direct(const std::string& source);
    
    // VM execution core
    Value execute_instruction(const FastInstruction& instr);
};

//=============================================================================
// Direct Pattern Compiler - Skip lexing/parsing for simple patterns
//=============================================================================

class DirectPatternCompiler {
public:
    // Detect and compile mathematical loops directly
    static bool try_compile_math_loop(const std::string& source, FastBytecodeVM& vm);
    
    // Extract loop parameters from source (regex-based, ultra-fast)
    struct LoopParams {
        std::string var_name;
        int64_t start_val;
        int64_t end_val;
        std::string operation;
        bool valid;
    };
    
    static LoopParams extract_loop_params(const std::string& source);
};

} // namespace Quanta