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
#include <atomic>
#include <thread>
#include <mutex>
#include <functional>
#include <queue>

namespace Quanta {

// Forward declarations
class Value;
class Function;

//=============================================================================
// Real-Time Performance Monitoring and Adaptive Optimization
//
// Intelligent performance monitoring system with adaptive optimization:
// - Real-time performance profiling and analysis
// - Dynamic optimization level adjustment
// - Adaptive JIT compilation thresholds
// - Memory usage optimization
// - CPU utilization monitoring
// - Thermal throttling detection
// - Battery-aware optimization
// - Network latency compensation
// - Cache behavior analysis
// - Branch prediction optimization
//=============================================================================

//=============================================================================
// Performance Metrics Collection
//=============================================================================

struct RuntimeMetrics {
    // Execution metrics
    std::atomic<uint64_t> total_instructions{0};
    std::atomic<uint64_t> total_function_calls{0};
    std::atomic<uint64_t> total_jit_compilations{0};
    std::atomic<uint64_t> total_deoptimizations{0};
    
    // Timing metrics (nanoseconds)
    std::atomic<uint64_t> total_execution_time_ns{0};
    std::atomic<uint64_t> jit_compile_time_ns{0};
    std::atomic<uint64_t> gc_time_ns{0};
    std::atomic<uint64_t> optimization_time_ns{0};
    
    // Memory metrics
    std::atomic<uint64_t> heap_size_bytes{0};
    std::atomic<uint64_t> used_heap_bytes{0};
    std::atomic<uint64_t> gc_collections{0};
    std::atomic<uint64_t> allocated_objects{0};
    
    // Cache metrics
    std::atomic<uint64_t> l1_cache_hits{0};
    std::atomic<uint64_t> l1_cache_misses{0};
    std::atomic<uint64_t> l2_cache_hits{0};
    std::atomic<uint64_t> l2_cache_misses{0};
    std::atomic<uint64_t> l3_cache_hits{0};
    std::atomic<uint64_t> l3_cache_misses{0};
    
    // Branch prediction metrics
    std::atomic<uint64_t> branch_predictions{0};
    std::atomic<uint64_t> branch_mispredictions{0};
    std::atomic<uint64_t> indirect_calls{0};
    std::atomic<uint64_t> polymorphic_calls{0};
    
    // System metrics
    std::atomic<uint32_t> cpu_usage_percent{0};
    std::atomic<uint32_t> memory_usage_percent{0};
    std::atomic<uint32_t> cpu_temperature_celsius{0};
    std::atomic<bool> thermal_throttling{false};
    std::atomic<bool> battery_powered{false};
    std::atomic<uint32_t> battery_level_percent{100};
    
    void reset() {
        total_instructions = 0;
        total_function_calls = 0;
        total_jit_compilations = 0;
        total_deoptimizations = 0;
        total_execution_time_ns = 0;
        jit_compile_time_ns = 0;
        gc_time_ns = 0;
        optimization_time_ns = 0;
        heap_size_bytes = 0;
        used_heap_bytes = 0;
        gc_collections = 0;
        allocated_objects = 0;
        l1_cache_hits = 0;
        l1_cache_misses = 0;
        l2_cache_hits = 0;
        l2_cache_misses = 0;
        l3_cache_hits = 0;
        l3_cache_misses = 0;
        branch_predictions = 0;
        branch_mispredictions = 0;
        indirect_calls = 0;
        polymorphic_calls = 0;
        cpu_usage_percent = 0;
        memory_usage_percent = 0;
        cpu_temperature_celsius = 0;
        thermal_throttling = false;
        battery_powered = false;
        battery_level_percent = 100;
    }
};

//=============================================================================
// Adaptive Optimization Strategies
//=============================================================================

enum class OptimizationStrategy {
    PERFORMANCE_FIRST,      // Maximum performance, high power consumption
    BALANCED,              // Balanced performance and efficiency
    EFFICIENCY_FIRST,      // Maximum efficiency, lower performance
    BATTERY_SAVER,         // Minimal power consumption
    THERMAL_AWARE,         // Temperature-conscious optimization
    MEMORY_CONSTRAINED,    // Optimized for low memory usage
    NETWORK_OPTIMIZED,     // Optimized for network applications
    COMPUTE_INTENSIVE,     // Optimized for CPU-heavy workloads
    ADAPTIVE              // Automatically adapt based on conditions
};

enum class OptimizationLevel {
    DISABLED = 0,
    MINIMAL = 1,
    BASIC = 2,
    AGGRESSIVE = 3,
    MAXIMUM = 4,
    optimized = 5
};

struct OptimizationThresholds {
    uint32_t jit_compilation_threshold;
    uint32_t hot_function_threshold;
    uint32_t deoptimization_threshold;
    uint32_t gc_trigger_threshold;
    uint32_t cache_optimization_threshold;
    uint32_t inline_threshold;
    uint32_t unroll_threshold;
    uint32_t vectorization_threshold;
    
    OptimizationThresholds() {
        set_balanced_thresholds();
    }
    
    void set_performance_thresholds() {
        jit_compilation_threshold = 5;
        hot_function_threshold = 10;
        deoptimization_threshold = 100;
        gc_trigger_threshold = 90;
        cache_optimization_threshold = 3;
        inline_threshold = 20;
        unroll_threshold = 8;
        vectorization_threshold = 4;
    }
    
    void set_balanced_thresholds() {
        jit_compilation_threshold = 15;
        hot_function_threshold = 50;
        deoptimization_threshold = 50;
        gc_trigger_threshold = 75;
        cache_optimization_threshold = 10;
        inline_threshold = 50;
        unroll_threshold = 16;
        vectorization_threshold = 8;
    }
    
    void set_efficiency_thresholds() {
        jit_compilation_threshold = 50;
        hot_function_threshold = 200;
        deoptimization_threshold = 20;
        gc_trigger_threshold = 60;
        cache_optimization_threshold = 50;
        inline_threshold = 100;
        unroll_threshold = 32;
        vectorization_threshold = 16;
    }
};

//=============================================================================
// Real-Time Performance Monitor
//=============================================================================

class RealTimePerformanceMonitor {
private:
    RuntimeMetrics current_metrics_;
    std::vector<RuntimeMetrics> historical_snapshots_;
    
    // Monitoring configuration
    std::atomic<bool> monitoring_enabled_{true};
    std::atomic<uint32_t> monitoring_interval_ms_{100};
    std::atomic<uint32_t> snapshot_interval_ms_{1000};
    
    // Monitoring thread
    std::thread monitoring_thread_;
    std::atomic<bool> should_stop_{false};
    
    // Performance analysis
    mutable std::mutex analysis_mutex_;
    std::chrono::high_resolution_clock::time_point start_time_;
    
    // Threshold monitoring
    std::unordered_map<std::string, std::function<void()>> threshold_callbacks_;

public:
    RealTimePerformanceMonitor();
    ~RealTimePerformanceMonitor();
    
    // Monitoring control
    void start_monitoring();
    void stop_monitoring();
    void pause_monitoring();
    void resume_monitoring();
    
    // Configuration
    void set_monitoring_interval(uint32_t interval_ms) { monitoring_interval_ms_ = interval_ms; }
    void set_snapshot_interval(uint32_t interval_ms) { snapshot_interval_ms_ = interval_ms; }
    
    // Metrics recording
    void record_instruction() { current_metrics_.total_instructions++; }
    void record_function_call() { current_metrics_.total_function_calls++; }
    void record_jit_compilation(uint64_t compile_time_ns);
    void record_deoptimization() { current_metrics_.total_deoptimizations++; }
    void record_gc_collection(uint64_t gc_time_ns);
    void record_memory_allocation(uint64_t bytes);
    void record_cache_access(bool l1_hit, bool l2_hit, bool l3_hit);
    void record_branch_prediction(bool correct);
    void update_system_metrics();
    
    // Data access
    const RuntimeMetrics& get_current_metrics() const { return current_metrics_; }
    std::vector<RuntimeMetrics> get_historical_snapshots() const;
    
    // Analysis
    double get_instructions_per_second() const;
    double get_cache_hit_ratio() const;
    double get_branch_prediction_accuracy() const;
    double get_jit_compilation_rate() const;
    double get_gc_overhead_percentage() const;
    double get_memory_utilization() const;
    
    // Threshold monitoring
    void set_threshold_callback(const std::string& metric, double threshold, std::function<void()> callback);
    void remove_threshold_callback(const std::string& metric);
    
    // Reporting
    void print_real_time_stats() const;
    void print_performance_trends() const;
    void export_performance_data(const std::string& filename) const;
    
    // Singleton access
    static RealTimePerformanceMonitor& get_instance();

private:
    void monitoring_loop();
    void take_snapshot();
    void check_thresholds();
    void collect_system_metrics();
};

//=============================================================================
// Adaptive Optimizer Engine
//=============================================================================

class AdaptiveOptimizer {
private:
    // Current optimization state
    OptimizationStrategy current_strategy_;
    OptimizationLevel current_level_;
    OptimizationThresholds current_thresholds_;
    
    // Optimization history
    struct OptimizationEvent {
        std::chrono::high_resolution_clock::time_point timestamp;
        OptimizationStrategy strategy;
        OptimizationLevel level;
        std::string reason;
        RuntimeMetrics metrics_before;
        RuntimeMetrics metrics_after;
        double performance_impact;
    };
    
    std::vector<OptimizationEvent> optimization_history_;
    
    // Adaptation parameters
    std::atomic<bool> adaptive_enabled_{true};
    std::atomic<uint32_t> adaptation_interval_ms_{5000};
    std::atomic<double> performance_threshold_{0.05}; // 5% performance change
    
    // Decision engine
    std::thread adaptation_thread_;
    std::atomic<bool> should_stop_adaptation_{false};
    mutable std::mutex optimization_mutex_;
    
    // Machine learning for optimization
    struct DecisionModel {
        std::unordered_map<std::string, double> feature_weights;
        std::vector<std::pair<std::string, double>> decision_tree;
        double learning_rate;
        uint32_t training_samples;
        
        DecisionModel() : learning_rate(0.01), training_samples(0) {}
    };
    
    DecisionModel decision_model_;

public:
    AdaptiveOptimizer();
    ~AdaptiveOptimizer();
    
    // Optimization control
    void start_adaptive_optimization();
    void stop_adaptive_optimization();
    void force_optimization_update();
    
    // Strategy management
    void set_optimization_strategy(OptimizationStrategy strategy);
    void set_optimization_level(OptimizationLevel level);
    OptimizationStrategy get_current_strategy() const { return current_strategy_; }
    OptimizationLevel get_current_level() const { return current_level_; }
    
    // Threshold management
    const OptimizationThresholds& get_current_thresholds() const { return current_thresholds_; }
    void update_thresholds(const OptimizationThresholds& thresholds);
    
    // Adaptive behavior
    void enable_adaptive_optimization() { adaptive_enabled_ = true; }
    void disable_adaptive_optimization() { adaptive_enabled_ = false; }
    bool is_adaptive_enabled() const { return adaptive_enabled_; }
    
    // Decision making
    OptimizationStrategy recommend_strategy(const RuntimeMetrics& metrics) const;
    OptimizationLevel recommend_level(const RuntimeMetrics& metrics) const;
    bool should_trigger_jit_compilation(uint32_t call_count) const;
    bool should_trigger_gc(double heap_utilization) const;
    bool should_deoptimize(const std::string& function_name, uint32_t deopt_count) const;
    
    // Learning and adaptation
    void learn_from_optimization(const OptimizationEvent& event);
    void update_decision_model();
    double predict_performance_impact(OptimizationStrategy strategy, OptimizationLevel level) const;
    
    // Analysis
    std::vector<OptimizationEvent> get_optimization_history() const;
    double get_optimization_effectiveness() const;
    std::string get_current_optimization_reason() const;
    
    // Reporting
    void print_optimization_summary() const;
    void print_adaptation_history() const;
    void export_optimization_data(const std::string& filename) const;
    
    // Singleton access
    static AdaptiveOptimizer& get_instance();

private:
    void adaptation_loop();
    void analyze_performance_trends();
    void make_optimization_decision();
    void apply_optimization_strategy(OptimizationStrategy strategy, OptimizationLevel level, const std::string& reason);
    double calculate_performance_score(const RuntimeMetrics& metrics) const;
    std::vector<double> extract_features(const RuntimeMetrics& metrics) const;
};

//=============================================================================
// Thermal and Power Management
//=============================================================================

class ThermalPowerManager {
private:
    // Thermal monitoring
    std::atomic<uint32_t> cpu_temperature_{0};
    std::atomic<uint32_t> thermal_threshold_{80}; // Celsius
    std::atomic<bool> thermal_throttling_{false};
    
    // Power monitoring
    std::atomic<bool> battery_powered_{false};
    std::atomic<uint32_t> battery_level_{100};
    std::atomic<uint32_t> power_usage_watts_{0};
    
    // Throttling state
    std::atomic<double> performance_scaling_{1.0};
    std::atomic<uint32_t> frequency_scaling_{100}; // Percentage
    
    // Monitoring thread
    std::thread thermal_thread_;
    std::atomic<bool> should_stop_thermal_{false};

public:
    ThermalPowerManager();
    ~ThermalPowerManager();
    
    // Monitoring control
    void start_thermal_monitoring();
    void stop_thermal_monitoring();
    
    // Temperature management
    uint32_t get_cpu_temperature() const { return cpu_temperature_; }
    void set_thermal_threshold(uint32_t threshold) { thermal_threshold_ = threshold; }
    bool is_thermally_throttled() const { return thermal_throttling_; }
    
    // Power management
    bool is_battery_powered() const { return battery_powered_; }
    uint32_t get_battery_level() const { return battery_level_; }
    uint32_t get_power_usage() const { return power_usage_watts_; }
    
    // Performance scaling
    double get_performance_scaling() const { return performance_scaling_; }
    uint32_t get_frequency_scaling() const { return frequency_scaling_; }
    
    // Throttling control
    void apply_thermal_throttling(double scaling_factor);
    void apply_power_throttling(double scaling_factor);
    void remove_throttling();
    
    // Callbacks
    void on_thermal_event(std::function<void(uint32_t temperature)> callback);
    void on_power_event(std::function<void(uint32_t battery_level)> callback);
    
    // Reporting
    void print_thermal_status() const;
    void print_power_status() const;
    
    // Singleton access
    static ThermalPowerManager& get_instance();

private:
    void thermal_monitoring_loop();
    void update_thermal_state();
    void update_power_state();
    void apply_adaptive_throttling();
};

//=============================================================================
// Network Performance Optimizer
//=============================================================================

class NetworkPerformanceOptimizer {
private:
    // Network metrics
    std::atomic<uint32_t> network_latency_ms_{0};
    std::atomic<uint32_t> bandwidth_mbps_{0};
    std::atomic<uint32_t> packet_loss_percent_{0};
    std::atomic<bool> network_available_{true};
    
    // Optimization state
    std::atomic<bool> network_optimization_enabled_{false};
    std::atomic<uint32_t> prefetch_threshold_{100}; // ms
    std::atomic<uint32_t> cache_retention_ms_{30000}; // 30 seconds
    
    // Request queue optimization
    std::queue<std::function<void()>> pending_requests_;
    std::mutex request_queue_mutex_;

public:
    NetworkPerformanceOptimizer();
    ~NetworkPerformanceOptimizer();
    
    // Network monitoring
    void update_network_metrics(uint32_t latency_ms, uint32_t bandwidth_mbps, uint32_t packet_loss);
    void set_network_availability(bool available) { network_available_ = available; }
    
    // Optimization control
    void enable_network_optimization() { network_optimization_enabled_ = true; }
    void disable_network_optimization() { network_optimization_enabled_ = false; }
    bool is_network_optimization_enabled() const { return network_optimization_enabled_; }
    
    // Adaptive behavior
    bool should_prefetch_data() const;
    bool should_cache_aggressively() const;
    uint32_t get_optimal_timeout() const;
    uint32_t get_optimal_retry_count() const;
    
    // Request optimization
    void queue_network_request(std::function<void()> request);
    void process_request_queue();
    void optimize_request_batching();
    
    // Metrics access
    uint32_t get_network_latency() const { return network_latency_ms_; }
    uint32_t get_bandwidth() const { return bandwidth_mbps_; }
    uint32_t get_packet_loss() const { return packet_loss_percent_; }
    bool is_network_available() const { return network_available_; }
    
    // Singleton access
    static NetworkPerformanceOptimizer& get_instance();
};

//=============================================================================
// Adaptive Optimization Integration
//=============================================================================

namespace AdaptiveOptimizationIntegration {
    // System initialization
    void initialize_adaptive_systems();
    void shutdown_adaptive_systems();
    
    // Monitoring control
    void start_all_monitoring();
    void stop_all_monitoring();
    void pause_all_monitoring();
    void resume_all_monitoring();
    
    // Optimization control
    void enable_adaptive_optimization();
    void disable_adaptive_optimization();
    void force_optimization_update();
    
    // Strategy management
    void set_global_strategy(OptimizationStrategy strategy);
    void set_global_level(OptimizationLevel level);
    void apply_emergency_throttling();
    void remove_emergency_throttling();
    
    // Analysis and reporting
    void print_comprehensive_performance_report();
    void print_optimization_effectiveness();
    void export_all_performance_data(const std::string& directory);
    
    // Callback registration
    void register_performance_callback(const std::string& name, std::function<void(const RuntimeMetrics&)> callback);
    void register_optimization_callback(const std::string& name, std::function<void(OptimizationStrategy, OptimizationLevel)> callback);
    
    // Utility functions
    void configure_for_development();
    void configure_for_production();
    void configure_for_mobile();
    void configure_for_server();
    void configure_for_embedded();
    
    // Emergency procedures
    void handle_thermal_emergency();
    void handle_memory_pressure();
    void handle_battery_critical();
    void handle_network_degradation();
}

//=============================================================================
// Performance Prediction and ML
//=============================================================================

class PerformancePredictor {
private:
    struct PredictionModel {
        std::vector<std::vector<double>> training_data;
        std::vector<double> training_labels;
        std::vector<double> weights;
        double bias;
        double learning_rate;
        uint32_t epochs;
        
        PredictionModel() : bias(0.0), learning_rate(0.01), epochs(1000) {}
    };
    
    PredictionModel jit_model_;
    PredictionModel gc_model_;
    PredictionModel cache_model_;
    
    mutable std::mutex model_mutex_;

public:
    PerformancePredictor();
    ~PerformancePredictor();
    
    // Model training
    void train_jit_model(const std::vector<std::vector<double>>& features, const std::vector<double>& performance);
    void train_gc_model(const std::vector<std::vector<double>>& features, const std::vector<double>& performance);
    void train_cache_model(const std::vector<std::vector<double>>& features, const std::vector<double>& performance);
    
    // Predictions
    double predict_jit_benefit(const std::vector<double>& features) const;
    double predict_gc_impact(const std::vector<double>& features) const;
    double predict_cache_behavior(const std::vector<double>& features) const;
    
    // Model management
    void save_models(const std::string& directory) const;
    void load_models(const std::string& directory);
    void reset_models();
    
    // Analysis
    double get_model_accuracy(const std::string& model_name) const;
    void print_model_statistics() const;
    
    // Singleton access
    static PerformancePredictor& get_instance();

private:
    void train_linear_model(PredictionModel& model);
    double predict_with_model(const PredictionModel& model, const std::vector<double>& features) const;
    std::vector<double> extract_jit_features(const RuntimeMetrics& metrics) const;
    std::vector<double> extract_gc_features(const RuntimeMetrics& metrics) const;
    std::vector<double> extract_cache_features(const RuntimeMetrics& metrics) const;
};

} // namespace Quanta