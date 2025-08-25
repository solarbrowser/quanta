/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <vector>
#include <unordered_map>
#include <memory>
#include <string>
#include <chrono>
#include <fstream>
#include <atomic>
#include <thread>
#include <functional>

namespace Quanta {

// Forward declarations
class Value;
class Context;

//=============================================================================
// Advanced Debugging and Profiling Tools
//
// Comprehensive debugging and profiling system for ultra-high performance:
// - Real-time execution profiling
// - Memory usage tracking and leak detection
// - Call stack analysis and optimization
// - Performance bottleneck identification
// - Interactive debugging interface
// - Code coverage analysis
// - Hot path identification
// - Garbage collection profiling
//=============================================================================

//=============================================================================
// Execution Profiler - Real-time performance analysis
//=============================================================================

struct ProfileData {
    std::string function_name;
    uint64_t call_count;
    uint64_t total_time_ns;
    uint64_t min_time_ns;
    uint64_t max_time_ns;
    uint64_t self_time_ns;        // Time excluding child calls
    std::vector<std::string> hot_paths;
    
    ProfileData() : call_count(0), total_time_ns(0), min_time_ns(UINT64_MAX), 
                   max_time_ns(0), self_time_ns(0) {}
    
    double get_average_time_us() const {
        return call_count > 0 ? (total_time_ns / 1000.0 / call_count) : 0.0;
    }
    
    double get_total_time_ms() const {
        return total_time_ns / 1000000.0;
    }
};

class ExecutionProfiler {
private:
    std::unordered_map<std::string, ProfileData> profile_data_;
    std::vector<std::string> call_stack_;
    std::vector<std::chrono::high_resolution_clock::time_point> time_stack_;
    
    // Performance tracking
    std::atomic<uint64_t> total_function_calls_{0};
    std::atomic<uint64_t> total_execution_time_ns_{0};
    std::atomic<bool> profiling_enabled_{true};
    
    // Hot path detection
    std::unordered_map<std::string, uint64_t> path_frequencies_;
    size_t max_path_depth_;
    
    mutable std::mutex profile_mutex_;

public:
    ExecutionProfiler(size_t max_path_depth = 10);
    ~ExecutionProfiler();
    
    // Profiling control
    void enable_profiling() { profiling_enabled_ = true; }
    void disable_profiling() { profiling_enabled_ = false; }
    bool is_profiling_enabled() const { return profiling_enabled_; }
    
    // Function entry/exit tracking
    void enter_function(const std::string& function_name);
    void exit_function(const std::string& function_name);
    
    // Manual timing
    void start_timing(const std::string& label);
    void end_timing(const std::string& label);
    
    // Data access
    const ProfileData* get_profile_data(const std::string& function_name) const;
    std::vector<std::pair<std::string, ProfileData>> get_sorted_by_total_time() const;
    std::vector<std::pair<std::string, ProfileData>> get_sorted_by_call_count() const;
    std::vector<std::pair<std::string, uint64_t>> get_hot_paths() const;
    
    // Reporting
    void print_profile_summary() const;
    void print_detailed_profile() const;
    void print_hot_paths() const;
    void export_profile_data(const std::string& filename) const;
    
    // Statistics
    uint64_t get_total_function_calls() const { return total_function_calls_; }
    double get_total_execution_time_ms() const { return total_execution_time_ns_ / 1000000.0; }
    size_t get_tracked_functions_count() const;
    
    // Reset
    void clear_profile_data();
    void reset_statistics();
    
    // Singleton access
    static ExecutionProfiler& get_instance();
};

//=============================================================================
// Memory Profiler - Advanced memory tracking and leak detection
//=============================================================================

struct MemoryAllocation {
    void* address;
    size_t size;
    std::string file;
    int line;
    std::chrono::high_resolution_clock::time_point timestamp;
    std::vector<std::string> call_stack;
    
    MemoryAllocation(void* addr, size_t sz, const std::string& f, int l)
        : address(addr), size(sz), file(f), line(l), 
          timestamp(std::chrono::high_resolution_clock::now()) {}
};

struct MemoryStats {
    uint64_t total_allocated_bytes;
    uint64_t total_freed_bytes;
    uint64_t current_allocated_bytes;
    uint64_t peak_allocated_bytes;
    uint64_t allocation_count;
    uint64_t deallocation_count;
    uint64_t leak_count;
    
    MemoryStats() : total_allocated_bytes(0), total_freed_bytes(0),
                   current_allocated_bytes(0), peak_allocated_bytes(0),
                   allocation_count(0), deallocation_count(0), leak_count(0) {}
};

class MemoryProfiler {
private:
    std::unordered_map<void*, MemoryAllocation> active_allocations_;
    std::vector<MemoryAllocation> leaked_allocations_;
    MemoryStats stats_;
    
    std::atomic<bool> tracking_enabled_{true};
    mutable std::mutex memory_mutex_;
    
    // Allocation size tracking
    std::unordered_map<size_t, uint64_t> size_histogram_;
    std::unordered_map<std::string, uint64_t> file_allocations_;

public:
    MemoryProfiler();
    ~MemoryProfiler();
    
    // Memory tracking control
    void enable_tracking() { tracking_enabled_ = true; }
    void disable_tracking() { tracking_enabled_ = false; }
    bool is_tracking_enabled() const { return tracking_enabled_; }
    
    // Allocation tracking
    void track_allocation(void* ptr, size_t size, const std::string& file = "", int line = 0);
    void track_deallocation(void* ptr);
    
    // Leak detection
    void check_for_leaks();
    std::vector<MemoryAllocation> get_memory_leaks() const;
    bool has_memory_leaks() const;
    
    // Statistics
    const MemoryStats& get_memory_stats() const { return stats_; }
    std::unordered_map<size_t, uint64_t> get_size_histogram() const;
    std::unordered_map<std::string, uint64_t> get_file_allocations() const;
    
    // Reporting
    void print_memory_summary() const;
    void print_allocation_histogram() const;
    void print_memory_leaks() const;
    void export_memory_report(const std::string& filename) const;
    
    // Utilities
    void clear_tracking_data();
    double get_fragmentation_ratio() const;
    size_t get_largest_allocation_size() const;
    
    // Singleton access
    static MemoryProfiler& get_instance();
};

//=============================================================================
// Call Stack Analyzer - Advanced call stack profiling
//=============================================================================

struct CallFrame {
    std::string function_name;
    std::string file_name;
    int line_number;
    uint64_t entry_time_ns;
    uint64_t self_time_ns;
    std::vector<std::unique_ptr<CallFrame>> children;
    
    CallFrame(const std::string& func, const std::string& file, int line)
        : function_name(func), file_name(file), line_number(line),
          entry_time_ns(0), self_time_ns(0) {}
};

class CallStackAnalyzer {
private:
    std::vector<std::unique_ptr<CallFrame>> call_tree_;
    std::vector<CallFrame*> current_stack_;
    
    // Statistics
    std::unordered_map<std::string, uint64_t> function_frequencies_;
    std::unordered_map<std::string, uint64_t> recursion_depths_;
    uint64_t max_stack_depth_;
    uint64_t total_calls_;
    
    mutable std::mutex stack_mutex_;

public:
    CallStackAnalyzer();
    ~CallStackAnalyzer();
    
    // Stack tracking
    void enter_function(const std::string& function_name, const std::string& file = "", int line = 0);
    void exit_function();
    
    // Analysis
    std::vector<std::string> get_current_call_stack() const;
    std::vector<std::pair<std::string, uint64_t>> get_function_frequencies() const;
    std::vector<std::pair<std::string, uint64_t>> get_recursion_analysis() const;
    
    // Statistics
    uint64_t get_max_stack_depth() const { return max_stack_depth_; }
    uint64_t get_total_calls() const { return total_calls_; }
    size_t get_current_stack_depth() const;
    
    // Reporting
    void print_call_tree() const;
    void print_stack_analysis() const;
    void print_current_stack() const;
    void export_call_tree(const std::string& filename) const;
    
    // Utilities
    void clear_call_data();
    bool is_recursive_call(const std::string& function_name) const;
    double get_average_stack_depth() const;
    
    // Singleton access
    static CallStackAnalyzer& get_instance();
};

//=============================================================================
// Performance Monitor - Real-time performance tracking
//=============================================================================

struct PerformanceMetrics {
    // Execution metrics
    uint64_t instructions_executed;
    uint64_t function_calls;
    uint64_t jit_compilations;
    uint64_t gc_collections;
    
    // Timing metrics
    uint64_t total_execution_time_ns;
    uint64_t jit_compile_time_ns;
    uint64_t gc_time_ns;
    uint64_t parse_time_ns;
    
    // Memory metrics
    uint64_t memory_allocated_bytes;
    uint64_t memory_freed_bytes;
    uint64_t peak_memory_usage_bytes;
    uint64_t gc_reclaimed_bytes;
    
    // Cache metrics
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint64_t inline_cache_hits;
    uint64_t inline_cache_misses;
    
    PerformanceMetrics() { reset(); }
    
    void reset() {
        instructions_executed = 0;
        function_calls = 0;
        jit_compilations = 0;
        gc_collections = 0;
        total_execution_time_ns = 0;
        jit_compile_time_ns = 0;
        gc_time_ns = 0;
        parse_time_ns = 0;
        memory_allocated_bytes = 0;
        memory_freed_bytes = 0;
        peak_memory_usage_bytes = 0;
        gc_reclaimed_bytes = 0;
        cache_hits = 0;
        cache_misses = 0;
        inline_cache_hits = 0;
        inline_cache_misses = 0;
    }
};

class PerformanceMonitor {
private:
    PerformanceMetrics current_metrics_;
    std::vector<PerformanceMetrics> historical_metrics_;
    
    std::chrono::high_resolution_clock::time_point start_time_;
    std::atomic<bool> monitoring_enabled_{true};
    
    // Real-time monitoring
    std::thread monitoring_thread_;
    std::atomic<bool> should_stop_monitoring_{false};
    uint32_t monitoring_interval_ms_;
    
    mutable std::mutex metrics_mutex_;

public:
    PerformanceMonitor(uint32_t monitoring_interval_ms = 100);
    ~PerformanceMonitor();
    
    // Monitoring control
    void start_monitoring();
    void stop_monitoring();
    void enable_monitoring() { monitoring_enabled_ = true; }
    void disable_monitoring() { monitoring_enabled_ = false; }
    
    // Metric recording
    void record_instruction() { if (monitoring_enabled_) current_metrics_.instructions_executed++; }
    void record_function_call() { if (monitoring_enabled_) current_metrics_.function_calls++; }
    void record_jit_compilation(uint64_t compile_time_ns);
    void record_gc_collection(uint64_t gc_time_ns, uint64_t reclaimed_bytes);
    void record_memory_allocation(uint64_t bytes);
    void record_memory_deallocation(uint64_t bytes);
    void record_cache_hit() { if (monitoring_enabled_) current_metrics_.cache_hits++; }
    void record_cache_miss() { if (monitoring_enabled_) current_metrics_.cache_misses++; }
    
    // Data access
    const PerformanceMetrics& get_current_metrics() const { return current_metrics_; }
    std::vector<PerformanceMetrics> get_historical_metrics() const;
    
    // Analysis
    double get_cache_hit_ratio() const;
    double get_average_function_call_time_us() const;
    double get_gc_overhead_percentage() const;
    double get_jit_overhead_percentage() const;
    uint64_t get_instructions_per_second() const;
    
    // Reporting
    void print_performance_summary() const;
    void print_detailed_metrics() const;
    void print_historical_analysis() const;
    void export_performance_data(const std::string& filename) const;
    
    // Utilities
    void reset_metrics();
    void snapshot_current_metrics();
    double get_uptime_seconds() const;
    
    // Singleton access
    static PerformanceMonitor& get_instance();

private:
    void monitoring_loop();
};

//=============================================================================
// Interactive Debugger - Runtime debugging interface
//=============================================================================

enum class DebugCommand {
    CONTINUE,
    STEP_OVER,
    STEP_INTO,
    STEP_OUT,
    SET_BREAKPOINT,
    REMOVE_BREAKPOINT,
    PRINT_VARIABLE,
    PRINT_STACK,
    PRINT_LOCALS,
    EVALUATE_EXPRESSION,
    QUIT
};

struct Breakpoint {
    std::string file;
    int line;
    bool enabled;
    std::string condition;
    uint64_t hit_count;
    
    Breakpoint(const std::string& f, int l) 
        : file(f), line(l), enabled(true), hit_count(0) {}
};

class InteractiveDebugger {
private:
    std::vector<Breakpoint> breakpoints_;
    bool debug_mode_enabled_;
    bool stepping_mode_;
    Context* current_context_;
    
    std::atomic<bool> breakpoint_hit_{false};
    std::string current_file_;
    int current_line_;

public:
    InteractiveDebugger();
    ~InteractiveDebugger();
    
    // Debug control
    void enable_debug_mode() { debug_mode_enabled_ = true; }
    void disable_debug_mode() { debug_mode_enabled_ = false; }
    bool is_debug_mode_enabled() const { return debug_mode_enabled_; }
    
    // Breakpoint management
    void set_breakpoint(const std::string& file, int line, const std::string& condition = "");
    void remove_breakpoint(const std::string& file, int line);
    void enable_breakpoint(const std::string& file, int line);
    void disable_breakpoint(const std::string& file, int line);
    void list_breakpoints() const;
    
    // Execution control
    bool check_breakpoint(const std::string& file, int line);
    void handle_breakpoint_hit(Context* context, const std::string& file, int line);
    DebugCommand wait_for_command();
    
    // Variable inspection
    void print_variable(const std::string& name, Context* context);
    void print_call_stack(Context* context);
    void print_local_variables(Context* context);
    Value evaluate_expression(const std::string& expression, Context* context);
    
    // Stepping
    void step_over() { stepping_mode_ = true; }
    void step_into() { stepping_mode_ = true; }
    void step_out() { stepping_mode_ = true; }
    
    // Utilities
    void print_debug_help() const;
    void clear_all_breakpoints();
    
    // Singleton access
    static InteractiveDebugger& get_instance();
};

//=============================================================================
// Code Coverage Analyzer - Code coverage and hot path analysis
//=============================================================================

struct CoverageData {
    std::string file;
    int line;
    uint64_t hit_count;
    bool is_hot_path;
    
    CoverageData(const std::string& f, int l) 
        : file(f), line(l), hit_count(0), is_hot_path(false) {}
};

class CodeCoverageAnalyzer {
private:
    std::unordered_map<std::string, std::unordered_map<int, CoverageData>> coverage_data_;
    std::atomic<bool> coverage_enabled_{true};
    uint64_t hot_path_threshold_;
    
    mutable std::mutex coverage_mutex_;

public:
    CodeCoverageAnalyzer(uint64_t hot_path_threshold = 1000);
    ~CodeCoverageAnalyzer();
    
    // Coverage control
    void enable_coverage() { coverage_enabled_ = true; }
    void disable_coverage() { coverage_enabled_ = false; }
    bool is_coverage_enabled() const { return coverage_enabled_; }
    
    // Coverage tracking
    void record_line_execution(const std::string& file, int line);
    void mark_hot_paths();
    
    // Analysis
    double get_coverage_percentage(const std::string& file) const;
    std::vector<std::pair<std::string, int>> get_uncovered_lines(const std::string& file) const;
    std::vector<std::pair<std::string, int>> get_hot_paths() const;
    std::vector<std::pair<std::string, uint64_t>> get_line_frequencies(const std::string& file) const;
    
    // Reporting
    void print_coverage_summary() const;
    void print_file_coverage(const std::string& file) const;
    void print_hot_path_analysis() const;
    void export_coverage_report(const std::string& filename) const;
    
    // Utilities
    void clear_coverage_data();
    void set_hot_path_threshold(uint64_t threshold) { hot_path_threshold_ = threshold; }
    size_t get_total_tracked_lines() const;
    
    // Singleton access
    static CodeCoverageAnalyzer& get_instance();
};

//=============================================================================
// Debug Integration - Main debugging interface
//=============================================================================

namespace DebugIntegration {
    // Initialize debugging systems
    void initialize_debugging_systems();
    void shutdown_debugging_systems();
    
    // Profiling control
    void start_profiling();
    void stop_profiling();
    void print_all_profiles();
    void export_all_debug_data(const std::string& directory);
    
    // Memory debugging
    void check_memory_leaks();
    void print_memory_report();
    
    // Performance monitoring
    void start_performance_monitoring();
    void stop_performance_monitoring();
    void print_performance_report();
    
    // Interactive debugging
    void enter_debug_mode();
    void exit_debug_mode();
    
    // Coverage analysis
    void enable_code_coverage();
    void disable_code_coverage();
    void print_coverage_report();
    
    // Utility functions
    void print_debug_summary();
    void reset_all_debug_data();
    void configure_debug_options();
}

//=============================================================================
// RAII Debug Helpers - Automatic profiling and timing
//=============================================================================

class ScopedProfiler {
private:
    std::string function_name_;
    
public:
    ScopedProfiler(const std::string& function_name) : function_name_(function_name) {
        ExecutionProfiler::get_instance().enter_function(function_name_);
    }
    
    ~ScopedProfiler() {
        ExecutionProfiler::get_instance().exit_function(function_name_);
    }
};

class ScopedTimer {
private:
    std::string label_;
    std::chrono::high_resolution_clock::time_point start_time_;
    
public:
    ScopedTimer(const std::string& label) : label_(label) {
        start_time_ = std::chrono::high_resolution_clock::now();
    }
    
    ~ScopedTimer() {
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time_).count();
        printf("⏱️  %s: %.1f μs\n", label_.c_str(), duration);
    }
};

// Macros for easy debugging
#define PROFILE_FUNCTION() ScopedProfiler _prof(__FUNCTION__)
#define PROFILE_SCOPE(name) ScopedProfiler _prof(name)
#define TIME_SCOPE(label) ScopedTimer _timer(label)
#define RECORD_MEMORY_ALLOC(ptr, size) MemoryProfiler::get_instance().track_allocation(ptr, size, __FILE__, __LINE__)
#define RECORD_MEMORY_FREE(ptr) MemoryProfiler::get_instance().track_deallocation(ptr)
#define RECORD_LINE_COVERAGE() CodeCoverageAnalyzer::get_instance().record_line_execution(__FILE__, __LINE__)

} // namespace Quanta