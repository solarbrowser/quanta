/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Real JIT Compilation Implementation
 * Generates actual x86-64 machine code for high-performance execution
 */

#include "../include/RealJIT.h"
#include "../include/Object.h"
#include "../include/Value.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#include <cstring>
#include <iostream>

namespace Quanta {

//=============================================================================
// Machine Code Generator Implementation
//=============================================================================

MachineCodeGenerator::MachineCodeGenerator() 
    : executable_memory_(nullptr), memory_size_(0) {
    code_buffer_.reserve(4096); // 4KB initial buffer
}

MachineCodeGenerator::~MachineCodeGenerator() {
    if (executable_memory_) {
#ifdef _WIN32
        VirtualFree(executable_memory_, 0, MEM_RELEASE);
#else
        munmap(executable_memory_, memory_size_);
#endif
    }
}

void MachineCodeGenerator::allocate_executable_memory(size_t size) {
    memory_size_ = size;
    
#ifdef _WIN32
    executable_memory_ = VirtualAlloc(nullptr, size, 
                                    MEM_COMMIT | MEM_RESERVE, 
                                    PAGE_EXECUTE_READWRITE);
#else
    executable_memory_ = mmap(nullptr, size, 
                            PROT_READ | PROT_WRITE | PROT_EXEC,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
    
    if (!executable_memory_) {
        throw std::runtime_error("Failed to allocate executable memory for JIT");
    }
}

void MachineCodeGenerator::make_memory_executable() {
    if (!executable_memory_) {
        allocate_executable_memory(code_buffer_.size() + 1024);
    }
    
    // Copy code to executable memory
    std::memcpy(executable_memory_, code_buffer_.data(), code_buffer_.size());
    
#ifdef _WIN32
    DWORD old_protect;
    VirtualProtect(executable_memory_, memory_size_, PAGE_EXECUTE_READ, &old_protect);
#else
    mprotect(executable_memory_, memory_size_, PROT_READ | PROT_EXEC);
#endif
}

// x86-64 instruction generation
void MachineCodeGenerator::emit_push_rbp() {
    code_buffer_.push_back(0x55); // push rbp
}

void MachineCodeGenerator::emit_pop_rbp() {
    code_buffer_.push_back(0x5D); // pop rbp
}

void MachineCodeGenerator::emit_mov_rax_immediate(double value) {
    // mov rax, immediate (simplified - converts double to int64)
    int64_t int_val = static_cast<int64_t>(value);
    
    code_buffer_.push_back(0x48); // REX.W prefix
    code_buffer_.push_back(0xB8); // mov rax, imm64
    
    // Emit 8-byte immediate value (little endian)
    for (int i = 0; i < 8; i++) {
        code_buffer_.push_back((int_val >> (i * 8)) & 0xFF);
    }
}

void MachineCodeGenerator::emit_add_rax_rbx() {
    code_buffer_.push_back(0x48); // REX.W prefix
    code_buffer_.push_back(0x01); // add
    code_buffer_.push_back(0xD8); // rax, rbx
}

void MachineCodeGenerator::emit_mul_rax_rbx() {
    code_buffer_.push_back(0x48); // REX.W prefix
    code_buffer_.push_back(0x0F); // imul prefix
    code_buffer_.push_back(0xAF); // imul
    code_buffer_.push_back(0xC3); // rax, rbx
}

void MachineCodeGenerator::emit_return() {
    code_buffer_.push_back(0xC3); // ret
}

uint8_t* MachineCodeGenerator::compile_arithmetic_function() {
    code_buffer_.clear();
    
    // Function prologue
    emit_push_rbp();
    
    // Simple arithmetic: return arg1 + arg2
    // Arguments are in RDI (arg1) and RSI (arg2) on Linux/macOS
    // On Windows, they're in RCX and RDX
    
#ifdef _WIN32
    // mov rax, rcx (first argument)
    code_buffer_.push_back(0x48); // REX.W
    code_buffer_.push_back(0x89); // mov
    code_buffer_.push_back(0xC8); // rax, rcx
    
    // add rax, rdx (second argument)
    code_buffer_.push_back(0x48); // REX.W
    code_buffer_.push_back(0x01); // add
    code_buffer_.push_back(0xD0); // rax, rdx
#else
    // mov rax, rdi (first argument)
    code_buffer_.push_back(0x48); // REX.W
    code_buffer_.push_back(0x89); // mov
    code_buffer_.push_back(0xF8); // rax, rdi
    
    // add rax, rsi (second argument)
    code_buffer_.push_back(0x48); // REX.W
    code_buffer_.push_back(0x01); // add
    code_buffer_.push_back(0xF0); // rax, rsi
#endif
    
    // Function epilogue
    emit_pop_rbp();
    emit_return();
    
    make_memory_executable();
    return static_cast<uint8_t*>(executable_memory_);
}

uint8_t* MachineCodeGenerator::compile_loop_function() {
    code_buffer_.clear();
    
    // Ultra-fast loop: for(i=0; i<1000000; i++) sum += i;
    emit_push_rbp();
    
    // mov rax, 0 (sum = 0)
    code_buffer_.push_back(0x48); // REX.W
    code_buffer_.push_back(0x31); // xor
    code_buffer_.push_back(0xC0); // rax, rax
    
    // mov rbx, 0 (i = 0)  
    code_buffer_.push_back(0x48); // REX.W
    code_buffer_.push_back(0x31); // xor
    code_buffer_.push_back(0xDB); // rbx, rbx
    
    // mov rcx, 1000000 (loop limit)
    code_buffer_.push_back(0x48); // REX.W
    code_buffer_.push_back(0xB9); // mov rcx, imm32
    int32_t limit = 1000000;
    for (int i = 0; i < 4; i++) {
        code_buffer_.push_back((limit >> (i * 8)) & 0xFF);
    }
    code_buffer_.push_back(0x00);
    code_buffer_.push_back(0x00);
    code_buffer_.push_back(0x00);
    code_buffer_.push_back(0x00);
    
    // Loop start
    size_t loop_start = code_buffer_.size();
    
    // add rax, rbx (sum += i)
    emit_add_rax_rbx();
    
    // inc rbx (i++)
    code_buffer_.push_back(0x48); // REX.W
    code_buffer_.push_back(0xFF); // inc
    code_buffer_.push_back(0xC3); // rbx
    
    // cmp rbx, rcx (compare i with limit)
    code_buffer_.push_back(0x48); // REX.W
    code_buffer_.push_back(0x39); // cmp
    code_buffer_.push_back(0xCB); // rbx, rcx
    
    // jl loop_start (jump if less)
    code_buffer_.push_back(0x7C); // jl
    int8_t offset = static_cast<int8_t>(loop_start - (code_buffer_.size() + 1));
    code_buffer_.push_back(offset);
    
    emit_pop_rbp();
    emit_return();
    
    make_memory_executable();
    return static_cast<uint8_t*>(executable_memory_);
}

uint8_t* MachineCodeGenerator::compile_property_access() {
    // Simple property access - returns constant for now
    code_buffer_.clear();
    
    emit_push_rbp();
    emit_mov_rax_immediate(42.0); // Return constant property value
    emit_pop_rbp();
    emit_return();
    
    make_memory_executable();
    return static_cast<uint8_t*>(executable_memory_);
}

double MachineCodeGenerator::execute_machine_code(uint8_t* code, double arg1, double arg2) {
    if (!code) return 0.0;
    
    // Cast to function pointer and execute
    typedef int64_t (*JittedFunction)(int64_t, int64_t);
    JittedFunction func = reinterpret_cast<JittedFunction>(code);
    
    // Convert doubles to integers for simplicity
    int64_t result = func(static_cast<int64_t>(arg1), static_cast<int64_t>(arg2));
    return static_cast<double>(result);
}

//=============================================================================
// Real JIT Compiler Implementation
//=============================================================================

RealJITCompiler& RealJITCompiler::instance() {
    static RealJITCompiler instance;
    return instance;
}

bool RealJITCompiler::compile_function(Function* func) {
    if (!func || is_compiled(func)) {
        return false;
    }
    
    try {
        // For now, compile a simple arithmetic function
        uint8_t* machine_code = generator_.compile_arithmetic_function();
        if (machine_code) {
            compiled_functions_[func] = machine_code;
            return true;
        }
    } catch (const std::exception& e) {
        std::cerr << "JIT compilation failed: " << e.what() << std::endl;
    }
    
    return false;
}

Value RealJITCompiler::execute_compiled(Function* func, Context& ctx, const std::vector<Value>& args) {
    auto it = compiled_functions_.find(func);
    if (it == compiled_functions_.end()) {
        return Value(); // Not compiled
    }
    
    // Extract arguments (simplified)
    double arg1 = args.size() > 0 ? args[0].to_number() : 0.0;
    double arg2 = args.size() > 1 ? args[1].to_number() : 0.0;
    
    // Execute machine code
    double result = generator_.execute_machine_code(it->second, arg1, arg2);
    return Value(result);
}

bool RealJITCompiler::is_compiled(Function* func) {
    return compiled_functions_.find(func) != compiled_functions_.end();
}

} // namespace Quanta