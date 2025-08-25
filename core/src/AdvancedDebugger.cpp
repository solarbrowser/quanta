/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../include/AdvancedDebugger.h"
#include "../include/Value.h"
#include "../include/Context.h"
#include <iostream>
#include <algorithm>
#include <iomanip>
#include <sstream>

namespace Quanta {

//=============================================================================
// ExecutionProfiler Implementation
//=============================================================================

ExecutionProfiler::ExecutionProfiler(size_t max_path_depth) 
    : max_path_depth_(max_path_depth) {
    std::cout << "🔍 EXECUTION PROFILER INITIALIZED" << std::endl;
}

ExecutionProfiler::~ExecutionProfiler() {
    print_profile_summary();
    std::cout << "🔍 EXECUTION PROFILER SHUTDOWN" << std::endl;
}

void ExecutionProfiler::enter_function(const std::string& function_name) {
    if (!profiling_enabled_) return;
    
    std::lock_guard<std::mutex> lock(profile_mutex_);
    
    call_stack_.push_back(function_name);
    time_stack_.push_back(std::chrono::high_resolution_clock::now());
    
    // Record function call
    total_function_calls_++;
    
    // Track hot paths
    if (call_stack_.size() <= max_path_depth_) {
        std::string path;
        for (const auto& func : call_stack_) {
            if (!path.empty()) path += " -> ";
            path += func;
        }
        path_frequencies_[path]++;
    }
}

void ExecutionProfiler::exit_function(const std::string& function_name) {
    if (!profiling_enabled_ || call_stack_.empty()) return;
    
    std::lock_guard<std::mutex> lock(profile_mutex_);
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto start_time = time_stack_.back();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();
    
    // Update profile data
    ProfileData& data = profile_data_[function_name];
    data.function_name = function_name;
    data.call_count++;
    data.total_time_ns += duration;
    data.self_time_ns += duration; // Will be adjusted for child calls
    
    if (duration < data.min_time_ns) data.min_time_ns = duration;
    if (duration > data.max_time_ns) data.max_time_ns = duration;
    
    total_execution_time_ns_ += duration;
    
    call_stack_.pop_back();
    time_stack_.pop_back();
}

void ExecutionProfiler::start_timing(const std::string& label) {
    enter_function(label);
}

void ExecutionProfiler::end_timing(const std::string& label) {
    exit_function(label);
}

const ProfileData* ExecutionProfiler::get_profile_data(const std::string& function_name) const {
    std::lock_guard<std::mutex> lock(profile_mutex_);
    auto it = profile_data_.find(function_name);
    return (it != profile_data_.end()) ? &it->second : nullptr;
}

std::vector<std::pair<std::string, ProfileData>> ExecutionProfiler::get_sorted_by_total_time() const {
    std::lock_guard<std::mutex> lock(profile_mutex_);
    
    std::vector<std::pair<std::string, ProfileData>> sorted_data;
    for (const auto& pair : profile_data_) {
        sorted_data.push_back(pair);
    }
    
    std::sort(sorted_data.begin(), sorted_data.end(),
              [](const auto& a, const auto& b) {
                  return a.second.total_time_ns > b.second.total_time_ns;
              });
    
    return sorted_data;
}

std::vector<std::pair<std::string, ProfileData>> ExecutionProfiler::get_sorted_by_call_count() const {
    std::lock_guard<std::mutex> lock(profile_mutex_);
    
    std::vector<std::pair<std::string, ProfileData>> sorted_data;
    for (const auto& pair : profile_data_) {
        sorted_data.push_back(pair);
    }
    
    std::sort(sorted_data.begin(), sorted_data.end(),
              [](const auto& a, const auto& b) {
                  return a.second.call_count > b.second.call_count;
              });
    
    return sorted_data;
}

std::vector<std::pair<std::string, uint64_t>> ExecutionProfiler::get_hot_paths() const {
    std::lock_guard<std::mutex> lock(profile_mutex_);
    
    std::vector<std::pair<std::string, uint64_t>> sorted_paths;
    for (const auto& pair : path_frequencies_) {
        sorted_paths.push_back(pair);
    }
    
    std::sort(sorted_paths.begin(), sorted_paths.end(),
              [](const auto& a, const auto& b) {
                  return a.second > b.second;
              });
    
    return sorted_paths;
}

void ExecutionProfiler::print_profile_summary() const {
    std::cout << "📊 EXECUTION PROFILE SUMMARY" << std::endl;
    std::cout << "============================" << std::endl;
    std::cout << "Total Function Calls: " << total_function_calls_ << std::endl;
    std::cout << "Total Execution Time: " << (total_execution_time_ns_ / 1000000.0) << " ms" << std::endl;
    std::cout << "Tracked Functions: " << profile_data_.size() << std::endl;
    
    if (total_function_calls_ > 0) {
        std::cout << "Average Call Time: " << (total_execution_time_ns_ / total_function_calls_ / 1000.0) << " μs" << std::endl;
    }
    
    auto top_functions = get_sorted_by_total_time();
    if (!top_functions.empty()) {
        std::cout << "\nTOP FUNCTIONS BY TIME:" << std::endl;
        for (size_t i = 0; i < std::min(size_t(5), top_functions.size()); ++i) {
            const auto& data = top_functions[i].second;
            std::cout << "  " << top_functions[i].first << ": " 
                     << data.get_total_time_ms() << " ms (" 
                     << data.call_count << " calls)" << std::endl;
        }
    }
}

void ExecutionProfiler::print_detailed_profile() const {
    std::cout << "📊 DETAILED EXECUTION PROFILE" << std::endl;
    std::cout << "=============================" << std::endl;
    
    auto sorted_functions = get_sorted_by_total_time();
    
    std::cout << std::left << std::setw(25) << "FUNCTION" 
             << std::setw(10) << "CALLS" 
             << std::setw(12) << "TOTAL(ms)" 
             << std::setw(12) << "AVG(μs)" 
             << std::setw(12) << "MIN(μs)" 
             << std::setw(12) << "MAX(μs)" << std::endl;
    std::cout << std::string(83, '-') << std::endl;
    
    for (const auto& pair : sorted_functions) {
        const auto& data = pair.second;
        std::cout << std::left << std::setw(25) << pair.first
                 << std::setw(10) << data.call_count
                 << std::setw(12) << std::fixed << std::setprecision(3) << data.get_total_time_ms()
                 << std::setw(12) << std::fixed << std::setprecision(1) << data.get_average_time_us()
                 << std::setw(12) << std::fixed << std::setprecision(1) << (data.min_time_ns / 1000.0)
                 << std::setw(12) << std::fixed << std::setprecision(1) << (data.max_time_ns / 1000.0)
                 << std::endl;
    }
}

void ExecutionProfiler::print_hot_paths() const {
    std::cout << "🔥 HOT EXECUTION PATHS" << std::endl;
    std::cout << "=====================" << std::endl;
    
    auto hot_paths = get_hot_paths();
    
    for (size_t i = 0; i < std::min(size_t(10), hot_paths.size()); ++i) {
        std::cout << "  " << (i + 1) << ". " << hot_paths[i].first 
                 << " (frequency: " << hot_paths[i].second << ")" << std::endl;
    }
}

void ExecutionProfiler::clear_profile_data() {
    std::lock_guard<std::mutex> lock(profile_mutex_);
    profile_data_.clear();
    path_frequencies_.clear();
    call_stack_.clear();
    time_stack_.clear();
}

void ExecutionProfiler::reset_statistics() {
    clear_profile_data();
    total_function_calls_ = 0;
    total_execution_time_ns_ = 0;
}

size_t ExecutionProfiler::get_tracked_functions_count() const {
    std::lock_guard<std::mutex> lock(profile_mutex_);
    return profile_data_.size();
}

ExecutionProfiler& ExecutionProfiler::get_instance() {
    static ExecutionProfiler instance;
    return instance;
}

//=============================================================================
// MemoryProfiler Implementation
//=============================================================================

MemoryProfiler::MemoryProfiler() {
    std::cout << "💾 MEMORY PROFILER INITIALIZED" << std::endl;
}

MemoryProfiler::~MemoryProfiler() {
    check_for_leaks();
    print_memory_summary();
    std::cout << "💾 MEMORY PROFILER SHUTDOWN" << std::endl;
}

void MemoryProfiler::track_allocation(void* ptr, size_t size, const std::string& file, int line) {
    if (!tracking_enabled_ || !ptr) return;
    
    std::lock_guard<std::mutex> lock(memory_mutex_);
    
    MemoryAllocation alloc(ptr, size, file, line);
    active_allocations_[ptr] = alloc;
    
    // Update statistics
    stats_.total_allocated_bytes += size;
    stats_.current_allocated_bytes += size;
    stats_.allocation_count++;
    
    if (stats_.current_allocated_bytes > stats_.peak_allocated_bytes) {
        stats_.peak_allocated_bytes = stats_.current_allocated_bytes;
    }
    
    // Update histograms
    size_histogram_[size]++;
    if (!file.empty()) {
        file_allocations_[file]++;
    }
}

void MemoryProfiler::track_deallocation(void* ptr) {
    if (!tracking_enabled_ || !ptr) return;
    
    std::lock_guard<std::mutex> lock(memory_mutex_);
    
    auto it = active_allocations_.find(ptr);
    if (it != active_allocations_.end()) {
        size_t size = it->second.size;
        active_allocations_.erase(it);
        
        // Update statistics
        stats_.total_freed_bytes += size;
        stats_.current_allocated_bytes -= size;
        stats_.deallocation_count++;
    }
}

void MemoryProfiler::check_for_leaks() {
    std::lock_guard<std::mutex> lock(memory_mutex_);
    
    leaked_allocations_.clear();
    for (const auto& pair : active_allocations_) {
        leaked_allocations_.push_back(pair.second);
    }
    
    stats_.leak_count = leaked_allocations_.size();
    
    if (!leaked_allocations_.empty()) {
        std::cout << "⚠️  MEMORY LEAKS DETECTED: " << leaked_allocations_.size() << " leaks" << std::endl;
    }
}

std::vector<MemoryAllocation> MemoryProfiler::get_memory_leaks() const {
    std::lock_guard<std::mutex> lock(memory_mutex_);
    return leaked_allocations_;
}

bool MemoryProfiler::has_memory_leaks() const {
    std::lock_guard<std::mutex> lock(memory_mutex_);
    return !active_allocations_.empty();
}

std::unordered_map<size_t, uint64_t> MemoryProfiler::get_size_histogram() const {
    std::lock_guard<std::mutex> lock(memory_mutex_);
    return size_histogram_;
}

std::unordered_map<std::string, uint64_t> MemoryProfiler::get_file_allocations() const {
    std::lock_guard<std::mutex> lock(memory_mutex_);
    return file_allocations_;
}

void MemoryProfiler::print_memory_summary() const {
    std::cout << "💾 MEMORY PROFILE SUMMARY" << std::endl;
    std::cout << "=========================" << std::endl;
    std::cout << "Total Allocated: " << (stats_.total_allocated_bytes / 1024.0) << " KB" << std::endl;
    std::cout << "Total Freed: " << (stats_.total_freed_bytes / 1024.0) << " KB" << std::endl;
    std::cout << "Current Usage: " << (stats_.current_allocated_bytes / 1024.0) << " KB" << std::endl;
    std::cout << "Peak Usage: " << (stats_.peak_allocated_bytes / 1024.0) << " KB" << std::endl;
    std::cout << "Allocations: " << stats_.allocation_count << std::endl;
    std::cout << "Deallocations: " << stats_.deallocation_count << std::endl;
    std::cout << "Memory Leaks: " << stats_.leak_count << std::endl;
    
    if (stats_.allocation_count > 0) {
        double avg_alloc_size = static_cast<double>(stats_.total_allocated_bytes) / stats_.allocation_count;
        std::cout << "Average Allocation Size: " << avg_alloc_size << " bytes" << std::endl;
    }
}

void MemoryProfiler::print_memory_leaks() const {
    auto leaks = get_memory_leaks();
    
    if (leaks.empty()) {
        std::cout << "✅ NO MEMORY LEAKS DETECTED" << std::endl;
        return;
    }
    
    std::cout << "⚠️  MEMORY LEAKS DETECTED" << std::endl;
    std::cout << "========================" << std::endl;
    
    for (const auto& leak : leaks) {
        std::cout << "  Leak at " << leak.address << ": " << leak.size << " bytes";
        if (!leak.file.empty()) {
            std::cout << " (" << leak.file << ":" << leak.line << ")";
        }
        std::cout << std::endl;
    }
}

void MemoryProfiler::clear_tracking_data() {
    std::lock_guard<std::mutex> lock(memory_mutex_);
    active_allocations_.clear();
    leaked_allocations_.clear();
    size_histogram_.clear();
    file_allocations_.clear();
    stats_ = MemoryStats();
}

MemoryProfiler& MemoryProfiler::get_instance() {
    static MemoryProfiler instance;
    return instance;
}

//=============================================================================
// CallStackAnalyzer Implementation
//=============================================================================

CallStackAnalyzer::CallStackAnalyzer() 
    : max_stack_depth_(0), total_calls_(0) {
    std::cout << "📞 CALL STACK ANALYZER INITIALIZED" << std::endl;
}

CallStackAnalyzer::~CallStackAnalyzer() {
    print_stack_analysis();
    std::cout << "📞 CALL STACK ANALYZER SHUTDOWN" << std::endl;
}

void CallStackAnalyzer::enter_function(const std::string& function_name, const std::string& file, int line) {
    std::lock_guard<std::mutex> lock(stack_mutex_);
    
    auto frame = std::make_unique<CallFrame>(function_name, file, line);
    frame->entry_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();
    
    if (current_stack_.empty()) {
        call_tree_.push_back(std::move(frame));
        current_stack_.push_back(call_tree_.back().get());
    } else {
        CallFrame* parent = current_stack_.back();
        parent->children.push_back(std::move(frame));
        current_stack_.push_back(parent->children.back().get());
    }
    
    // Update statistics
    function_frequencies_[function_name]++;
    total_calls_++;
    
    if (current_stack_.size() > max_stack_depth_) {
        max_stack_depth_ = current_stack_.size();
    }
    
    // Check for recursion
    size_t recursion_depth = 0;
    for (const auto& frame : current_stack_) {
        if (frame->function_name == function_name) {
            recursion_depth++;
        }
    }
    if (recursion_depth > 1) {
        recursion_depths_[function_name] = std::max(recursion_depths_[function_name], recursion_depth);
    }
}

void CallStackAnalyzer::exit_function() {
    std::lock_guard<std::mutex> lock(stack_mutex_);
    
    if (!current_stack_.empty()) {
        CallFrame* frame = current_stack_.back();
        uint64_t exit_time = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count();
        
        frame->self_time_ns = exit_time - frame->entry_time_ns;
        current_stack_.pop_back();
    }
}

std::vector<std::string> CallStackAnalyzer::get_current_call_stack() const {
    std::lock_guard<std::mutex> lock(stack_mutex_);
    
    std::vector<std::string> stack;
    for (const auto& frame : current_stack_) {
        stack.push_back(frame->function_name);
    }
    return stack;
}

void CallStackAnalyzer::print_call_tree() const {
    std::cout << "🌳 CALL TREE ANALYSIS" << std::endl;
    std::cout << "=====================" << std::endl;
    
    std::function<void(const CallFrame*, int)> print_frame = [&](const CallFrame* frame, int depth) {
        std::string indent(depth * 2, ' ');
        std::cout << indent << "├─ " << frame->function_name;
        if (frame->self_time_ns > 0) {
            std::cout << " (" << (frame->self_time_ns / 1000.0) << " μs)";
        }
        std::cout << std::endl;
        
        for (const auto& child : frame->children) {
            print_frame(child.get(), depth + 1);
        }
    };
    
    for (const auto& root : call_tree_) {
        print_frame(root.get(), 0);
    }
}

void CallStackAnalyzer::print_stack_analysis() const {
    std::cout << "📞 CALL STACK ANALYSIS" << std::endl;
    std::cout << "======================" << std::endl;
    std::cout << "Total Function Calls: " << total_calls_ << std::endl;
    std::cout << "Maximum Stack Depth: " << max_stack_depth_ << std::endl;
    std::cout << "Current Stack Depth: " << current_stack_.size() << std::endl;
    std::cout << "Unique Functions: " << function_frequencies_.size() << std::endl;
    
    if (total_calls_ > 0) {
        std::cout << "Average Stack Depth: " << get_average_stack_depth() << std::endl;
    }
}

size_t CallStackAnalyzer::get_current_stack_depth() const {
    std::lock_guard<std::mutex> lock(stack_mutex_);
    return current_stack_.size();
}

double CallStackAnalyzer::get_average_stack_depth() const {
    return total_calls_ > 0 ? static_cast<double>(max_stack_depth_) / total_calls_ : 0.0;
}

CallStackAnalyzer& CallStackAnalyzer::get_instance() {
    static CallStackAnalyzer instance;
    return instance;
}

//=============================================================================
// PerformanceMonitor Implementation
//=============================================================================

PerformanceMonitor::PerformanceMonitor(uint32_t monitoring_interval_ms) 
    : monitoring_interval_ms_(monitoring_interval_ms) {
    start_time_ = std::chrono::high_resolution_clock::now();
    std::cout << " PERFORMANCE MONITOR INITIALIZED" << std::endl;
}

PerformanceMonitor::~PerformanceMonitor() {
    stop_monitoring();
    print_performance_summary();
    std::cout << " PERFORMANCE MONITOR SHUTDOWN" << std::endl;
}

void PerformanceMonitor::start_monitoring() {
    should_stop_monitoring_ = false;
    monitoring_thread_ = std::thread(&PerformanceMonitor::monitoring_loop, this);
    std::cout << "🔍 Real-time performance monitoring started" << std::endl;
}

void PerformanceMonitor::stop_monitoring() {
    should_stop_monitoring_ = true;
    if (monitoring_thread_.joinable()) {
        monitoring_thread_.join();
    }
    std::cout << "🔍 Real-time performance monitoring stopped" << std::endl;
}

void PerformanceMonitor::record_jit_compilation(uint64_t compile_time_ns) {
    if (!monitoring_enabled_) return;
    
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    current_metrics_.jit_compilations++;
    current_metrics_.jit_compile_time_ns += compile_time_ns;
}

void PerformanceMonitor::record_gc_collection(uint64_t gc_time_ns, uint64_t reclaimed_bytes) {
    if (!monitoring_enabled_) return;
    
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    current_metrics_.gc_collections++;
    current_metrics_.gc_time_ns += gc_time_ns;
    current_metrics_.gc_reclaimed_bytes += reclaimed_bytes;
}

void PerformanceMonitor::record_memory_allocation(uint64_t bytes) {
    if (!monitoring_enabled_) return;
    
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    current_metrics_.memory_allocated_bytes += bytes;
    
    uint64_t current_usage = current_metrics_.memory_allocated_bytes - current_metrics_.memory_freed_bytes;
    if (current_usage > current_metrics_.peak_memory_usage_bytes) {
        current_metrics_.peak_memory_usage_bytes = current_usage;
    }
}

void PerformanceMonitor::record_memory_deallocation(uint64_t bytes) {
    if (!monitoring_enabled_) return;
    
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    current_metrics_.memory_freed_bytes += bytes;
}

double PerformanceMonitor::get_cache_hit_ratio() const {
    uint64_t total_accesses = current_metrics_.cache_hits + current_metrics_.cache_misses;
    return total_accesses > 0 ? static_cast<double>(current_metrics_.cache_hits) / total_accesses : 0.0;
}

void PerformanceMonitor::print_performance_summary() const {
    std::cout << " PERFORMANCE SUMMARY" << std::endl;
    std::cout << "=====================" << std::endl;
    std::cout << "Instructions Executed: " << current_metrics_.instructions_executed << std::endl;
    std::cout << "Function Calls: " << current_metrics_.function_calls << std::endl;
    std::cout << "JIT Compilations: " << current_metrics_.jit_compilations << std::endl;
    std::cout << "GC Collections: " << current_metrics_.gc_collections << std::endl;
    std::cout << "Cache Hit Ratio: " << (get_cache_hit_ratio() * 100.0) << "%" << std::endl;
    std::cout << "Peak Memory Usage: " << (current_metrics_.peak_memory_usage_bytes / 1024.0) << " KB" << std::endl;
    std::cout << "Uptime: " << get_uptime_seconds() << " seconds" << std::endl;
}

void PerformanceMonitor::reset_metrics() {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    current_metrics_.reset();
    start_time_ = std::chrono::high_resolution_clock::now();
}

double PerformanceMonitor::get_uptime_seconds() const {
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time_).count();
    return duration / 1000.0;
}

void PerformanceMonitor::monitoring_loop() {
    while (!should_stop_monitoring_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(monitoring_interval_ms_));
        
        if (monitoring_enabled_) {
            snapshot_current_metrics();
        }
    }
}

void PerformanceMonitor::snapshot_current_metrics() {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    historical_metrics_.push_back(current_metrics_);
    
    // Keep only last 100 snapshots
    if (historical_metrics_.size() > 100) {
        historical_metrics_.erase(historical_metrics_.begin());
    }
}

PerformanceMonitor& PerformanceMonitor::get_instance() {
    static PerformanceMonitor instance;
    return instance;
}

//=============================================================================
// Debug Integration Implementation
//=============================================================================

namespace DebugIntegration {

void initialize_debugging_systems() {
    std::cout << "🔧 INITIALIZING ADVANCED DEBUGGING SYSTEMS" << std::endl;
    
    // Initialize all debugging components
    ExecutionProfiler::get_instance();
    MemoryProfiler::get_instance();
    CallStackAnalyzer::get_instance();
    PerformanceMonitor::get_instance();
    
    std::cout << "✅ ALL DEBUGGING SYSTEMS INITIALIZED" << std::endl;
    std::cout << "  📊 Execution Profiler: Ready" << std::endl;
    std::cout << "  💾 Memory Profiler: Ready" << std::endl;
    std::cout << "  📞 Call Stack Analyzer: Ready" << std::endl;
    std::cout << "   Performance Monitor: Ready" << std::endl;
}

void shutdown_debugging_systems() {
    std::cout << "🔧 SHUTTING DOWN DEBUGGING SYSTEMS" << std::endl;
    
    // Print final reports
    print_all_profiles();
    
    std::cout << "✅ ALL DEBUGGING SYSTEMS SHUTDOWN" << std::endl;
}

void start_profiling() {
    ExecutionProfiler::get_instance().enable_profiling();
    MemoryProfiler::get_instance().enable_tracking();
    PerformanceMonitor::get_instance().enable_monitoring();
    
    std::cout << "🔍 PROFILING STARTED" << std::endl;
}

void stop_profiling() {
    ExecutionProfiler::get_instance().disable_profiling();
    MemoryProfiler::get_instance().disable_tracking();
    PerformanceMonitor::get_instance().disable_monitoring();
    
    std::cout << "🔍 PROFILING STOPPED" << std::endl;
}

void print_all_profiles() {
    std::cout << "\n" << std::string(60, '=') << std::endl;
    std::cout << "🔍 COMPREHENSIVE DEBUG REPORT" << std::endl;
    std::cout << std::string(60, '=') << std::endl;
    
    ExecutionProfiler::get_instance().print_detailed_profile();
    std::cout << std::endl;
    
    MemoryProfiler::get_instance().print_memory_summary();
    std::cout << std::endl;
    
    CallStackAnalyzer::get_instance().print_stack_analysis();
    std::cout << std::endl;
    
    PerformanceMonitor::get_instance().print_performance_summary();
    std::cout << std::endl;
}

void check_memory_leaks() {
    MemoryProfiler::get_instance().check_for_leaks();
    MemoryProfiler::get_instance().print_memory_leaks();
}

void print_debug_summary() {
    std::cout << "🔍 DEBUG SYSTEM SUMMARY" << std::endl;
    std::cout << "=======================" << std::endl;
    
    auto& profiler = ExecutionProfiler::get_instance();
    auto& memory = MemoryProfiler::get_instance();
    auto& stack = CallStackAnalyzer::get_instance();
    auto& perf = PerformanceMonitor::get_instance();
    
    std::cout << "📊 Execution Profiler:" << std::endl;
    std::cout << "  Tracked Functions: " << profiler.get_tracked_functions_count() << std::endl;
    std::cout << "  Total Calls: " << profiler.get_total_function_calls() << std::endl;
    
    std::cout << "💾 Memory Profiler:" << std::endl;
    std::cout << "  Memory Leaks: " << (memory.has_memory_leaks() ? "DETECTED" : "NONE") << std::endl;
    
    std::cout << "📞 Call Stack Analyzer:" << std::endl;
    std::cout << "  Max Stack Depth: " << stack.get_max_stack_depth() << std::endl;
    std::cout << "  Total Calls: " << stack.get_total_calls() << std::endl;
    
    std::cout << " Performance Monitor:" << std::endl;
    std::cout << "  Uptime: " << perf.get_uptime_seconds() << " seconds" << std::endl;
}

} // namespace DebugIntegration

} // namespace Quanta