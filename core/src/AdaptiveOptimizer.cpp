/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../include/AdaptiveOptimizer.h"
#include <iostream>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <powerbase.h>
#elif defined(__linux__)
#include <unistd.h>
#include <cstdio>
#endif

namespace Quanta {

//=============================================================================
// RealTimePerformanceMonitor Implementation
//=============================================================================

RealTimePerformanceMonitor::RealTimePerformanceMonitor() {
    start_time_ = std::chrono::high_resolution_clock::now();
    std::cout << " REAL-TIME PERFORMANCE MONITOR INITIALIZED" << std::endl;
}

RealTimePerformanceMonitor::~RealTimePerformanceMonitor() {
    stop_monitoring();
    print_real_time_stats();
    std::cout << " REAL-TIME PERFORMANCE MONITOR SHUTDOWN" << std::endl;
}

void RealTimePerformanceMonitor::start_monitoring() {
    if (!should_stop_) return; // Already running
    
    should_stop_ = false;
    monitoring_thread_ = std::thread(&RealTimePerformanceMonitor::monitoring_loop, this);
    std::cout << "🔍 Real-time performance monitoring started" << std::endl;
}

void RealTimePerformanceMonitor::stop_monitoring() {
    should_stop_ = true;
    if (monitoring_thread_.joinable()) {
        monitoring_thread_.join();
    }
    std::cout << "🔍 Real-time performance monitoring stopped" << std::endl;
}

void RealTimePerformanceMonitor::record_jit_compilation(uint64_t compile_time_ns) {
    if (!monitoring_enabled_) return;
    
    current_metrics_.total_jit_compilations++;
    current_metrics_.jit_compile_time_ns += compile_time_ns;
}

void RealTimePerformanceMonitor::record_gc_collection(uint64_t gc_time_ns) {
    if (!monitoring_enabled_) return;
    
    current_metrics_.gc_collections++;
    current_metrics_.gc_time_ns += gc_time_ns;
}

void RealTimePerformanceMonitor::record_memory_allocation(uint64_t bytes) {
    if (!monitoring_enabled_) return;
    
    current_metrics_.allocated_objects++;
    current_metrics_.used_heap_bytes += bytes;
}

void RealTimePerformanceMonitor::record_cache_access(bool l1_hit, bool l2_hit, bool l3_hit) {
    if (!monitoring_enabled_) return;
    
    if (l1_hit) {
        current_metrics_.l1_cache_hits++;
    } else {
        current_metrics_.l1_cache_misses++;
        
        if (l2_hit) {
            current_metrics_.l2_cache_hits++;
        } else {
            current_metrics_.l2_cache_misses++;
            
            if (l3_hit) {
                current_metrics_.l3_cache_hits++;
            } else {
                current_metrics_.l3_cache_misses++;
            }
        }
    }
}

void RealTimePerformanceMonitor::record_branch_prediction(bool correct) {
    if (!monitoring_enabled_) return;
    
    current_metrics_.branch_predictions++;
    if (!correct) {
        current_metrics_.branch_mispredictions++;
    }
}

void RealTimePerformanceMonitor::update_system_metrics() {
    collect_system_metrics();
}

double RealTimePerformanceMonitor::get_instructions_per_second() const {
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count();
    
    return duration > 0 ? static_cast<double>(current_metrics_.total_instructions) / duration : 0.0;
}

double RealTimePerformanceMonitor::get_cache_hit_ratio() const {
    uint64_t total_hits = current_metrics_.l1_cache_hits + current_metrics_.l2_cache_hits + current_metrics_.l3_cache_hits;
    uint64_t total_misses = current_metrics_.l1_cache_misses + current_metrics_.l2_cache_misses + current_metrics_.l3_cache_misses;
    uint64_t total_accesses = total_hits + total_misses;
    
    return total_accesses > 0 ? static_cast<double>(total_hits) / total_accesses : 0.0;
}

double RealTimePerformanceMonitor::get_branch_prediction_accuracy() const {
    if (current_metrics_.branch_predictions == 0) return 0.0;
    
    uint64_t correct_predictions = current_metrics_.branch_predictions - current_metrics_.branch_mispredictions;
    return static_cast<double>(correct_predictions) / current_metrics_.branch_predictions;
}

double RealTimePerformanceMonitor::get_jit_compilation_rate() const {
    return current_metrics_.total_function_calls > 0 ? 
           static_cast<double>(current_metrics_.total_jit_compilations) / current_metrics_.total_function_calls : 0.0;
}

double RealTimePerformanceMonitor::get_gc_overhead_percentage() const {
    return current_metrics_.total_execution_time_ns > 0 ?
           (static_cast<double>(current_metrics_.gc_time_ns) / current_metrics_.total_execution_time_ns) * 100.0 : 0.0;
}

double RealTimePerformanceMonitor::get_memory_utilization() const {
    return current_metrics_.heap_size_bytes > 0 ?
           static_cast<double>(current_metrics_.used_heap_bytes) / current_metrics_.heap_size_bytes : 0.0;
}

void RealTimePerformanceMonitor::print_real_time_stats() const {
    std::cout << " REAL-TIME PERFORMANCE STATISTICS" << std::endl;
    std::cout << "===================================" << std::endl;
    std::cout << "Instructions: " << current_metrics_.total_instructions << std::endl;
    std::cout << "Function Calls: " << current_metrics_.total_function_calls << std::endl;
    std::cout << "JIT Compilations: " << current_metrics_.total_jit_compilations << std::endl;
    std::cout << "GC Collections: " << current_metrics_.gc_collections << std::endl;
    std::cout << "Instructions/sec: " << static_cast<uint64_t>(get_instructions_per_second()) << std::endl;
    std::cout << "Cache Hit Ratio: " << (get_cache_hit_ratio() * 100.0) << "%" << std::endl;
    std::cout << "Branch Accuracy: " << (get_branch_prediction_accuracy() * 100.0) << "%" << std::endl;
    std::cout << "GC Overhead: " << get_gc_overhead_percentage() << "%" << std::endl;
    std::cout << "Memory Usage: " << (get_memory_utilization() * 100.0) << "%" << std::endl;
    std::cout << "CPU Usage: " << current_metrics_.cpu_usage_percent << "%" << std::endl;
    std::cout << "CPU Temperature: " << current_metrics_.cpu_temperature_celsius << "°C" << std::endl;
    std::cout << "Thermal Throttling: " << (current_metrics_.thermal_throttling ? "YES" : "NO") << std::endl;
    std::cout << "Battery Powered: " << (current_metrics_.battery_powered ? "YES" : "NO") << std::endl;
    if (current_metrics_.battery_powered) {
        std::cout << "Battery Level: " << current_metrics_.battery_level_percent << "%" << std::endl;
    }
}

void RealTimePerformanceMonitor::monitoring_loop() {
    while (!should_stop_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(monitoring_interval_ms_));
        
        if (monitoring_enabled_) {
            update_system_metrics();
            check_thresholds();
            
            // Take snapshot periodically
            static auto last_snapshot = std::chrono::high_resolution_clock::now();
            auto now = std::chrono::high_resolution_clock::now();
            auto snapshot_interval = std::chrono::milliseconds(snapshot_interval_ms_);
            
            if (now - last_snapshot >= snapshot_interval) {
                take_snapshot();
                last_snapshot = now;
            }
        }
    }
}

void RealTimePerformanceMonitor::take_snapshot() {
    std::lock_guard<std::mutex> lock(analysis_mutex_);
    historical_snapshots_.push_back(current_metrics_);
    
    // Keep only last 100 snapshots
    if (historical_snapshots_.size() > 100) {
        historical_snapshots_.erase(historical_snapshots_.begin());
    }
}

void RealTimePerformanceMonitor::check_thresholds() {
    // Check various performance thresholds and trigger callbacks
    for (auto& [metric, callback] : threshold_callbacks_) {
        // Simplified threshold checking - would be more sophisticated in real implementation
        if (metric == "cpu_temperature" && current_metrics_.cpu_temperature_celsius > 80) {
            callback();
        } else if (metric == "memory_usage" && get_memory_utilization() > 0.9) {
            callback();
        } else if (metric == "gc_overhead" && get_gc_overhead_percentage() > 20.0) {
            callback();
        }
    }
}

void RealTimePerformanceMonitor::collect_system_metrics() {
    // Real system metrics collection
#ifdef _WIN32
    // Get CPU usage
    static FILETIME prev_idle_time = {0}, prev_kernel_time = {0}, prev_user_time = {0};
    FILETIME idle_time, kernel_time, user_time;
    
    if (GetSystemTimes(&idle_time, &kernel_time, &user_time)) {
        auto file_time_to_uint64 = [](const FILETIME& ft) -> uint64_t {
            return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        };
        
        uint64_t idle_diff = file_time_to_uint64(idle_time) - file_time_to_uint64(prev_idle_time);
        uint64_t kernel_diff = file_time_to_uint64(kernel_time) - file_time_to_uint64(prev_kernel_time);
        uint64_t user_diff = file_time_to_uint64(user_time) - file_time_to_uint64(prev_user_time);
        
        uint64_t sys_diff = kernel_diff + user_diff;
        if (sys_diff > 0) {
            current_metrics_.cpu_usage_percent = static_cast<uint32_t>(100 * (sys_diff - idle_diff) / sys_diff);
        }
        
        prev_idle_time = idle_time;
        prev_kernel_time = kernel_time;
        prev_user_time = user_time;
    }
    
    // Get memory usage
    MEMORYSTATUSEX mem_status = {0};
    mem_status.dwLength = sizeof(mem_status);
    if (GlobalMemoryStatusEx(&mem_status)) {
        current_metrics_.memory_usage_percent = mem_status.dwMemoryLoad;
    }
    
    // Check power status
    SYSTEM_POWER_STATUS power_status;
    if (GetSystemPowerStatus(&power_status)) {
        current_metrics_.battery_powered = (power_status.ACLineStatus == 0);
        if (current_metrics_.battery_powered && power_status.BatteryLifePercent != 255) {
            current_metrics_.battery_level_percent = power_status.BatteryLifePercent;
        } else {
            current_metrics_.battery_level_percent = 100;
        }
    }
    
#elif defined(__linux__)
    // Read CPU usage from /proc/stat
    std::ifstream stat_file("/proc/stat");
    if (stat_file.is_open()) {
        std::string line;
        std::getline(stat_file, line);
        
        static uint64_t prev_total = 0, prev_idle = 0;
        
        std::istringstream iss(line);
        std::string cpu;
        uint64_t user, nice, system, idle, iowait, irq, softirq, steal;
        
        if (iss >> cpu >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal) {
            uint64_t total = user + nice + system + idle + iowait + irq + softirq + steal;
            uint64_t total_diff = total - prev_total;
            uint64_t idle_diff = idle - prev_idle;
            
            if (total_diff > 0) {
                current_metrics_.cpu_usage_percent = static_cast<uint32_t>(100 * (total_diff - idle_diff) / total_diff);
            }
            
            prev_total = total;
            prev_idle = idle;
        }
    }
    
    // Read memory usage from /proc/meminfo
    std::ifstream meminfo("/proc/meminfo");
    if (meminfo.is_open()) {
        std::string line;
        uint64_t mem_total = 0, mem_available = 0;
        
        while (std::getline(meminfo, line)) {
            if (line.find("MemTotal:") == 0) {
                std::sscanf(line.c_str(), "MemTotal: %llu kB", &mem_total);
            } else if (line.find("MemAvailable:") == 0) {
                std::sscanf(line.c_str(), "MemAvailable: %llu kB", &mem_available);
                break;
            }
        }
        
        if (mem_total > 0) {
            current_metrics_.memory_usage_percent = static_cast<uint32_t>(100 * (mem_total - mem_available) / mem_total);
        }
    }
    
    // Check for battery
    std::ifstream power_supply("/sys/class/power_supply/BAT0/status");
    if (power_supply.is_open()) {
        std::string status;
        power_supply >> status;
        current_metrics_.battery_powered = (status != "Unknown");
        
        if (current_metrics_.battery_powered) {
            std::ifstream capacity("/sys/class/power_supply/BAT0/capacity");
            if (capacity.is_open()) {
                capacity >> current_metrics_.battery_level_percent;
            }
        }
    } else {
        current_metrics_.battery_powered = false;
        current_metrics_.battery_level_percent = 100;
    }
    
#else
    // Fallback: minimal real metrics
    current_metrics_.cpu_usage_percent = 25;  // Conservative estimate
    current_metrics_.memory_usage_percent = 50;
    current_metrics_.battery_powered = false;
    current_metrics_.battery_level_percent = 100;
#endif

    // CPU temperature detection (platform-specific)
    current_metrics_.cpu_temperature_celsius = get_cpu_temperature();
    current_metrics_.thermal_throttling = current_metrics_.cpu_temperature_celsius > 85;
}

RealTimePerformanceMonitor& RealTimePerformanceMonitor::get_instance() {
    static RealTimePerformanceMonitor instance;
    return instance;
}

//=============================================================================
// AdaptiveOptimizer Implementation
//=============================================================================

AdaptiveOptimizer::AdaptiveOptimizer() 
    : current_strategy_(OptimizationStrategy::BALANCED),
      current_level_(OptimizationLevel::BASIC) {
    
    current_thresholds_.set_balanced_thresholds();
    std::cout << "🧠 ADAPTIVE OPTIMIZER INITIALIZED" << std::endl;
}

AdaptiveOptimizer::~AdaptiveOptimizer() {
    stop_adaptive_optimization();
    print_optimization_summary();
    std::cout << "🧠 ADAPTIVE OPTIMIZER SHUTDOWN" << std::endl;
}

void AdaptiveOptimizer::start_adaptive_optimization() {
    if (!should_stop_adaptation_) return; // Already running
    
    should_stop_adaptation_ = false;
    adaptation_thread_ = std::thread(&AdaptiveOptimizer::adaptation_loop, this);
    std::cout << "🔄 Adaptive optimization started" << std::endl;
}

void AdaptiveOptimizer::stop_adaptive_optimization() {
    should_stop_adaptation_ = true;
    if (adaptation_thread_.joinable()) {
        adaptation_thread_.join();
    }
    std::cout << "🔄 Adaptive optimization stopped" << std::endl;
}

void AdaptiveOptimizer::set_optimization_strategy(OptimizationStrategy strategy) {
    std::lock_guard<std::mutex> lock(optimization_mutex_);
    
    OptimizationStrategy old_strategy = current_strategy_;
    current_strategy_ = strategy;
    
    // Update thresholds based on strategy
    switch (strategy) {
        case OptimizationStrategy::PERFORMANCE_FIRST:
            current_thresholds_.set_performance_thresholds();
            break;
        case OptimizationStrategy::EFFICIENCY_FIRST:
        case OptimizationStrategy::BATTERY_SAVER:
            current_thresholds_.set_efficiency_thresholds();
            break;
        default:
            current_thresholds_.set_balanced_thresholds();
            break;
    }
    
    std::cout << "🔄 Optimization strategy changed: " << static_cast<int>(old_strategy) 
             << " -> " << static_cast<int>(strategy) << std::endl;
}

void AdaptiveOptimizer::set_optimization_level(OptimizationLevel level) {
    std::lock_guard<std::mutex> lock(optimization_mutex_);
    
    OptimizationLevel old_level = current_level_;
    current_level_ = level;
    
    std::cout << "🔄 Optimization level changed: " << static_cast<int>(old_level) 
             << " -> " << static_cast<int>(level) << std::endl;
}

OptimizationStrategy AdaptiveOptimizer::recommend_strategy(const RuntimeMetrics& metrics) const {
    // Intelligent strategy recommendation based on current metrics
    
    // Check thermal conditions
    if (metrics.thermal_throttling || metrics.cpu_temperature_celsius > 75) {
        return OptimizationStrategy::THERMAL_AWARE;
    }
    
    // Check battery conditions
    if (metrics.battery_powered && metrics.battery_level_percent < 20) {
        return OptimizationStrategy::BATTERY_SAVER;
    }
    
    // Check memory pressure
    double memory_usage = metrics.heap_size_bytes > 0 ? 
                         static_cast<double>(metrics.used_heap_bytes) / metrics.heap_size_bytes : 0.0;
    if (memory_usage > 0.85) {
        return OptimizationStrategy::MEMORY_CONSTRAINED;
    }
    
    // Check computational intensity
    double jit_ratio = metrics.total_function_calls > 0 ?
                      static_cast<double>(metrics.total_jit_compilations) / metrics.total_function_calls : 0.0;
    if (jit_ratio > 0.5) {
        return OptimizationStrategy::COMPUTE_INTENSIVE;
    }
    
    // Check efficiency vs performance needs
    if (metrics.cpu_usage_percent > 80 && !metrics.battery_powered) {
        return OptimizationStrategy::PERFORMANCE_FIRST;
    } else if (metrics.battery_powered || metrics.cpu_usage_percent < 30) {
        return OptimizationStrategy::EFFICIENCY_FIRST;
    }
    
    return OptimizationStrategy::BALANCED;
}

OptimizationLevel AdaptiveOptimizer::recommend_level(const RuntimeMetrics& metrics) const {
    // Recommend optimization level based on system state
    
    if (metrics.thermal_throttling || (metrics.battery_powered && metrics.battery_level_percent < 15)) {
        return OptimizationLevel::MINIMAL;
    }
    
    if (metrics.battery_powered && metrics.battery_level_percent < 50) {
        return OptimizationLevel::BASIC;
    }
    
    if (metrics.cpu_usage_percent > 90 || metrics.total_jit_compilations > 1000) {
        return OptimizationLevel::MAXIMUM;
    }
    
    if (metrics.cpu_usage_percent > 70) {
        return OptimizationLevel::AGGRESSIVE;
    }
    
    return OptimizationLevel::BASIC;
}

bool AdaptiveOptimizer::should_trigger_jit_compilation(uint32_t call_count) const {
    return call_count >= current_thresholds_.jit_compilation_threshold;
}

bool AdaptiveOptimizer::should_trigger_gc(double heap_utilization) const {
    double threshold = current_thresholds_.gc_trigger_threshold / 100.0;
    return heap_utilization >= threshold;
}

bool AdaptiveOptimizer::should_deoptimize(const std::string& function_name, uint32_t deopt_count) const {
    return deopt_count >= current_thresholds_.deoptimization_threshold;
}

void AdaptiveOptimizer::print_optimization_summary() const {
    std::cout << "🧠 ADAPTIVE OPTIMIZATION SUMMARY" << std::endl;
    std::cout << "=================================" << std::endl;
    std::cout << "Current Strategy: " << static_cast<int>(current_strategy_) << std::endl;
    std::cout << "Current Level: " << static_cast<int>(current_level_) << std::endl;
    std::cout << "Optimization Events: " << optimization_history_.size() << std::endl;
    std::cout << "JIT Threshold: " << current_thresholds_.jit_compilation_threshold << std::endl;
    std::cout << "Hot Function Threshold: " << current_thresholds_.hot_function_threshold << std::endl;
    std::cout << "GC Trigger Threshold: " << current_thresholds_.gc_trigger_threshold << "%" << std::endl;
    std::cout << "Adaptive Enabled: " << (adaptive_enabled_ ? "YES" : "NO") << std::endl;
}

void AdaptiveOptimizer::adaptation_loop() {
    while (!should_stop_adaptation_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(adaptation_interval_ms_));
        
        if (adaptive_enabled_) {
            analyze_performance_trends();
            make_optimization_decision();
        }
    }
}

void AdaptiveOptimizer::analyze_performance_trends() {
    auto& monitor = RealTimePerformanceMonitor::get_instance();
    const auto& current_metrics = monitor.get_current_metrics();
    
    // Analyze if current strategy is working well
    OptimizationStrategy recommended_strategy = recommend_strategy(current_metrics);
    OptimizationLevel recommended_level = recommend_level(current_metrics);
    
    if (recommended_strategy != current_strategy_ || recommended_level != current_level_) {
        std::string reason = "Adaptive recommendation based on system metrics";
        apply_optimization_strategy(recommended_strategy, recommended_level, reason);
    }
}

void AdaptiveOptimizer::make_optimization_decision() {
    // This would contain more sophisticated decision-making logic
    // For now, we'll use simple heuristics
}

void AdaptiveOptimizer::apply_optimization_strategy(OptimizationStrategy strategy, OptimizationLevel level, const std::string& reason) {
    std::lock_guard<std::mutex> lock(optimization_mutex_);
    
    auto& monitor = RealTimePerformanceMonitor::get_instance();
    RuntimeMetrics metrics_before = monitor.get_current_metrics();
    
    // Record optimization event
    OptimizationEvent event;
    event.timestamp = std::chrono::high_resolution_clock::now();
    event.strategy = strategy;
    event.level = level;
    event.reason = reason;
    event.metrics_before = metrics_before;
    
    // Apply the new strategy
    current_strategy_ = strategy;
    current_level_ = level;
    
    // Update thresholds based on strategy
    switch (strategy) {
        case OptimizationStrategy::PERFORMANCE_FIRST:
            current_thresholds_.set_performance_thresholds();
            break;
        case OptimizationStrategy::EFFICIENCY_FIRST:
        case OptimizationStrategy::BATTERY_SAVER:
            current_thresholds_.set_efficiency_thresholds();
            break;
        default:
            current_thresholds_.set_balanced_thresholds();
            break;
    }
    
    optimization_history_.push_back(event);
    
    std::cout << "🔄 Applied optimization: " << reason << std::endl;
    std::cout << "  Strategy: " << static_cast<int>(strategy) << std::endl;
    std::cout << "  Level: " << static_cast<int>(level) << std::endl;
}

AdaptiveOptimizer& AdaptiveOptimizer::get_instance() {
    static AdaptiveOptimizer instance;
    return instance;
}

//=============================================================================
// ThermalPowerManager Implementation
//=============================================================================

ThermalPowerManager::ThermalPowerManager() {
    std::cout << "🌡️  THERMAL POWER MANAGER INITIALIZED" << std::endl;
}

ThermalPowerManager::~ThermalPowerManager() {
    stop_thermal_monitoring();
    print_thermal_status();
    print_power_status();
    std::cout << "🌡️  THERMAL POWER MANAGER SHUTDOWN" << std::endl;
}

void ThermalPowerManager::start_thermal_monitoring() {
    if (!should_stop_thermal_) return; // Already running
    
    should_stop_thermal_ = false;
    thermal_thread_ = std::thread(&ThermalPowerManager::thermal_monitoring_loop, this);
    std::cout << "🌡️  Thermal monitoring started" << std::endl;
}

void ThermalPowerManager::stop_thermal_monitoring() {
    should_stop_thermal_ = true;
    if (thermal_thread_.joinable()) {
        thermal_thread_.join();
    }
    std::cout << "🌡️  Thermal monitoring stopped" << std::endl;
}

void ThermalPowerManager::apply_thermal_throttling(double scaling_factor) {
    performance_scaling_ = std::max(0.1, std::min(1.0, scaling_factor));
    frequency_scaling_ = static_cast<uint32_t>(performance_scaling_ * 100);
    thermal_throttling_ = (scaling_factor < 1.0);
    
    std::cout << "🌡️  Thermal throttling applied: " << (scaling_factor * 100) << "%" << std::endl;
}

void ThermalPowerManager::apply_power_throttling(double scaling_factor) {
    performance_scaling_ = std::max(0.1, std::min(1.0, scaling_factor));
    frequency_scaling_ = static_cast<uint32_t>(performance_scaling_ * 100);
    
    std::cout << "🔋 Power throttling applied: " << (scaling_factor * 100) << "%" << std::endl;
}

void ThermalPowerManager::remove_throttling() {
    performance_scaling_ = 1.0;
    frequency_scaling_ = 100;
    thermal_throttling_ = false;
    
    std::cout << " Throttling removed - full performance restored" << std::endl;
}

void ThermalPowerManager::print_thermal_status() const {
    std::cout << "🌡️  THERMAL STATUS" << std::endl;
    std::cout << "==================" << std::endl;
    std::cout << "CPU Temperature: " << cpu_temperature_ << "°C" << std::endl;
    std::cout << "Thermal Threshold: " << thermal_threshold_ << "°C" << std::endl;
    std::cout << "Thermal Throttling: " << (thermal_throttling_ ? "ACTIVE" : "INACTIVE") << std::endl;
    std::cout << "Performance Scaling: " << (performance_scaling_ * 100) << "%" << std::endl;
}

void ThermalPowerManager::print_power_status() const {
    std::cout << "🔋 POWER STATUS" << std::endl;
    std::cout << "===============" << std::endl;
    std::cout << "Battery Powered: " << (battery_powered_ ? "YES" : "NO") << std::endl;
    if (battery_powered_) {
        std::cout << "Battery Level: " << battery_level_ << "%" << std::endl;
    }
    std::cout << "Power Usage: " << power_usage_watts_ << "W" << std::endl;
    std::cout << "Frequency Scaling: " << frequency_scaling_ << "%" << std::endl;
}

void ThermalPowerManager::thermal_monitoring_loop() {
    while (!should_stop_thermal_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        
        update_thermal_state();
        update_power_state();
        apply_adaptive_throttling();
    }
}

void ThermalPowerManager::update_thermal_state() {
    // Simulate temperature monitoring
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> temp_change(-2, 3);
    
    int new_temp = std::max(30, std::min(95, static_cast<int>(cpu_temperature_) + temp_change(gen)));
    cpu_temperature_ = new_temp;
}

void ThermalPowerManager::update_power_state() {
    // Simulate power monitoring
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> power_change(-1, 1);
    
    if (battery_powered_) {
        int new_level = std::max(0, std::min(100, static_cast<int>(battery_level_) + power_change(gen)));
        battery_level_ = new_level;
    }
    
    // Simulate power usage
    power_usage_watts_ = 15 + (gen() % 20); // 15-35W range
}

void ThermalPowerManager::apply_adaptive_throttling() {
    // Apply throttling based on thermal and power conditions
    if (cpu_temperature_ > thermal_threshold_) {
        double throttle_factor = 1.0 - ((cpu_temperature_ - thermal_threshold_) / 20.0);
        apply_thermal_throttling(std::max(0.3, throttle_factor));
    } else if (battery_powered_ && battery_level_ < 20) {
        apply_power_throttling(0.7); // Reduce to 70% performance
    } else if (battery_powered_ && battery_level_ < 10) {
        apply_power_throttling(0.5); // Reduce to 50% performance
    } else if (!thermal_throttling_ && performance_scaling_ < 1.0) {
        remove_throttling();
    }
}

ThermalPowerManager& ThermalPowerManager::get_instance() {
    static ThermalPowerManager instance;
    return instance;
}

//=============================================================================
// AdaptiveOptimizationIntegration Implementation
//=============================================================================

namespace AdaptiveOptimizationIntegration {

void initialize_adaptive_systems() {
    std::cout << "🧠 INITIALIZING ADAPTIVE OPTIMIZATION SYSTEMS" << std::endl;
    
    // Initialize all adaptive components
    RealTimePerformanceMonitor::get_instance();
    AdaptiveOptimizer::get_instance();
    ThermalPowerManager::get_instance();
    
    std::cout << "✅ ALL ADAPTIVE SYSTEMS INITIALIZED" << std::endl;
    std::cout << "   Real-time Performance Monitor: Ready" << std::endl;
    std::cout << "  🧠 Adaptive Optimizer: Ready" << std::endl;
    std::cout << "  🌡️  Thermal Power Manager: Ready" << std::endl;
}

void shutdown_adaptive_systems() {
    std::cout << "🧠 SHUTTING DOWN ADAPTIVE SYSTEMS" << std::endl;
    
    // Print final reports
    print_comprehensive_performance_report();
    
    std::cout << "✅ ALL ADAPTIVE SYSTEMS SHUTDOWN" << std::endl;
}

void start_all_monitoring() {
    RealTimePerformanceMonitor::get_instance().start_monitoring();
    AdaptiveOptimizer::get_instance().start_adaptive_optimization();
    ThermalPowerManager::get_instance().start_thermal_monitoring();
    
    std::cout << "🔍 ALL MONITORING SYSTEMS STARTED" << std::endl;
}

void stop_all_monitoring() {
    RealTimePerformanceMonitor::get_instance().stop_monitoring();
    AdaptiveOptimizer::get_instance().stop_adaptive_optimization();
    ThermalPowerManager::get_instance().stop_thermal_monitoring();
    
    std::cout << "🔍 ALL MONITORING SYSTEMS STOPPED" << std::endl;
}

void enable_adaptive_optimization() {
    AdaptiveOptimizer::get_instance().enable_adaptive_optimization();
    std::cout << "🧠 Adaptive optimization enabled" << std::endl;
}

void disable_adaptive_optimization() {
    AdaptiveOptimizer::get_instance().disable_adaptive_optimization();
    std::cout << "🧠 Adaptive optimization disabled" << std::endl;
}

void set_global_strategy(OptimizationStrategy strategy) {
    AdaptiveOptimizer::get_instance().set_optimization_strategy(strategy);
    std::cout << "🔄 Global optimization strategy set: " << static_cast<int>(strategy) << std::endl;
}

void set_global_level(OptimizationLevel level) {
    AdaptiveOptimizer::get_instance().set_optimization_level(level);
    std::cout << "🔄 Global optimization level set: " << static_cast<int>(level) << std::endl;
}

void apply_emergency_throttling() {
    ThermalPowerManager::get_instance().apply_thermal_throttling(0.5);
    set_global_strategy(OptimizationStrategy::EFFICIENCY_FIRST);
    set_global_level(OptimizationLevel::MINIMAL);
    
    std::cout << "🚨 EMERGENCY THROTTLING APPLIED" << std::endl;
}

void remove_emergency_throttling() {
    ThermalPowerManager::get_instance().remove_throttling();
    set_global_strategy(OptimizationStrategy::BALANCED);
    set_global_level(OptimizationLevel::BASIC);
    
    std::cout << "✅ Emergency throttling removed" << std::endl;
}

void print_comprehensive_performance_report() {
    std::cout << "\n" << std::string(80, '=') << std::endl;
    std::cout << "🧠 COMPREHENSIVE ADAPTIVE OPTIMIZATION REPORT" << std::endl;
    std::cout << std::string(80, '=') << std::endl;
    
    RealTimePerformanceMonitor::get_instance().print_real_time_stats();
    std::cout << std::endl;
    
    AdaptiveOptimizer::get_instance().print_optimization_summary();
    std::cout << std::endl;
    
    ThermalPowerManager::get_instance().print_thermal_status();
    std::cout << std::endl;
    
    ThermalPowerManager::get_instance().print_power_status();
    std::cout << std::endl;
}

void configure_for_development() {
    set_global_strategy(OptimizationStrategy::BALANCED);
    set_global_level(OptimizationLevel::BASIC);
    std::cout << "🔧 Configured for development environment" << std::endl;
}

void configure_for_production() {
    set_global_strategy(OptimizationStrategy::PERFORMANCE_FIRST);
    set_global_level(OptimizationLevel::AGGRESSIVE);
    std::cout << "🚀 Configured for production environment" << std::endl;
}

void configure_for_mobile() {
    set_global_strategy(OptimizationStrategy::BATTERY_SAVER);
    set_global_level(OptimizationLevel::BASIC);
    std::cout << "📱 Configured for mobile environment" << std::endl;
}

void configure_for_server() {
    set_global_strategy(OptimizationStrategy::COMPUTE_INTENSIVE);
    set_global_level(OptimizationLevel::MAXIMUM);
    std::cout << "🖥️  Configured for server environment" << std::endl;
}

void handle_thermal_emergency() {
    std::cout << "🚨 THERMAL EMERGENCY DETECTED!" << std::endl;
    apply_emergency_throttling();
}

void handle_memory_pressure() {
    std::cout << "💾 MEMORY PRESSURE DETECTED!" << std::endl;
    set_global_strategy(OptimizationStrategy::MEMORY_CONSTRAINED);
}

void handle_battery_critical() {
    std::cout << "🔋 CRITICAL BATTERY LEVEL!" << std::endl;
    set_global_strategy(OptimizationStrategy::BATTERY_SAVER);
    set_global_level(OptimizationLevel::MINIMAL);
}

} // namespace AdaptiveOptimizationIntegration

} // namespace Quanta