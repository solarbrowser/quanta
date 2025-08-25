/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <atomic>

namespace Quanta {

//=============================================================================
// CPU-Specific Optimizations (Intel/AMD Instruction Sets)
//
// Advanced CPU optimization system for maximum performance:
// - CPU feature detection (SSE, AVX, AVX-512, etc.)
// - Dynamic instruction set selection
// - CPU vendor-specific optimizations (Intel vs AMD)
// - Branch prediction optimization
// - Cache hierarchy optimization
// - Microarchitecture-specific tuning
// - Assembly code generation
// - Performance counter integration
//=============================================================================

//=============================================================================
// CPU Feature Detection
//=============================================================================

enum class CPUVendor {
    INTEL,
    AMD,
    UNKNOWN
};

enum class CPUArchitecture {
    X86,
    X86_64,
    ARM,
    ARM64,
    UNKNOWN
};

struct CPUFeatures {
    // Basic features
    bool sse;
    bool sse2;
    bool sse3;
    bool ssse3;
    bool sse4_1;
    bool sse4_2;
    bool sse4a;         // AMD-specific
    
    // Advanced vector extensions
    bool avx;
    bool avx2;
    bool avx512f;       // Foundation
    bool avx512cd;      // Conflict Detection
    bool avx512er;      // Exponential and Reciprocal
    bool avx512pf;      // Prefetch
    bool avx512bw;      // Byte and Word
    bool avx512dq;      // Doubleword and Quadword
    bool avx512vl;      // Vector Length Extensions
    
    // Bit manipulation
    bool bmi1;
    bool bmi2;
    bool popcnt;
    bool lzcnt;
    
    // Cryptography
    bool aes;
    bool pclmul;
    bool sha;
    bool sha512;        // AMD-specific
    
    // Memory and cache
    bool prefetchw;
    bool prefetchwt1;
    bool clflush;
    bool clflushopt;
    bool clwb;
    
    // Threading and synchronization
    bool htt;           // Hyper-Threading
    bool cmpxchg16b;
    bool movbe;
    
    // Performance monitoring
    bool rdtscp;
    bool pdcm;          // Performance Debug Capability MSR
    bool pcid;          // Process Context Identifiers
    
    // AMD-specific features
    bool fma4;          // 4-operand FMA
    bool xop;           // eXtended Operations
    bool tbm;           // Trailing Bit Manipulation
    bool lwp;           // Lightweight Profiling
    bool svm;           // Secure Virtual Machine
    
    // Intel-specific features
    bool mpx;           // Memory Protection Extensions
    bool sgx;           // Software Guard Extensions
    bool cet;           // Control-flow Enforcement Technology
    bool intel_pt;      // Intel Processor Trace
    
    CPUFeatures() { reset(); }
    
    void reset() {
        sse = sse2 = sse3 = ssse3 = sse4_1 = sse4_2 = sse4a = false;
        avx = avx2 = avx512f = avx512cd = avx512er = avx512pf = false;
        avx512bw = avx512dq = avx512vl = false;
        bmi1 = bmi2 = popcnt = lzcnt = false;
        aes = pclmul = sha = sha512 = false;
        prefetchw = prefetchwt1 = clflush = clflushopt = clwb = false;
        htt = cmpxchg16b = movbe = false;
        rdtscp = pdcm = pcid = false;
        fma4 = xop = tbm = lwp = svm = false;
        mpx = sgx = cet = intel_pt = false;
    }
};

struct CPUInfo {
    CPUVendor vendor;
    CPUArchitecture architecture;
    std::string brand_string;
    std::string model_name;
    
    uint32_t family;
    uint32_t model;
    uint32_t stepping;
    uint32_t signature;
    
    uint32_t logical_cores;
    uint32_t physical_cores;
    uint32_t threads_per_core;
    
    // Cache hierarchy
    uint32_t l1_data_cache_size;        // KB
    uint32_t l1_instruction_cache_size; // KB
    uint32_t l2_cache_size;             // KB
    uint32_t l3_cache_size;             // KB
    uint32_t cache_line_size;           // bytes
    
    // Performance characteristics
    uint32_t base_frequency_mhz;
    uint32_t max_frequency_mhz;
    uint32_t bus_frequency_mhz;
    
    CPUFeatures features;
    
    CPUInfo() { reset(); }
    
    void reset() {
        vendor = CPUVendor::UNKNOWN;
        architecture = CPUArchitecture::UNKNOWN;
        brand_string.clear();
        model_name.clear();
        family = model = stepping = signature = 0;
        logical_cores = physical_cores = threads_per_core = 0;
        l1_data_cache_size = l1_instruction_cache_size = 0;
        l2_cache_size = l3_cache_size = cache_line_size = 0;
        base_frequency_mhz = max_frequency_mhz = bus_frequency_mhz = 0;
        features.reset();
    }
};

//=============================================================================
// CPU Detection Engine
//=============================================================================

class CPUDetector {
private:
    CPUInfo cpu_info_;
    bool detection_complete_;
    
public:
    CPUDetector();
    ~CPUDetector();
    
    // Detection methods
    bool detect_cpu_info();
    void force_redetection();
    
    // Information access
    const CPUInfo& get_cpu_info() const { return cpu_info_; }
    CPUVendor get_vendor() const { return cpu_info_.vendor; }
    CPUArchitecture get_architecture() const { return cpu_info_.architecture; }
    const CPUFeatures& get_features() const { return cpu_info_.features; }
    
    // Feature queries
    bool has_feature(const std::string& feature_name) const;
    bool supports_vector_width(uint32_t bits) const;
    uint32_t get_max_vector_width() const;
    std::vector<std::string> get_supported_instruction_sets() const;
    
    // Performance characteristics
    uint32_t get_cache_line_size() const { return cpu_info_.cache_line_size; }
    uint32_t get_l3_cache_size() const { return cpu_info_.l3_cache_size; }
    uint32_t get_logical_core_count() const { return cpu_info_.logical_cores; }
    bool supports_hyperthreading() const { return cpu_info_.features.htt; }
    
    // Utility methods
    void print_cpu_info() const;
    void print_supported_features() const;
    std::string get_optimal_instruction_set() const;
    
    // Singleton access
    static CPUDetector& get_instance();

private:
    void detect_vendor_and_brand();
    void detect_features_intel();
    void detect_features_amd();
    void detect_cache_info();
    void detect_performance_info();
    
    // CPUID wrapper functions
    struct CPUIDResult {
        uint32_t eax, ebx, ecx, edx;
    };
    
    CPUIDResult cpuid(uint32_t leaf, uint32_t subleaf = 0) const;
    bool is_intel() const { return cpu_info_.vendor == CPUVendor::INTEL; }
    bool is_amd() const { return cpu_info_.vendor == CPUVendor::AMD; }
};

//=============================================================================
// CPU-Specific Code Generator
//=============================================================================

enum class OptimizationTarget {
    GENERIC,
    INTEL_CORE,
    INTEL_ATOM,
    AMD_ZEN,
    AMD_BULLDOZER,
    ARM_CORTEX_A,
    ARM_CORTEX_M
};

class CPUCodeGenerator {
private:
    const CPUInfo& cpu_info_;
    OptimizationTarget target_;
    std::vector<uint8_t> code_buffer_;
    
    // Code generation state
    struct CodeGenState {
        size_t current_offset;
        std::unordered_map<std::string, size_t> labels;
        std::vector<std::pair<size_t, std::string>> fixups;
        uint32_t register_allocation_mask;
        
        CodeGenState() : current_offset(0), register_allocation_mask(0) {}
    };
    
    CodeGenState state_;

public:
    explicit CPUCodeGenerator(const CPUInfo& cpu_info);
    ~CPUCodeGenerator();
    
    // Target selection
    void set_optimization_target(OptimizationTarget target) { target_ = target; }
    OptimizationTarget get_optimization_target() const { return target_; }
    
    // Code generation
    void begin_function();
    void end_function();
    void clear_code();
    
    // Instruction emission
    void emit_mov(uint8_t dst_reg, uint64_t immediate);
    void emit_add(uint8_t dst_reg, uint8_t src_reg);
    void emit_mul(uint8_t dst_reg, uint8_t src_reg);
    void emit_call(const std::string& function_name);
    void emit_ret();
    
    // Vector instructions
    void emit_vector_add_f32(uint8_t dst_reg, uint8_t src1_reg, uint8_t src2_reg);
    void emit_vector_mul_f32(uint8_t dst_reg, uint8_t src1_reg, uint8_t src2_reg);
    void emit_vector_load(uint8_t dst_reg, uint8_t addr_reg, int32_t offset);
    void emit_vector_store(uint8_t src_reg, uint8_t addr_reg, int32_t offset);
    
    // Specialized instructions
    void emit_prefetch(uint8_t addr_reg, int32_t offset, uint8_t locality);
    void emit_cache_flush(uint8_t addr_reg);
    void emit_memory_fence();
    void emit_branch_hint(const std::string& label, bool likely);
    
    // Advanced features
    void emit_crypto_aes_encrypt(uint8_t data_reg, uint8_t key_reg);
    void emit_bit_scan_forward(uint8_t dst_reg, uint8_t src_reg);
    void emit_population_count(uint8_t dst_reg, uint8_t src_reg);
    
    // Code finalization
    void* finalize_code();
    size_t get_code_size() const { return code_buffer_.size(); }
    
    // Optimization hints
    void optimize_for_intel_core();
    void optimize_for_amd_zen();
    void apply_microarchitecture_hints();

private:
    void emit_bytes(const std::vector<uint8_t>& bytes);
    void emit_byte(uint8_t byte);
    void emit_word(uint16_t word);
    void emit_dword(uint32_t dword);
    void emit_qword(uint64_t qword);
    
    uint8_t allocate_register();
    void free_register(uint8_t reg);
    
    std::vector<uint8_t> encode_instruction(const std::string& mnemonic, 
                                           const std::vector<std::string>& operands);
};

//=============================================================================
// Branch Prediction Optimizer
//=============================================================================

class BranchPredictor {
private:
    struct BranchInfo {
        uint64_t address;
        uint64_t taken_count;
        uint64_t not_taken_count;
        double prediction_accuracy;
        bool is_hot_branch;
        
        BranchInfo() : address(0), taken_count(0), not_taken_count(0), 
                      prediction_accuracy(0.0), is_hot_branch(false) {}
    };
    
    std::unordered_map<uint64_t, BranchInfo> branch_statistics_;
    uint64_t total_branches_;
    uint64_t correct_predictions_;
    
    // Branch prediction strategies
    enum class PredictionStrategy {
        STATIC_TAKEN,
        STATIC_NOT_TAKEN,
        BIMODAL,
        GSHARE,
        NEURAL
    };
    
    PredictionStrategy current_strategy_;

public:
    BranchPredictor();
    ~BranchPredictor();
    
    // Branch tracking
    void record_branch(uint64_t address, bool taken);
    void update_prediction_accuracy();
    
    // Prediction
    bool predict_branch(uint64_t address) const;
    double get_confidence(uint64_t address) const;
    
    // Optimization
    void optimize_branch_layout();
    void apply_branch_hints();
    std::vector<uint64_t> get_hot_branches() const;
    
    // Statistics
    double get_overall_accuracy() const;
    void print_branch_statistics() const;
    void reset_statistics();
    
    // Strategy management
    void set_prediction_strategy(PredictionStrategy strategy) { current_strategy_ = strategy; }
    PredictionStrategy get_prediction_strategy() const { return current_strategy_; }

private:
    bool predict_bimodal(uint64_t address) const;
    bool predict_gshare(uint64_t address) const;
    bool predict_neural(uint64_t address) const;
    void update_bimodal_predictor(uint64_t address, bool taken);
    void update_gshare_predictor(uint64_t address, bool taken);
};

//=============================================================================
// Cache Optimizer
//=============================================================================

class CacheOptimizer {
private:
    const CPUInfo& cpu_info_;
    
    struct CacheProfile {
        uint64_t l1_hits;
        uint64_t l1_misses;
        uint64_t l2_hits;
        uint64_t l2_misses;
        uint64_t l3_hits;
        uint64_t l3_misses;
        uint64_t prefetch_hits;
        uint64_t prefetch_misses;
        
        CacheProfile() : l1_hits(0), l1_misses(0), l2_hits(0), l2_misses(0),
                        l3_hits(0), l3_misses(0), prefetch_hits(0), prefetch_misses(0) {}
    };
    
    CacheProfile cache_profile_;
    std::vector<uint64_t> hot_addresses_;
    uint32_t cache_line_size_;

public:
    explicit CacheOptimizer(const CPUInfo& cpu_info);
    ~CacheOptimizer();
    
    // Cache profiling
    void record_memory_access(uint64_t address, uint8_t level, bool hit);
    void update_cache_profile();
    
    // Optimization strategies
    void optimize_data_layout();
    void apply_prefetch_hints();
    void align_hot_data();
    void group_related_data();
    
    // Cache-friendly code generation
    void emit_cache_optimized_loop(CPUCodeGenerator& codegen, 
                                  uint64_t array_base, 
                                  size_t element_size, 
                                  size_t count);
    
    void emit_streaming_instructions(CPUCodeGenerator& codegen,
                                   uint64_t src_addr,
                                   uint64_t dst_addr,
                                   size_t size);
    
    // Analysis
    double get_cache_hit_ratio(uint8_t level) const;
    std::vector<uint64_t> identify_cache_hotspots() const;
    void print_cache_statistics() const;
    
    // Recommendations
    struct CacheRecommendation {
        std::string description;
        uint32_t priority;
        double estimated_benefit;
    };
    
    std::vector<CacheRecommendation> get_optimization_recommendations() const;

private:
    bool is_cache_line_aligned(uint64_t address) const;
    uint64_t align_to_cache_line(uint64_t address) const;
    void analyze_access_patterns();
};

//=============================================================================
// Performance Counter Integration
//=============================================================================

class PerformanceCounters {
private:
    struct CounterInfo {
        std::string name;
        uint64_t value;
        uint64_t previous_value;
        double rate;
        bool enabled;
        
        CounterInfo() : value(0), previous_value(0), rate(0.0), enabled(false) {}
    };
    
    std::unordered_map<std::string, CounterInfo> counters_;
    bool monitoring_active_;
    std::chrono::high_resolution_clock::time_point last_update_;

public:
    PerformanceCounters();
    ~PerformanceCounters();
    
    // Counter management
    bool enable_counter(const std::string& name);
    void disable_counter(const std::string& name);
    void enable_all_available_counters();
    
    // Data collection
    void start_monitoring();
    void stop_monitoring();
    void update_counters();
    
    // Data access
    uint64_t get_counter_value(const std::string& name) const;
    double get_counter_rate(const std::string& name) const;
    std::vector<std::string> get_available_counters() const;
    
    // Analysis
    void print_counter_summary() const;
    void export_counter_data(const std::string& filename) const;
    
    // Intel-specific counters
    void setup_intel_counters();
    uint64_t get_instructions_retired() const;
    uint64_t get_cycles() const;
    double get_ipc() const; // Instructions per cycle
    
    // AMD-specific counters
    void setup_amd_counters();
    uint64_t get_branch_mispredictions() const;
    uint64_t get_cache_misses() const;

private:
    bool is_counter_available(const std::string& name) const;
    uint64_t read_performance_counter(const std::string& name) const;
    void configure_hardware_counters();
};

//=============================================================================
// CPU Optimization Integration
//=============================================================================

namespace CPUOptimizationIntegration {
    // System initialization
    void initialize_cpu_optimization();
    void shutdown_cpu_optimization();
    
    // Detection and configuration
    void detect_and_configure_cpu();
    void apply_cpu_specific_optimizations();
    void enable_performance_monitoring();
    
    // Code generation
    std::unique_ptr<CPUCodeGenerator> create_optimized_codegen();
    void optimize_existing_code();
    void apply_microarchitecture_tuning();
    
    // Performance monitoring
    void start_performance_profiling();
    void stop_performance_profiling();
    void analyze_performance_bottlenecks();
    
    // Optimization recommendations
    void print_optimization_recommendations();
    void apply_automatic_optimizations();
    
    // Utility functions
    std::string get_cpu_optimization_summary();
    void configure_for_maximum_performance();
    void configure_for_power_efficiency();
    void configure_for_balanced_performance();
}

} // namespace Quanta