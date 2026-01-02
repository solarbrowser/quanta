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



struct NUMANode {
    uint32_t node_id;
    uint64_t total_memory_bytes;
    uint64_t free_memory_bytes;
    std::vector<uint32_t> cpu_cores;
    std::vector<uint32_t> distances;
    
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
    
    bool detect_numa_topology();
    void force_refresh_topology();
    
    uint32_t get_node_count() const { return static_cast<uint32_t>(nodes_.size()); }
    const NUMANode& get_node(uint32_t node_id) const;
    uint32_t get_current_node() const { return local_node_id_; }
    bool is_numa_available() const { return numa_available_; }
    
    uint32_t get_distance(uint32_t from_node, uint32_t to_node) const;
    uint32_t get_closest_node_to(uint32_t reference_node) const;
    std::vector<uint32_t> get_nodes_by_distance(uint32_t from_node) const;
    
    uint64_t get_node_memory_size(uint32_t node_id) const;
    uint64_t get_node_free_memory(uint32_t node_id) const;
    double get_node_memory_utilization(uint32_t node_id) const;
    
    std::vector<uint32_t> get_node_cpus(uint32_t node_id) const;
    uint32_t get_cpu_node(uint32_t cpu_id) const;
    
    void print_topology() const;
    void benchmark_node_performance();
    
    static NUMATopology& get_instance();

private:
    void detect_windows_numa();
    void detect_linux_numa();
    void detect_distances();
    void benchmark_memory_bandwidth(uint32_t node_id);
    void benchmark_memory_latency(uint32_t node_id);
};


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
    
    enum class AllocationPolicy {
        LOCAL_ONLY,
        PREFERRED_LOCAL,
        INTERLEAVED,
        BANDWIDTH_OPTIMIZED,
        LATENCY_
    };
    
    AllocationPolicy current_policy_;
    uint32_t next_interleave_node_;
    
    mutable std::mutex allocator_mutex_;

public:
    NUMAAllocator();
    ~NUMAAllocator();
    
    void* allocate(size_t size, uint32_t preferred_node = UINT32_MAX);
    void* allocate_on_node(size_t size, uint32_t node_id);
    void* allocate_interleaved(size_t size);
    void deallocate(void* ptr);
    
    bool migrate_memory(void* ptr, uint32_t target_node);
    void* reallocate(void* ptr, size_t new_size);
    void prefault_memory(void* ptr, size_t size);
    
    void set_allocation_policy(AllocationPolicy policy) { current_policy_ = policy; }
    AllocationPolicy get_allocation_policy() const { return current_policy_; }
    
    uint32_t get_allocation_node(void* ptr) const;
    size_t get_allocation_size(void* ptr) const;
    uint64_t get_node_allocated_bytes(uint32_t node_id) const;
    
    void record_memory_access(void* ptr);
    std::vector<void*> get_hot_allocations() const;
    void print_allocation_statistics() const;
    
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
    
    bool bind_thread_to_node(std::thread::id thread_id, uint32_t node_id);
    bool bind_current_thread_to_node(uint32_t node_id);
    void unbind_thread(std::thread::id thread_id);
    
    void register_thread(std::thread::id thread_id, uint32_t preferred_node = UINT32_MAX);
    void unregister_thread(std::thread::id thread_id);
    
    uint32_t get_thread_node(std::thread::id thread_id) const;
    uint32_t get_current_thread_node() const;
    std::vector<uint32_t> get_thread_allowed_nodes(std::thread::id thread_id) const;
    
    void balance_threads_across_nodes();
    uint32_t get_least_loaded_node() const;
    uint32_t get_thread_count_on_node(uint32_t node_id) const;
    
    void record_memory_access(std::thread::id thread_id, uint32_t node_id);
    double get_thread_locality_ratio(std::thread::id thread_id) const;
    void print_thread_statistics() const;
    
    void optimize_thread_placement();
    void suggest_thread_migration();

private:
    bool platform_bind_thread(std::thread::id thread_id, uint32_t node_id);
    uint32_t platform_get_current_node();
};


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
    
    void start_monitoring();
    void stop_monitoring();
    void enable_monitoring() { monitoring_enabled_ = true; }
    void disable_monitoring() { monitoring_enabled_ = false; }
    
    void record_memory_access(uint32_t node_id, bool is_local, double latency_ns);
    void record_memory_migration(uint32_t from_node, uint32_t to_node);
    void record_thread_migration(uint32_t from_node, uint32_t to_node);
    
    double get_numa_efficiency() const;
    double get_memory_locality_ratio() const;
    double get_average_cross_node_latency() const;
    uint64_t get_total_migrations() const;
    
    void print_performance_summary() const;
    void print_detailed_metrics() const;
    void export_performance_data(const std::string& filename) const;
    
    std::vector<std::string> get_optimization_recommendations() const;

private:
    void monitoring_loop();
    void update_bandwidth_utilization();
    void analyze_access_patterns();
};


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
    
    bool initialize();
    void shutdown();
    
    void* allocate(size_t size, uint32_t preferred_node = UINT32_MAX);
    void deallocate(void* ptr);
    bool migrate_memory(void* ptr, uint32_t target_node);
    
    bool bind_current_thread_to_node(uint32_t node_id);
    uint32_t get_current_thread_node() const;
    
    void enable_performance_monitoring();
    void disable_performance_monitoring();
    double get_numa_efficiency() const;
    
    void enable_auto_optimization();
    void disable_auto_optimization();
    void run_optimization_cycle();
    
    void print_numa_status() const;
    void print_comprehensive_report() const;
    std::string get_numa_summary() const;
    
    static NUMAMemoryManager& get_instance();

private:
    void optimization_loop();
    void perform_automatic_optimizations();
};


namespace NUMAIntegration {
    void initialize_numa_system();
    void shutdown_numa_system();
    
    template<typename T>
    T* allocate_numa(size_t count = 1, uint32_t preferred_node = UINT32_MAX);
    
    template<typename T>
    void deallocate_numa(T* ptr);
    
    void bind_current_thread_to_local_node();
    void bind_current_thread_to_node(uint32_t node_id);
    
    void enable_numa_optimizations();
    void print_numa_recommendations();
    
    bool is_numa_available();
    uint32_t get_numa_node_count();
    uint32_t get_optimal_node_for_allocation(size_t size);
    
    void configure_for_compute_workload();
    void configure_for_memory_workload();
    void configure_for_balanced_workload();
}


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

}
