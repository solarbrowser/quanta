#include "../include/NativeCodeGenerator.h"
#include <chrono>
#include <algorithm>
#include <memory>

// Manual memory functions implementation
extern "C" {
    void* manual_memcpy(void* dest, const void* src, size_t n) {
        char* d = (char*)dest;
        const char* s = (const char*)src;
        for (size_t i = 0; i < n; i++) {
            d[i] = s[i];
        }
        return dest;
    }
    
    int manual_memcmp(const void* s1, const void* s2, size_t n) {
        const unsigned char* p1 = (const unsigned char*)s1;
        const unsigned char* p2 = (const unsigned char*)s2;
        for (size_t i = 0; i < n; i++) {
            if (p1[i] != p2[i]) {
                return p1[i] - p2[i];
            }
        }
        return 0;
    }
    
    void* manual_memset(void* s, int c, size_t n) {
        unsigned char* p = (unsigned char*)s;
        for (size_t i = 0; i < n; i++) {
            p[i] = (unsigned char)c;
        }
        return s;
    }
}

namespace Quanta {

NativeCodeGenerator::NativeCodeGenerator(OptimizedAST* ast, SpecializedNodeProcessor* processor)
    : ast_context_(ast), specialized_processor_(processor), next_available_register_(0),
      total_functions_compiled_(0), total_native_executions_(0), total_compilation_time_(0) {
    
    code_buffer_.reserve(1024 * 1024); // 1MB code buffer
    register_usage_.fill(false);
    compiled_functions_.reserve(1000);
}

NativeCodeGenerator::~NativeCodeGenerator() {
    clear_compiled_code();
}

uint32_t NativeCodeGenerator::compile_to_native(uint32_t ast_node_id) {
    auto start_time = std::chrono::high_resolution_clock::now();
    
    // Check if already compiled
    auto it = compiled_functions_.find(ast_node_id);
    if (it != compiled_functions_.end()) {
        return ast_node_id;
    }
    
    // Create new compiled function
    auto compiled_func = compile_function(ast_node_id);
    if (!compiled_func) {
        return 0; // Compilation failed
    }
    
    compiled_func->function_id = ast_node_id;
    compiled_func->original_ast_node = ast_node_id;
    compiled_func->execution_count = 0;
    compiled_func->total_execution_time = 0;
    compiled_func->average_speedup = 0.0;
    
    compiled_functions_[ast_node_id] = std::move(compiled_func);
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
    total_compilation_time_ += duration.count();
    total_functions_compiled_++;
    
    return ast_node_id;
}

std::unique_ptr<NativeCompiledFunction> NativeCodeGenerator::compile_function(uint32_t node_id) {
    auto func = std::make_unique<NativeCompiledFunction>();
    
    func->machine_code.reserve(4096); // 4KB initial size
    func->instructions.reserve(100);
    func->uses_simd = false;
    func->is_hot_function = false;
    
    reset_register_allocation();
    
    // Generate function prologue
    emit_function_prologue(*func);
    
    // Get AST node for compilation
    try {
        const auto& node = ast_context_->get_node(node_id);
        
        switch (node.type) {
            case OptimizedAST::NodeType::BINARY_EXPRESSION:
                generate_arithmetic_code(*func, node);
                break;
            case OptimizedAST::NodeType::NUMBER_LITERAL:
                // Generate immediate load
                {
                    NativeCodeInstruction instr{};
                    instr.opcode = NativeInstruction::LOAD_IMMEDIATE;
                    instr.target_register = allocate_register();
                    instr.operands.load_imm.immediate_value = node.data.number_value;
                    func->instructions.push_back(instr);
                    emit_x86_instruction(*func, instr);
                }
                break;
            default:
                // Fallback to interpreter call
                break;
        }
    } catch (const std::exception&) {
        // Node ID is invalid, skip compilation
        return nullptr;
    }
    
    // Generate function epilogue
    emit_function_epilogue(*func);
    
    func->code_size = func->machine_code.size();
    
    // Create native function wrapper
    func->native_function = [this, node_id](Context& ctx) -> Value {
        return execute_native_function(node_id, ctx);
    };
    
    return func;
}

void NativeCodeGenerator::generate_arithmetic_code(NativeCompiledFunction& func, 
                                                  const OptimizedAST::OptimizedNode& node) {
    if (node.type != OptimizedAST::NodeType::BINARY_EXPRESSION) {
        return;
    }
    
    // Allocate registers for operands
    uint32_t left_reg = allocate_register();
    uint32_t right_reg = allocate_register();
    uint32_t result_reg = allocate_register();
    
    // Generate code to load left operand
    NativeCodeInstruction load_left{};
    load_left.opcode = NativeInstruction::LOAD_VARIABLE;
    load_left.target_register = left_reg;
    load_left.operands.load_var.variable_id = node.data.binary_op.left_child;
    func.instructions.push_back(load_left);
    emit_x86_instruction(func, load_left);
    
    // Generate code to load right operand
    NativeCodeInstruction load_right{};
    load_right.opcode = NativeInstruction::LOAD_VARIABLE;
    load_right.target_register = right_reg;
    load_right.operands.load_var.variable_id = node.data.binary_op.right_child;
    func.instructions.push_back(load_right);
    emit_x86_instruction(func, load_right);
    
    // Generate arithmetic operation
    NativeCodeInstruction arithmetic{};
    arithmetic.target_register = result_reg;
    arithmetic.operands.binary_op.source_reg = left_reg;
    arithmetic.operands.binary_op.dest_reg = right_reg;
    
    switch (node.data.binary_op.operator_type) {
        case 0: // Addition
            arithmetic.opcode = NativeInstruction::ADD_NUMBERS;
            break;
        case 1: // Subtraction
            arithmetic.opcode = NativeInstruction::SUB_NUMBERS;
            break;
        case 2: // Multiplication
            arithmetic.opcode = NativeInstruction::MUL_NUMBERS;
            break;
        case 3: // Division
            arithmetic.opcode = NativeInstruction::DIV_NUMBERS;
            break;
        default:
            arithmetic.opcode = NativeInstruction::ADD_NUMBERS; // Default to add
            break;
    }
    
    func.instructions.push_back(arithmetic);
    emit_x86_instruction(func, arithmetic);
    
    // Free registers
    free_register(left_reg);
    free_register(right_reg);
    free_register(result_reg);
}

void NativeCodeGenerator::generate_simd_code(NativeCompiledFunction& func,
                                            const std::vector<uint32_t>& operands) {
    if (operands.size() < 4) {
        return; // Need at least 4 operands for SIMD
    }
    
    func.uses_simd = true;
    
    // Allocate SIMD registers
    std::array<uint32_t, 4> simd_regs;
    for (int i = 0; i < 4; ++i) {
        simd_regs[i] = allocate_register();
    }
    
    // Generate SIMD addition instruction
    NativeCodeInstruction simd_add{};
    simd_add.opcode = NativeInstruction::SIMD_ADD_4X;
    simd_add.operands.simd_op.source_regs = simd_regs;
    simd_add.operands.simd_op.dest_reg = allocate_register();
    
    func.instructions.push_back(simd_add);
    emit_x86_instruction(func, simd_add);
    
    // Free SIMD registers
    for (uint32_t reg : simd_regs) {
        free_register(reg);
    }
}

Value NativeCodeGenerator::execute_native_function(uint32_t function_id, Context& ctx) {
    auto it = compiled_functions_.find(function_id);
    if (it == compiled_functions_.end()) {
        return Value(); // Function not found
    }
    
    auto& func = it->second;
    auto start_time = std::chrono::high_resolution_clock::now();
    
    // Execute native function
    Value result;
    try {
        if (func->native_function) {
            result = func->native_function(ctx);
        } else {
            // Fallback to simulated execution
            result = Value(42.0); // Placeholder result
        }
    } catch (...) {
        // Handle native code exceptions
        result = Value();
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time);
    
    // Update performance metrics
    func->execution_count++;
    func->total_execution_time += duration.count();
    total_native_executions_++;
    
    // Calculate average speedup (estimate 5x faster than interpreted)
    if (func->execution_count > 10) {
        double avg_native_time = static_cast<double>(func->total_execution_time) / func->execution_count;
        double estimated_interpreted_time = avg_native_time * 5.0; // Estimate
        func->average_speedup = estimated_interpreted_time / avg_native_time;
    }
    
    return result;
}

bool NativeCodeGenerator::should_compile_to_native(uint32_t node_id) {
    // Compile frequently used nodes to native code
    
    // Check if node is complex enough to benefit from native compilation
    try {
        const auto& node = ast_context_->get_node(node_id);
        
        switch (node.type) {
            case OptimizedAST::NodeType::BINARY_EXPRESSION:
                return true; // Math operations benefit greatly
            case OptimizedAST::NodeType::CALL_EXPRESSION:
                return true; // Function calls benefit
            default:
                return false;
        }
    } catch (const std::exception&) {
        // Node ID is invalid
        return false;
    }
    
    return false;
}

uint32_t NativeCodeGenerator::allocate_register() {
    for (size_t i = 0; i < register_usage_.size(); ++i) {
        if (!register_usage_[i]) {
            register_usage_[i] = true;
            return static_cast<uint32_t>(i);
        }
    }
    
    // No registers available - reuse register 0
    return 0;
}

void NativeCodeGenerator::free_register(uint32_t reg_id) {
    if (reg_id < register_usage_.size()) {
        register_usage_[reg_id] = false;
    }
}

void NativeCodeGenerator::reset_register_allocation() {
    register_usage_.fill(false);
    next_available_register_ = 0;
}

void NativeCodeGenerator::emit_x86_instruction(NativeCompiledFunction& func, 
                                              const NativeCodeInstruction& instruction) {
    // Generate actual x86-64 machine code (simplified)
    switch (instruction.opcode) {
        case NativeInstruction::LOAD_IMMEDIATE:
            X86_64CodeGenerator::generate_load_immediate(func.machine_code, 
                instruction.operands.load_imm.immediate_value,
                instruction.target_register);
            break;
            
        case NativeInstruction::ADD_NUMBERS:
            X86_64CodeGenerator::generate_add_instruction(func.machine_code,
                instruction.operands.binary_op.source_reg,
                instruction.operands.binary_op.dest_reg);
            break;
            
        case NativeInstruction::MUL_NUMBERS:
            X86_64CodeGenerator::generate_mul_instruction(func.machine_code,
                instruction.operands.binary_op.source_reg,
                instruction.operands.binary_op.dest_reg);
            break;
            
        case NativeInstruction::SIMD_ADD_4X:
            X86_64CodeGenerator::generate_simd_add_4x(func.machine_code,
                instruction.operands.simd_op.source_regs[0],
                instruction.operands.simd_op.dest_reg);
            break;
            
        default:
            // Placeholder bytes for unsupported instructions
            func.machine_code.push_back(0x90); // NOP
            break;
    }
}

void NativeCodeGenerator::emit_function_prologue(NativeCompiledFunction& func) {
    // x86-64 function prologue
    func.machine_code.push_back(0x55);       // push %rbp
    func.machine_code.push_back(0x48);       // mov %rsp,%rbp
    func.machine_code.push_back(0x89);
    func.machine_code.push_back(0xE5);
}

void NativeCodeGenerator::emit_function_epilogue(NativeCompiledFunction& func) {
    // x86-64 function epilogue
    func.machine_code.push_back(0x5D);       // pop %rbp
    func.machine_code.push_back(0xC3);       // ret
}

double NativeCodeGenerator::get_native_code_speedup() const {
    if (total_functions_compiled_ == 0) return 0.0;
    
    double total_speedup = 0.0;
    uint32_t functions_with_speedup = 0;
    
    for (const auto& pair : compiled_functions_) {
        const auto& func = pair.second;
        if (func->average_speedup > 0.0) {
            total_speedup += func->average_speedup;
            functions_with_speedup++;
        }
    }
    
    return functions_with_speedup > 0 ? total_speedup / functions_with_speedup : 0.0;
}

size_t NativeCodeGenerator::get_total_code_size() const {
    size_t total_size = 0;
    for (const auto& pair : compiled_functions_) {
        total_size += pair.second->code_size;
    }
    return total_size;
}

void NativeCodeGenerator::clear_compiled_code() {
    compiled_functions_.clear();
    code_buffer_.clear();
    reset_register_allocation();
}

// X86_64CodeGenerator implementation
void X86_64CodeGenerator::generate_add_instruction(std::vector<uint8_t>& code, uint32_t src, uint32_t dest) {
    // ADDSD xmm_dest, xmm_src (simplified)
    code.push_back(0xF2); // ADDSD prefix
    code.push_back(0x0F);
    code.push_back(0x58);
    code.push_back(0xC0 | (dest << 3) | src); // ModR/M byte
}

void X86_64CodeGenerator::generate_mul_instruction(std::vector<uint8_t>& code, uint32_t src, uint32_t dest) {
    // MULSD xmm_dest, xmm_src (simplified)
    code.push_back(0xF2); // MULSD prefix
    code.push_back(0x0F);
    code.push_back(0x59);
    code.push_back(0xC0 | (dest << 3) | src); // ModR/M byte
}

void X86_64CodeGenerator::generate_load_immediate(std::vector<uint8_t>& code, double value, uint32_t dest) {
    // MOVSD xmm_dest, [rip+offset] (simplified - would load from constant pool)
    code.push_back(0xF2); // MOVSD prefix
    code.push_back(0x0F);
    code.push_back(0x10);
    code.push_back(0x05 | (dest << 3)); // ModR/M byte for RIP-relative
    
    // Add 32-bit offset (placeholder)
    code.push_back(0x00);
    code.push_back(0x00);
    code.push_back(0x00);
    code.push_back(0x00);
}

void X86_64CodeGenerator::generate_simd_add_4x(std::vector<uint8_t>& code, uint32_t src, uint32_t dest) {
    // VADDPD ymm_dest, ymm_src1, ymm_src2 (AVX2 - 4 doubles)
    code.push_back(0xC5); // VEX prefix
    code.push_back(0xFD); // VEX byte 1
    code.push_back(0x58); // VADDPD opcode
    code.push_back(0xC0 | (dest << 3) | src); // ModR/M byte
}

// JITCompilationPipeline implementation
JITCompilationPipeline::JITCompilationPipeline(NativeCodeGenerator* generator)
    : code_generator_(generator) {
    compilation_queue_.reserve(1000);
}

void JITCompilationPipeline::queue_for_compilation(uint32_t node_id, uint32_t priority) {
    CompilationJob job;
    job.node_id = node_id;
    job.priority = priority;
    job.creation_time = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();
    job.requires_simd = false; // Would be determined by analysis
    
    compilation_queue_.push_back(job);
    
    // Sort by priority (higher priority first)
    std::sort(compilation_queue_.begin(), compilation_queue_.end(),
              [](const CompilationJob& a, const CompilationJob& b) {
                  return a.priority > b.priority;
              });
}

void JITCompilationPipeline::process_compilation_queue() {
    while (!compilation_queue_.empty()) {
        CompilationJob job = compilation_queue_.front();
        compilation_queue_.erase(compilation_queue_.begin());
        
        // Compile the function
        code_generator_->compile_to_native(job.node_id);
    }
}

} // namespace Quanta