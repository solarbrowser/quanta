/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "../../include/Value.h"
#include <unordered_map>
#include <vector>
#include <memory>

namespace Quanta {

class Function;
class Context;
class ASTNode;

/**
 * Just-In-Time compiler for high-performance JavaScript execution
 */
class JITCompiler {
public:
    enum class OptimizationLevel {
        None,
        Basic,
        Advanced,
        Aggressive
    };

    struct CompilationStats {
        size_t functions_compiled;
        size_t bytecode_generated;
        size_t native_code_generated;
        double compilation_time;
        double execution_speedup;

        CompilationStats() : functions_compiled(0), bytecode_generated(0),
                           native_code_generated(0), compilation_time(0.0), execution_speedup(0.0) {}
    };

private:
    OptimizationLevel optimization_level_;
    std::unordered_map<Function*, void*> compiled_functions_;
    std::unordered_map<Function*, size_t> execution_counts_;
    CompilationStats stats_;
    bool hot_spot_detection_enabled_;
    size_t hot_spot_threshold_;

public:
    JITCompiler();
    ~JITCompiler();

    // Compilation
    bool compile_function(Function* func, OptimizationLevel level = OptimizationLevel::Basic);
    void* get_compiled_code(Function* func);
    bool is_compiled(Function* func) const;

    // Hot-spot detection
    void record_execution(Function* func);
    bool is_hot_spot(Function* func) const;
    void enable_hot_spot_detection(bool enable) { hot_spot_detection_enabled_ = enable; }
    void set_hot_spot_threshold(size_t threshold) { hot_spot_threshold_ = threshold; }

    // Optimization
    void set_optimization_level(OptimizationLevel level) { optimization_level_ = level; }
    void optimize_function(Function* func);
    void deoptimize_function(Function* func);

    // Cache management
    void invalidate_cache();
    void cleanup_unused_code();
    size_t get_cache_size() const;

    // Statistics
    const CompilationStats& get_stats() const { return stats_; }
    void reset_stats();

    // Advanced features
    void enable_profile_guided_optimization(bool enable);
    void enable_adaptive_compilation(bool enable);
    void enable_speculative_optimization(bool enable);

private:
    // Code generation
    void* compile_to_bytecode(Function* func);
    void* compile_to_native(Function* func);
    void apply_optimizations(Function* func, OptimizationLevel level);

    // Analysis
    void analyze_function(Function* func);
    bool should_compile(Function* func) const;
    bool should_optimize(Function* func) const;

    // Memory management
    void allocate_code_memory(size_t size);
    void free_code_memory(void* ptr);
};

} // namespace Quanta