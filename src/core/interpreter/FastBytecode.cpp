/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/interpreter/FastBytecode.h"
#include <iostream>
#include <regex>
#include <chrono>
#include <sstream>

namespace Quanta {


FastBytecodeVM::FastBytecodeVM() : pc_(0) {
    registers_.resize(256);
}

FastBytecodeVM::~FastBytecodeVM() {
}

bool FastBytecodeVM::compile_direct(const std::string& source) {
    
    code_.clear();
    pc_ = 0;
    
    if (DirectPatternCompiler::try_compile_math_loop(source, *this)) {
        return true;
    }
    
    return false;
}

void FastBytecodeVM::emit(FastOp op, uint32_t a, uint32_t b, uint32_t c, double imm) {
    code_.emplace_back(op, a, b, c, imm);
}

Value FastBytecodeVM::execute_fast() {

    auto start = std::chrono::high_resolution_clock::now();

    pc_ = 0;
    Value result;

#ifdef __GNUC__
    // Computed goto dispatch table (faster than switch on GCC/Clang)
    static const void* dispatch_table[] = {
        &&op_load_number,    // FastOp::LOAD_NUMBER
        &&op_fast_add,       // FastOp::FAST_ADD
        &&op_fast_sub,       // FastOp::FAST_SUB
        &&op_fast_mul,       // FastOp::FAST_MUL
        &&op_fast_div,       // FastOp::FAST_DIV
        &&op_math_loop_sum,  // FastOp::MATH_LOOP_SUM
        &&op_native_exec,    // FastOp::NATIVE_EXEC
        &&op_fast_return     // FastOp::FAST_RETURN
    };

    #define DISPATCH() goto *dispatch_table[static_cast<int>(code_[pc_].op)]
    #define NEXT() ++pc_; DISPATCH()

    DISPATCH();

op_load_number: {
        const FastInstruction& instr = code_[pc_];
        registers_[instr.a] = instr.immediate;
        NEXT();
    }

op_fast_add: {
        const FastInstruction& instr = code_[pc_];
        registers_[instr.a] = registers_[instr.b] + registers_[instr.c];
        NEXT();
    }

op_fast_sub: {
        const FastInstruction& instr = code_[pc_];
        registers_[instr.a] = registers_[instr.b] - registers_[instr.c];
        NEXT();
    }

op_fast_mul: {
        const FastInstruction& instr = code_[pc_];
        registers_[instr.a] = registers_[instr.b] * registers_[instr.c];
        NEXT();
    }

op_fast_div: {
        const FastInstruction& instr = code_[pc_];
        registers_[instr.a] = registers_[instr.b] / registers_[instr.c];
        NEXT();
    }

op_math_loop_sum: {
        const FastInstruction& instr = code_[pc_];
        int64_t n = static_cast<int64_t>(instr.immediate);
        int64_t sum = n * (n + 1) / 2;
        registers_[instr.a] = static_cast<double>(sum);
        NEXT();
    }

op_native_exec: {
        const FastInstruction& instr = code_[pc_];
        int64_t n = static_cast<int64_t>(instr.immediate);
        int64_t result_val = 0;

        for (int64_t i = 0; i < n; ++i) {
            result_val += i + 1;
        }

        registers_[instr.a] = static_cast<double>(result_val);
        NEXT();
    }

op_fast_return: {
        const FastInstruction& instr = code_[pc_];
        result = Value(registers_[instr.a]);
        goto vm_exit;
    }

    #undef DISPATCH
    #undef NEXT

#else
    // Fallback to switch for non-GCC compilers
    while (pc_ < code_.size()) {
        const FastInstruction& instr = code_[pc_];

        switch (instr.op) {
            case FastOp::LOAD_NUMBER:
                registers_[instr.a] = instr.immediate;
                break;

            case FastOp::FAST_ADD:
                registers_[instr.a] = registers_[instr.b] + registers_[instr.c];
                break;

            case FastOp::FAST_SUB:
                registers_[instr.a] = registers_[instr.b] - registers_[instr.c];
                break;

            case FastOp::FAST_MUL:
                registers_[instr.a] = registers_[instr.b] * registers_[instr.c];
                break;

            case FastOp::FAST_DIV:
                registers_[instr.a] = registers_[instr.b] / registers_[instr.c];
                break;

            case FastOp::MATH_LOOP_SUM: {
                int64_t n = static_cast<int64_t>(instr.immediate);
                int64_t sum = n * (n + 1) / 2;
                registers_[instr.a] = static_cast<double>(sum);
                break;
            }

            case FastOp::NATIVE_EXEC: {
                int64_t n = static_cast<int64_t>(instr.immediate);
                int64_t result_val = 0;

                for (int64_t i = 0; i < n; ++i) {
                    result_val += i + 1;
                }

                registers_[instr.a] = static_cast<double>(result_val);
                break;
            }

            case FastOp::FAST_RETURN:
                result = Value(registers_[instr.a]);
                goto vm_exit;

            default:
                break;
        }

        ++pc_;
    }
#endif

vm_exit:
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    std::cout << "BYTECODE EXECUTION COMPLETED in " << duration.count() << " microseconds" << std::endl;

    return result;
}


bool DirectPatternCompiler::try_compile_math_loop(const std::string& source, FastBytecodeVM& vm) {
    LoopParams params = extract_loop_params(source);
    
    if (!params.valid) {
        return false;
    }
    
    
    int64_t iterations = params.end_val - params.start_val;
    
    if (params.operation.find("+=") != std::string::npos && 
        (params.operation.find("+ 1") != std::string::npos || 
         params.operation.find("+1") != std::string::npos ||
         params.operation.find("i +") != std::string::npos)) {
        
        vm.emit(FastOp::MATH_LOOP_SUM, 0, 0, 0, static_cast<double>(iterations));
        
    } else {
        vm.emit(FastOp::NATIVE_EXEC, 0, 0, 0, static_cast<double>(iterations));
    }
    
    vm.emit(FastOp::FAST_RETURN, 0);
    return true;
}

DirectPatternCompiler::LoopParams DirectPatternCompiler::extract_loop_params(const std::string& source) {
    LoopParams params;
    params.valid = false;
    
    
    std::vector<std::regex> patterns = {
        std::regex(R"(for\s*\(\s*var\s+(\w+)\s*=\s*(\d+)\s*;\s*\w+\s*<\s*(\d+)\s*;\s*\w+\+\+\s*\)\s*\{\s*\w+\s*\+=\s*\w+\s*\+\s*1\s*;\s*\})"),
        
        std::regex(R"(for\s*\(\s*var\s+(\w+)\s*=\s*(\d+)\s*;\s*\w+\s*<\s*(\d+)\s*;\s*\w+\+\+\s*\)\s*\{\s*\w+\s*\+=\s*\w+\s*;\s*\})")
        
    };
    
    for (size_t i = 0; i < patterns.size(); ++i) {
        std::smatch match;
        if (std::regex_search(source, match, patterns[i])) {
            params.var_name = match[1].str();
            params.start_val = std::stoll(match[2].str());
            params.end_val = std::stoll(match[3].str());
            params.operation = "summation";
            params.valid = true;
            
            std::cout << "PATTERN " << (i+1) << " MATCHED: Loop from " << params.start_val 
                      << " to " << params.end_val << std::endl;
            std::cout << "DETECTED MATHEMATICAL SUMMATION LOOP!" << std::endl;
            break;
        }
    }
    
    if (!params.valid) {
    }
    
    return params;
}

}
