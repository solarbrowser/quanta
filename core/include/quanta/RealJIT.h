/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Real JIT Compilation for Execution
 * This generates actual x86-64 machine code for hot functions
 */

#ifndef QUANTA_REAL_JIT_H
#define QUANTA_REAL_JIT_H

#include <vector>
#include <unordered_map>
#include <memory>
#include <cstdint>

namespace Quanta {

class Function;
class Context;
class Value;

class MachineCodeGenerator {
public:
    MachineCodeGenerator();
    ~MachineCodeGenerator();
    
    uint8_t* compile_arithmetic_function();
    
    uint8_t* compile_loop_function();
    
    uint8_t* compile_property_access();
    
    double execute_machine_code(uint8_t* code, double arg1, double arg2 = 0);
    
private:
    std::vector<uint8_t> code_buffer_;
    void* executable_memory_;
    size_t memory_size_;
    
    void emit_mov_rax_immediate(double value);
    void emit_add_rax_rbx();
    void emit_mul_rax_rbx();
    void emit_return();
    void emit_push_rbp();
    void emit_pop_rbp();
    
    void allocate_executable_memory(size_t size);
    void make_memory_executable();
};

class RealJITCompiler {
public:
    static RealJITCompiler& instance();
    
    bool compile_function(Function* func);
    
    Value execute_compiled(Function* func, Context& ctx, const std::vector<Value>& args);
    
    bool is_compiled(Function* func);
    
private:
    std::unordered_map<Function*, uint8_t*> compiled_functions_;
    MachineCodeGenerator generator_;
    
    RealJITCompiler() = default;
};

}

#endif
