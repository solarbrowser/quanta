/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "quanta/Value.h"
#include "quanta/Context.h"
#include <vector>
#include <cstdint>
#include <memory>
#include <regex>

namespace Quanta {


enum class FastOp : uint8_t {
    LOAD_NUMBER = 0x01,
    LOAD_VAR = 0x02,
    STORE_VAR = 0x03,
    
    FAST_ADD = 0x10,
    FAST_SUB = 0x11,
    FAST_MUL = 0x12,
    FAST_DIV = 0x13,
    
    FAST_JUMP = 0x20,
    FAST_LOOP = 0x21,
    FAST_CALL = 0x22,
    FAST_RETURN = 0x23,
    
    MATH_LOOP_SUM = 0x30,
    NATIVE_EXEC = 0x31,
};

struct FastInstruction {
    FastOp op;
    uint32_t a, b, c;
    double immediate;
    
    FastInstruction(FastOp op, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0, double imm = 0.0)
        : op(op), a(a), b(b), c(c), immediate(imm) {}
};

class FastBytecodeVM {
private:
    std::vector<double> registers_;
    std::vector<FastInstruction> code_;
    uint32_t pc_;
    
public:
    FastBytecodeVM();
    ~FastBytecodeVM();
    
    bool compile_direct(const std::string& source);
    
    Value execute_fast();
    
    void emit(FastOp op, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0, double imm = 0.0);
    
    uint32_t get_register_count() const { return static_cast<uint32_t>(registers_.size()); }
    
private:
    bool is_simple_math_loop(const std::string& source);
    void compile_math_loop_direct(const std::string& source);
    
    Value execute_instruction(const FastInstruction& instr);
};


class DirectPatternCompiler {
public:
    static bool try_compile_math_loop(const std::string& source, FastBytecodeVM& vm);
    
    struct LoopParams {
        std::string var_name;
        int64_t start_val;
        int64_t end_val;
        std::string operation;
        bool valid;
    };
    
    static LoopParams extract_loop_params(const std::string& source);
};

}
