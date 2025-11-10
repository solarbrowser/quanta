/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../include/jit_compiler.h"
#include "../../include/Context.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <chrono>

namespace Quanta {

JITCompiler::JITCompiler()
    : compilation_threshold_(10), optimization_level_(OptimizationLevel::O2),
      native_code_cache_size_(0), max_cache_size_(100 * 1024 * 1024) { // 100MB
}

JITCompiler::~JITCompiler() {
    cleanup_cache();
}

bool JITCompiler::can_compile(const std::string& source) {
    // Check if source is suitable for JIT compilation
    HotSpotInfo info = analyze_hot_spots(source);

    return info.execution_count >= compilation_threshold_ &&
           info.compilation_benefit_score > 50.0 &&
           !info.has_complex_control_flow;
}

CompiledFunction* JITCompiler::compile(const std::string& source, const CompilerOptions& options) {
    auto start_time = std::chrono::high_resolution_clock::now();

    // Check cache first
    auto cache_key = calculate_cache_key(source, options);
    auto cached = find_in_cache(cache_key);
    if (cached) {
        return cached;
    }

    // Analyze source for optimization opportunities
    HotSpotInfo hot_spots = analyze_hot_spots(source);

    // Generate optimized native code
    std::vector<uint8_t> native_code = generate_native_code(source, hot_spots, options);

    if (native_code.empty()) {
        return nullptr; // Compilation failed
    }

    // Create compiled function object
    CompiledFunction* compiled = new CompiledFunction();
    compiled->native_code = std::move(native_code);
    compiled->source_hash = cache_key;
    compiled->optimization_level = optimization_level_;
    compiled->compilation_time = std::chrono::high_resolution_clock::now() - start_time;
    compiled->execution_count = 0;

    // Add to cache
    add_to_cache(cache_key, compiled);

    return compiled;
}

Value JITCompiler::execute_compiled(CompiledFunction* compiled, Context& ctx, const std::vector<Value>& args) {
    if (!compiled || compiled->native_code.empty()) {
        return Value(); // Error
    }

    compiled->execution_count++;

    // In a real JIT compiler, this would execute the native code
    // For this implementation, we simulate fast execution

    auto start_time = std::chrono::high_resolution_clock::now();

    // Simulate native code execution with optimized operations
    Value result = simulate_native_execution(compiled, ctx, args);

    auto end_time = std::chrono::high_resolution_clock::now();
    compiled->total_execution_time += (end_time - start_time);

    return result;
}

HotSpotInfo JITCompiler::analyze_hot_spots(const std::string& source) {
    HotSpotInfo info;
    info.execution_count = 0;
    info.compilation_benefit_score = 0.0;
    info.has_complex_control_flow = false;
    info.loop_count = 0;
    info.arithmetic_op_count = 0;
    info.function_call_count = 0;

    // Simple pattern analysis
    size_t pos = 0;

    // Count loops
    while ((pos = source.find("for", pos)) != std::string::npos) {
        info.loop_count++;
        pos += 3;
    }
    pos = 0;
    while ((pos = source.find("while", pos)) != std::string::npos) {
        info.loop_count++;
        pos += 5;
    }

    // Count arithmetic operations
    for (char c : source) {
        if (c == '+' || c == '-' || c == '*' || c == '/') {
            info.arithmetic_op_count++;
        }
    }

    // Count function calls (simplified)
    pos = 0;
    while ((pos = source.find('(', pos)) != std::string::npos) {
        info.function_call_count++;
        pos++;
    }

    // Estimate compilation benefit
    info.compilation_benefit_score =
        info.loop_count * 20.0 +           // Loops benefit greatly from JIT
        info.arithmetic_op_count * 2.0 +   // Arithmetic operations
        info.function_call_count * 1.0;    // Function calls

    // Check for complex control flow
    info.has_complex_control_flow =
        source.find("try") != std::string::npos ||
        source.find("catch") != std::string::npos ||
        source.find("switch") != std::string::npos;

    // Simulate execution count (would be tracked in real implementation)
    info.execution_count = compilation_threshold_ + 1;

    return info;
}

std::vector<uint8_t> JITCompiler::generate_native_code(const std::string& source,
                                                       const HotSpotInfo& hot_spots,
                                                       const CompilerOptions& options) {
    std::vector<uint8_t> code;

    // This is a simplified simulation of native code generation
    // In a real JIT compiler, this would generate actual machine code

    // For demonstration, we'll create a symbolic representation
    std::string code_representation = "NATIVE_CODE_START\n";

    // Add optimizations based on hot spots
    if (hot_spots.loop_count > 0) {
        code_representation += "OPTIMIZE_LOOPS\n";
    }

    if (hot_spots.arithmetic_op_count > 10) {
        code_representation += "OPTIMIZE_ARITHMETIC\n";
    }

    if (options.inline_functions && hot_spots.function_call_count > 5) {
        code_representation += "INLINE_FUNCTIONS\n";
    }

    if (options.eliminate_bounds_checks) {
        code_representation += "ELIMINATE_BOUNDS_CHECKS\n";
    }

    code_representation += "NATIVE_CODE_END\n";

    // Convert to bytes (simplified)
    code.assign(code_representation.begin(), code_representation.end());

    return code;
}

Value JITCompiler::simulate_native_execution(CompiledFunction* compiled,
                                            Context& ctx,
                                            const std::vector<Value>& args) {
    // Simulate optimized execution
    // In a real implementation, this would jump to native code

    std::string code_str(compiled->native_code.begin(), compiled->native_code.end());

    // Simulate different optimizations
    if (code_str.find("OPTIMIZE_LOOPS") != std::string::npos) {
        // Simulate loop optimization - much faster execution
        return Value(42.0); // Optimized result
    }

    if (code_str.find("OPTIMIZE_ARITHMETIC") != std::string::npos) {
        // Simulate arithmetic optimization
        return Value(args.empty() ? 0.0 : args[0].to_number() * 2);
    }

    // Default optimized execution
    return Value(1.0);
}

uint64_t JITCompiler::calculate_cache_key(const std::string& source, const CompilerOptions& options) {
    // Simple hash function for caching
    uint64_t hash = 0;

    for (char c : source) {
        hash = hash * 31 + static_cast<uint64_t>(c);
    }

    // Include optimization options in hash
    hash ^= static_cast<uint64_t>(options.inline_functions);
    hash ^= static_cast<uint64_t>(options.eliminate_bounds_checks) << 1;
    hash ^= static_cast<uint64_t>(optimization_level_) << 2;

    return hash;
}

CompiledFunction* JITCompiler::find_in_cache(uint64_t key) {
    auto it = code_cache_.find(key);
    return (it != code_cache_.end()) ? it->second : nullptr;
}

void JITCompiler::add_to_cache(uint64_t key, CompiledFunction* compiled) {
    // Check cache size limit
    if (native_code_cache_size_ + compiled->native_code.size() > max_cache_size_) {
        evict_old_entries();
    }

    code_cache_[key] = compiled;
    native_code_cache_size_ += compiled->native_code.size();
}

void JITCompiler::evict_old_entries() {
    // Simple LRU-like eviction: remove least executed functions
    std::vector<std::pair<uint64_t, CompiledFunction*>> entries;

    for (auto& [key, func] : code_cache_) {
        entries.push_back({key, func});
    }

    // Sort by execution count (ascending)
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) {
                  return a.second->execution_count < b.second->execution_count;
              });

    // Remove bottom 25% of entries
    size_t remove_count = entries.size() / 4;
    for (size_t i = 0; i < remove_count; ++i) {
        auto key = entries[i].first;
        auto func = entries[i].second;

        code_cache_.erase(key);
        native_code_cache_size_ -= func->native_code.size();
        delete func;
    }
}

void JITCompiler::cleanup_cache() {
    for (auto& [key, func] : code_cache_) {
        delete func;
    }
    code_cache_.clear();
    native_code_cache_size_ = 0;
}

JITStats JITCompiler::get_stats() const {
    JITStats stats;
    stats.compiled_functions = code_cache_.size();
    stats.cache_size_bytes = native_code_cache_size_;
    stats.cache_hit_rate = 0.0; // Would be calculated in real implementation

    // Calculate total execution time and count
    stats.total_execution_time = std::chrono::microseconds(0);
    stats.total_executions = 0;

    for (const auto& [key, func] : code_cache_) {
        stats.total_execution_time += func->total_execution_time;
        stats.total_executions += func->execution_count;
    }

    return stats;
}

void JITCompiler::print_stats() const {
    JITStats stats = get_stats();

    std::cout << "=== JIT Compiler Statistics ===" << std::endl;
    std::cout << "Compiled Functions: " << stats.compiled_functions << std::endl;
    std::cout << "Cache Size: " << stats.cache_size_bytes << " bytes" << std::endl;
    std::cout << "Cache Hit Rate: " << (stats.cache_hit_rate * 100) << "%" << std::endl;
    std::cout << "Total Executions: " << stats.total_executions << std::endl;
    std::cout << "Total Execution Time: " << stats.total_execution_time.count() << " microseconds" << std::endl;
}

void JITCompiler::set_optimization_level(OptimizationLevel level) {
    optimization_level_ = level;
}

void JITCompiler::set_compilation_threshold(uint32_t threshold) {
    compilation_threshold_ = threshold;
}

// Static factory methods for JavaScript binding
Value JITCompiler::create_compiler(Context& ctx, const std::vector<Value>& args) {
    auto compiler = new JITCompiler();
    return Value(static_cast<Object*>(compiler));
}

void JITCompiler::setup_jit_object(Context& ctx) {
    // Set up JIT compiler bindings
    // This would be called during engine initialization
}

} // namespace Quanta