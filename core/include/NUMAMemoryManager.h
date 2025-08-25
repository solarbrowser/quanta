/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <vector>
#include <unordered_map>
#include <memory>
#include <atomic>
#include <mutex>
#include <thread>
#include <cstdint>

namespace Quanta {

//=============================================================================
// PHASE 3: Advanced Memory Management (NUMA Awareness)
//
// Real NUMA-aware memory management for maximum performance:
// - NUMA topology detection
// - Node-local memory allocation
// - Memory affinity optimization
// - Cross-node access minimization
// - Thread-to-node binding
// - Memory migration strategies
// - Bandwidth optimization
// - Latency minimization
//=============================================================================

//=============================================================================
// NUMA Topology Detection
//=============================================================================

struct NUMANode {
    uint32_t node_id;
    uint64_t total_memory_bytes;
    uint64_t free_memory_bytes;
    std::vector<uint32_t> cpu_cores;
    std::vector<uint32_t> distances; // Distance to other nodes
    
    double memory_bandwidth_gb_s;
    double memory_latency_ns;
    bool is_available;
    
    NUMANode() : node_id(0), total_memory_bytes(0), free_memory_bytes(0),
                memory_bandwidth_gb_s(0.0), memory_latency_ns(0.0), is_available(true) {}
};

class NUMATopology {
private:
    std::vector<NUMANode> nodes_;
    std::vector<std::vector<uint32_t>> distance_matrix_;
    uint32_t local_node_id_;
    bool numa_available_;
    
public:
    NUMATopology();
    ~NUMATopology();
    
    // Topology detection
    bool detect_numa_topology();
    void force_refresh_topology();
    
    // Node information
    uint32_t get_node_count() const { return static_cast<uint32_t>(nodes_.size()); }
    const NUMANode& get_node(uint32_t node_id) const;
    uint32_t get_current_node() const { return local_node_id_; }
    bool is_numa_available() const { return numa_available_; }
    
    // Distance and affinity
    uint32_t get_distance(uint32_t from_node, uint32_t to_node) const;
    uint32_t get_closest_node_to(uint32_t reference_node) const;
    std::vector<uint32_t> get_nodes_by_distance(uint32_t from_node) const;
    
    // Memory information
    uint64_t get_node_memory_size(uint32_t node_id) const;
    uint64_t get_node_free_memory(uint32_t node_id) const;
    double get_node_memory_utilization(uint32_t node_id) const;
    
    // CPU affinity
    std::vector<uint32_t> get_node_cpus(uint32_t node_id) const;
    uint32_t get_cpu_node(uint32_t cpu_id) const;
    
    // Diagnostics
    void print_topology() const;
    void benchmark_node_performance();
    
    // Singleton access
    static NUMATopology& get_instance();

private:
    void detect_windows_numa();
    void detect_linux_numa();
    void detect_distances();
    void benchmark_memory_bandwidth(uint32_t node_id);
    void benchmark_memory_latency(uint32_t node_id);
};

//=============================================================================
// NUMA-Aware Allocator
//=============================================================================

class NUMAAllocator {
private:
    struct AllocationInfo {
        void* address;
        size_t size;
        uint32_t node_id;
        uint64_t allocation_time;
        bool is_migrated;
        uint64_t access_count;
        
        AllocationInfo() : address(nullptr), size(0), node_id(0),
                          allocation_time(0), is_migrated(false), access_count(0) {}
    };
    
    std::unordered_map<void*, AllocationInfo> allocations_;
    std::vector<std::atomic<uint64_t>> node_allocated_bytes_;
    std::vector<std::mutex> node_mutexes_;
    
    // Allocation policies
    enum class AllocationPolicy {
        LOCAL_ONLY,           // Allocate on current node only
        PREFERRED_LOCAL,      // Prefer local, fallback to others
        INTERLEAVED,         // Round-robin across nodes
        BANDWIDTH_OPTIMIZED, // Choose based on bandwidth
        LATENCY_OPTIMIZED   // Choose based on latency
    };
    
    AllocationPolicy current_policy_;
    uint32_t next_interleave_node_;
    
    mutable std::mutex allocator_mutex_;

public:
    NUMAAllocator();
    ~NUMAAllocator();
    
    // Memory allocation
    void* allocate(size_t size, uint32_t preferred_node = UINT32_MAX);
    void* allocate_on_node(size_t size, uint32_t node_id);
    void* allocate_interleaved(size_t size);
    void deallocate(void* ptr);
    
    // Memory management
    bool migrate_memory(void* ptr, uint32_t target_node);
    void* reallocate(void* ptr, size_t new_size);
    void prefault_memory(void* ptr, size_t size);
    
    // Allocation policies
    void set_allocation_policy(AllocationPolicy policy) { current_policy_ = policy; }
    AllocationPolicy get_allocation_policy() const { return current_policy_; }
    
    // Memory information
    uint32_t get_allocation_node(void* ptr) const;
    size_t get_allocation_size(void* ptr) const;
    uint64_t get_node_allocated_bytes(uint32_t node_id) const;
    
    // Statistics
    void record_memory_access(void* ptr);
    std::vector<void*> get_hot_allocations() const;
    void print_allocation_statistics() const;
    
    // Optimization
    void optimize_allocations();
    void migrate_hot_data_to_local_nodes();
    void balance_memory_across_nodes();

private:
    uint32_t choose_optimal_node(size_t size) const;
    uint32_t choose_bandwidth_optimal_node() const;
    uint32_t choose_latency_optimal_node() const;
    void* platform_allocate_on_node(size_t size, uint32_t node_id);
    bool platform_migrate_memory(void* ptr, size_t size, uint32_t target_node);
};

//=============================================================================
// NUMA Thread Affinity Manager
//=============================================================================

class NUMAThreadManager {
private:
    struct ThreadInfo {
        std::thread::id thread_id;
        uint32_t assigned_node;
        uint32_t preferred_node;
        std::vector<uint32_t> allowed_nodes;
        uint64_t memory_accesses;
        uint64_t cross_node_accesses;
        bool is_bound;
        
        ThreadInfo() : assigned_node(UINT32_MAX), preferred_node(UINT32_MAX),
                      memory_accesses(0), cross_node_accesses(0), is_bound(false) {}
    };
    
    std::unordered_map<std::thread::id, ThreadInfo> threads_;
    std::vector<std::atomic<uint32_t>> node_thread_counts_;
    
    mutable std::mutex manager_mutex_;

public:
    NUMAThreadManager();
    ~NUMAThreadManager();
    
    // Thread affinity
    bool bind_thread_to_node(std::thread::id thread_id, uint32_t node_id);
    bool bind_current_thread_to_node(uint32_t node_id);
    void unbind_thread(std::thread::id thread_id);
    
    // Thread management
    void register_thread(std::thread::id thread_id, uint32_t preferred_node = UINT32_MAX);
    void unregister_thread(std::thread::id thread_id);
    
    // Affinity queries
    uint32_t get_thread_node(std::thread::id thread_id) const;
    uint32_t get_current_thread_node() const;
    std::vector<uint32_t> get_thread_allowed_nodes(std::thread::id thread_id) const;
    
    // Load balancing
    void balance_threads_across_nodes();
    uint32_t get_least_loaded_node() const;
    uint32_t get_thread_count_on_node(uint32_t node_id) const;
    
    // Statistics
    void record_memory_access(std::thread::id thread_id, uint32_t node_id);
    double get_thread_locality_ratio(std::thread::id thread_id) const;
    void print_thread_statistics() const;
    
    // Optimization
    void optimize_thread_placement();
    void suggest_thread_migration();

private:
    bool platform_bind_thread(std::thread::id thread_id, uint32_t node_id);
    uint32_t platform_get_current_node();
};

//=============================================================================
// NUMA Performance Monitor
//=============================================================================

class NUMAPerformanceMonitor {
private:
    struct PerformanceMetrics {
        uint64_t local_memory_accesses;
        uint64_t remote_memory_accesses;
        uint64_t memory_migrations;
        uint64_t thread_migrations;
        
        double average_local_latency_ns;
        double average_remote_latency_ns;
        double memory_bandwidth_utilization;
        
        PerformanceMetrics() : local_memory_accesses(0), remote_memory_accesses(0),
                              memory_migrations(0), thread_migrations(0),
                              average_local_latency_ns(0.0), average_remote_latency_ns(0.0),
                              memory_bandwidth_utilization(0.0) {}
    };
    
    std::vector<PerformanceMetrics> node_metrics_;
    std::atomic<bool> monitoring_enabled_{true};
    std::thread monitoring_thread_;
    std::atomic<bool> should_stop_{false};
    
    mutable std::mutex metrics_mutex_;

public:
    NUMAPerformanceMonitor();
    ~NUMAPerformanceMonitor();
    
    // Monitoring control
    void start_monitoring();
    void stop_monitoring();
    void enable_monitoring() { monitoring_enabled_ = true; }
    void disable_monitoring() { monitoring_enabled_ = false; }
    
    // Metric recording
    void record_memory_access(uint32_t node_id, bool is_local, double latency_ns);
    void record_memory_migration(uint32_t from_node, uint32_t to_node);
    void record_thread_migration(uint32_t from_node, uint32_t to_node);
    
    // Performance analysis
    double get_numa_efficiency() const;
    double get_memory_locality_ratio() const;
    double get_average_cross_node_latency() const;
    uint64_t get_total_migrations() const;
    
    // Reporting
    void print_performance_summary() const;
    void print_detailed_metrics() const;
    void export_performance_data(const std::string& filename) const;
    
    // Recommendations
    std::vector<std::string> get_optimization_recommendations() const;

private:
    void monitoring_loop();
    void update_bandwidth_utilization();
    void analyze_access_patterns();
};

//=============================================================================
// NUMA Memory Manager Integration
//=============================================================================

class NUMAMemoryManager {
private:
    std::unique_ptr<NUMAAllocator> allocator_;
    std::unique_ptr<NUMAThreadManager> thread_manager_;
    std::unique_ptr<NUMAPerformanceMonitor> performance_monitor_;
    
    bool auto_optimization_enabled_;
    std::thread optimization_thread_;
    std::atomic<bool> should_stop_optimization_{false};

public:
    NUMAMemoryManager();
    ~NUMAMemoryManager();
    
    // Initialization
    bool initialize();
    void shutdown();
    
    // Memory operations (delegated to allocator)
    void* allocate(size_t size, uint32_t preferred_node = UINT32_MAX);
    void deallocate(void* ptr);
    bool migrate_memory(void* ptr, uint32_t target_node);
    
    // Thread operations (delegated to thread manager)
    bool bind_current_thread_to_node(uint32_t node_id);
    uint32_t get_current_thread_node() const;
    
    // Performance monitoring
    void enable_performance_monitoring();
    void disable_performance_monitoring();
    double get_numa_efficiency() const;
    
    // Auto-optimization
    void enable_auto_optimization();
    void disable_auto_optimization();
    void run_optimization_cycle();
    
    // Information and diagnostics
    void print_numa_status() const;
    void print_comprehensive_report() const;
    std::string get_numa_summary() const;
    
    // Singleton access
    static NUMAMemoryManager& get_instance();

private:
    void optimization_loop();
    void perform_automatic_optimizations();
};

//=============================================================================
// NUMA Integration Helpers
//=============================================================================

namespace NUMAIntegration {
    // System initialization
    void initialize_numa_system();
    void shutdown_numa_system();
    
    // Memory allocation helpers
    template<typename T>
    T* allocate_numa(size_t count = 1, uint32_t preferred_node = UINT32_MAX);
    
    template<typename T>
    void deallocate_numa(T* ptr);
    
    // Thread affinity helpers
    void bind_current_thread_to_local_node();
    void bind_current_thread_to_node(uint32_t node_id);
    
    // Performance helpers
    void enable_numa_optimizations();
    void print_numa_recommendations();
    
    // Utility functions
    bool is_numa_available();
    uint32_t get_numa_node_count();
    uint32_t get_optimal_node_for_allocation(size_t size);
    
    // Automatic optimization
    void configure_for_compute_workload();
    void configure_for_memory_workload();
    void configure_for_balanced_workload();
}

//=============================================================================
// Template Implementation
//=============================================================================

template<typename T>
T* NUMAIntegration::allocate_numa(size_t count, uint32_t preferred_node) {
    auto& manager = NUMAMemoryManager::get_instance();
    void* ptr = manager.allocate(sizeof(T) * count, preferred_node);
    return static_cast<T*>(ptr);
}

template<typename T>
void NUMAIntegration::deallocate_numa(T* ptr) {
    auto& manager = NUMAMemoryManager::get_instance();
    manager.deallocate(ptr);
}

} // namespace Quanta