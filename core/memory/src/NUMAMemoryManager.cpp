/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../include/NUMAMemoryManager.h"
#include <iostream>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>

#ifdef _WIN32
#include <cstring>
#include <windows.h>
#include <winbase.h>
#elif defined(__linux__)
#include <numa.h>
#include <numaif.h>
#include <sched.h>
#include <unistd.h>
#endif

namespace Quanta {

//=============================================================================
// NUMATopology Implementation
//=============================================================================

NUMATopology::NUMATopology() : local_node_id_(0), numa_available_(false) {
    std::cout << "� NUMA TOPOLOGY DETECTOR INITIALIZED" << std::endl;
}

NUMATopology::~NUMATopology() {
    std::cout << "� NUMA TOPOLOGY DETECTOR SHUTDOWN" << std::endl;
}

bool NUMATopology::detect_numa_topology() {
    std::cout << "� Detecting NUMA topology..." << std::endl;
    
    nodes_.clear();
    distance_matrix_.clear();
    
#ifdef _WIN32
    detect_windows_numa();
#elif defined(__linux__)
    detect_linux_numa();
#else
    // Fallback: assume single node
    numa_available_ = false;
    NUMANode single_node;
    single_node.node_id = 0;
    single_node.total_memory_bytes = 8ULL * 1024 * 1024 * 1024; // 8GB default
    single_node.free_memory_bytes = 4ULL * 1024 * 1024 * 1024;  // 4GB free
    single_node.cpu_cores = {0, 1, 2, 3}; // 4 cores default
    single_node.memory_bandwidth_gb_s = 25.0;
    single_node.memory_latency_ns = 100.0;
    nodes_.push_back(single_node);
#endif
    
    if (nodes_.empty()) {
        std::cout << "⚠️  No NUMA nodes detected, using single-node fallback" << std::endl;
        numa_available_ = false;
        return false;
    }
    
    detect_distances();
    
    std::cout << " NUMA topology detected:" << std::endl;
    std::cout << "  Nodes: " << nodes_.size() << std::endl;
    std::cout << "  NUMA available: " << (numa_available_ ? "YES" : "NO") << std::endl;
    
    return true;
}

#ifdef _WIN32
void NUMATopology::detect_windows_numa() {
    ULONG highest_node;
    if (!GetNumaHighestNodeNumber(&highest_node)) {
        std::cout << "⚠️  GetNumaHighestNodeNumber failed" << std::endl;
        return;
    }
    
    numa_available_ = (highest_node > 0);
    
    for (ULONG node_id = 0; node_id <= highest_node; ++node_id) {
        NUMANode node;
        node.node_id = node_id;
        
        // Get node memory information
        ULONGLONG available_bytes;
        if (GetNumaAvailableMemoryNodeEx(node_id, &available_bytes)) {
            node.free_memory_bytes = available_bytes;
            node.total_memory_bytes = available_bytes * 2; // Estimate
        }
        
        // Get processor mask for this node
        GROUP_AFFINITY group_affinity;
        if (GetNumaNodeProcessorMaskEx(node_id, &group_affinity)) {
            // Count set bits in the mask to get CPU cores
            KAFFINITY mask = group_affinity.Mask;
            for (int cpu = 0; cpu < 64; ++cpu) {
                if (mask & (1ULL << cpu)) {
                    node.cpu_cores.push_back(cpu);
                }
            }
        }
        
        node.memory_bandwidth_gb_s = 25.0; // Default estimate
        node.memory_latency_ns = 100.0;    // Default estimate
        node.is_available = true;
        
        nodes_.push_back(node);
        
        std::cout << "  Node " << node_id << ": " << node.cpu_cores.size() 
                 << " CPUs, " << (node.free_memory_bytes / (1024*1024)) << " MB free" << std::endl;
    }
}
#endif

#ifdef __linux__
void NUMATopology::detect_linux_numa() {
    if (numa_available() == -1) {
        std::cout << "⚠️  NUMA not available on this system" << std::endl;
        numa_available_ = false;
        return;
    }
    
    numa_available_ = true;
    int max_node = numa_max_node();
    
    for (int node_id = 0; node_id <= max_node; ++node_id) {
        if (numa_bitmask_isbitset(numa_nodes_ptr, node_id)) {
            NUMANode node;
            node.node_id = node_id;
            
            // Get memory size
            long long node_size = numa_node_size64(node_id, nullptr);
            if (node_size > 0) {
                node.total_memory_bytes = node_size;
                node.free_memory_bytes = node_size / 2; // Estimate
            }
            
            // Get CPUs for this node
            struct bitmask* cpu_mask = numa_allocate_cpumask();
            if (numa_node_to_cpus(node_id, cpu_mask) == 0) {
                for (int cpu = 0; cpu < numa_num_possible_cpus(); ++cpu) {
                    if (numa_bitmask_isbitset(cpu_mask, cpu)) {
                        node.cpu_cores.push_back(cpu);
                    }
                }
            }
            numa_free_cpumask(cpu_mask);
            
            node.memory_bandwidth_gb_s = 25.0; // Would need benchmarking
            node.memory_latency_ns = 100.0;    // Would need benchmarking
            node.is_available = true;
            
            nodes_.push_back(node);
            
            std::cout << "  Node " << node_id << ": " << node.cpu_cores.size() 
                     << " CPUs, " << (node.total_memory_bytes / (1024*1024*1024)) << " GB" << std::endl;
        }
    }
}
#endif

void NUMATopology::detect_distances() {
    size_t node_count = nodes_.size();
    distance_matrix_.resize(node_count);
    
    for (size_t i = 0; i < node_count; ++i) {
        distance_matrix_[i].resize(node_count);
        nodes_[i].distances.resize(node_count);
        
        for (size_t j = 0; j < node_count; ++j) {
            uint32_t distance;
            
#ifdef __linux__
            if (numa_available_) {
                distance = numa_distance(i, j);
            } else {
                distance = (i == j) ? 10 : 20; // Default distances
            }
#else
            distance = (i == j) ? 10 : 20; // Default distances
#endif
            
            distance_matrix_[i][j] = distance;
            nodes_[i].distances[j] = distance;
        }
    }
}

const NUMANode& NUMATopology::get_node(uint32_t node_id) const {
    if (node_id >= nodes_.size()) {
        throw std::out_of_range("Invalid NUMA node ID");
    }
    return nodes_[node_id];
}

uint32_t NUMATopology::get_distance(uint32_t from_node, uint32_t to_node) const {
    if (from_node >= nodes_.size() || to_node >= nodes_.size()) {
        return UINT32_MAX;
    }
    return distance_matrix_[from_node][to_node];
}

void NUMATopology::print_topology() const {
    std::cout << "� NUMA TOPOLOGY" << std::endl;
    std::cout << "================" << std::endl;
    std::cout << "NUMA Available: " << (numa_available_ ? "YES" : "NO") << std::endl;
    std::cout << "Node Count: " << nodes_.size() << std::endl;
    std::cout << "Current Node: " << local_node_id_ << std::endl;
    
    for (const auto& node : nodes_) {
        std::cout << "\nNode " << node.node_id << ":" << std::endl;
        std::cout << "  Memory: " << (node.total_memory_bytes / (1024*1024*1024)) << " GB total, "
                 << (node.free_memory_bytes / (1024*1024*1024)) << " GB free" << std::endl;
        std::cout << "  CPUs: ";
        for (size_t i = 0; i < node.cpu_cores.size(); ++i) {
            if (i > 0) std::cout << ", ";
            std::cout << node.cpu_cores[i];
        }
        std::cout << std::endl;
        std::cout << "  Bandwidth: " << node.memory_bandwidth_gb_s << " GB/s" << std::endl;
        std::cout << "  Latency: " << node.memory_latency_ns << " ns" << std::endl;
    }
    
    if (nodes_.size() > 1) {
        std::cout << "\nDistance Matrix:" << std::endl;
        std::cout << "     ";
        for (size_t i = 0; i < nodes_.size(); ++i) {
            std::cout << std::setw(4) << i;
        }
        std::cout << std::endl;
        
        for (size_t i = 0; i < nodes_.size(); ++i) {
            std::cout << std::setw(4) << i << ":";
            for (size_t j = 0; j < nodes_.size(); ++j) {
                std::cout << std::setw(4) << distance_matrix_[i][j];
            }
            std::cout << std::endl;
        }
    }
}

NUMATopology& NUMATopology::get_instance() {
    static NUMATopology instance;
    return instance;
}

//=============================================================================
// NUMAAllocator Implementation
//=============================================================================

NUMAAllocator::NUMAAllocator() 
    : current_policy_(AllocationPolicy::PREFERRED_LOCAL), next_interleave_node_(0) {
    
    auto& topology = NUMATopology::get_instance();
    uint32_t node_count = topology.get_node_count();
    
    node_allocated_bytes_.resize(node_count);
    node_mutexes_.clear();
    node_mutexes_.reserve(node_count);

    for (uint32_t i = 0; i < node_count; ++i) {
        node_allocated_bytes_[i] = 0;
        node_mutexes_.emplace_back(std::make_unique<std::mutex>());
    }
    
    std::cout << "� NUMA ALLOCATOR INITIALIZED (" << node_count << " nodes)" << std::endl;
}

NUMAAllocator::~NUMAAllocator() {
    print_allocation_statistics();
    std::cout << "� NUMA ALLOCATOR SHUTDOWN" << std::endl;
}

void* NUMAAllocator::allocate(size_t size, uint32_t preferred_node) {
    uint32_t target_node;
    
    if (preferred_node != UINT32_MAX) {
        target_node = preferred_node;
    } else {
        target_node = choose_optimal_node(size);
    }
    
    void* ptr = platform_allocate_on_node(size, target_node);
    
    if (ptr) {
        std::lock_guard<std::mutex> lock(allocator_mutex_);
        
        AllocationInfo info;
        info.address = ptr;
        info.size = size;
        info.node_id = target_node;
        info.allocation_time = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count();
        
        allocations_[ptr] = info;
        node_allocated_bytes_[target_node] += size;
        
        std::cout << "� Allocated " << size << " bytes on node " << target_node 
                 << " at " << ptr << std::endl;
    }
    
    return ptr;
}

void* NUMAAllocator::platform_allocate_on_node(size_t size, uint32_t node_id) {
#ifdef _WIN32
    return VirtualAllocExNuma(GetCurrentProcess(), nullptr, size,
                             MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE, node_id);
#elif defined(__linux__)
    if (numa_available() != -1) {
        return numa_alloc_onnode(size, node_id);
    } else {
        return malloc(size);
    }
#else
    return malloc(size);
#endif
}

void NUMAAllocator::deallocate(void* ptr) {
    if (!ptr) return;
    
    std::lock_guard<std::mutex> lock(allocator_mutex_);
    
    auto it = allocations_.find(ptr);
    if (it != allocations_.end()) {
        const auto& info = it->second;
        
        node_allocated_bytes_[info.node_id] -= info.size;
        
        std::cout << "� Deallocated " << info.size << " bytes from node " 
                 << info.node_id << " at " << ptr << std::endl;
        
        allocations_.erase(it);
    }
    
#ifdef _WIN32
    VirtualFree(ptr, 0, MEM_RELEASE);
#elif defined(__linux__)
    if (numa_available() != -1) {
        numa_free(ptr, 0); // Size will be tracked internally by numa
    } else {
        free(ptr);
    }
#else
    free(ptr);
#endif
}

uint32_t NUMAAllocator::choose_optimal_node(size_t size) const {
    auto& topology = NUMATopology::get_instance();
    
    switch (current_policy_) {
        case AllocationPolicy::LOCAL_ONLY:
            return topology.get_current_node();
            
        case AllocationPolicy::PREFERRED_LOCAL: {
            uint32_t local_node = topology.get_current_node();
            if (topology.get_node_free_memory(local_node) >= size) {
                return local_node;
            }
            // Fallback to node with most free memory
            uint32_t best_node = 0;
            uint64_t max_free = 0;
            for (uint32_t i = 0; i < topology.get_node_count(); ++i) {
                uint64_t free = topology.get_node_free_memory(i);
                if (free > max_free) {
                    max_free = free;
                    best_node = i;
                }
            }
            return best_node;
        }
        
        case AllocationPolicy::INTERLEAVED:
            return next_interleave_node_ % topology.get_node_count();
            
        case AllocationPolicy::BANDWIDTH_OPTIMIZED:
            return choose_bandwidth_optimal_node();
            
        case AllocationPolicy::LATENCY_OPTIMIZED:
            return choose_latency_optimal_node();
            
        default:
            return topology.get_current_node();
    }
}

uint32_t NUMAAllocator::choose_bandwidth_optimal_node() const {
    auto& topology = NUMATopology::get_instance();
    uint32_t best_node = 0;
    double best_bandwidth = 0.0;
    
    for (uint32_t i = 0; i < topology.get_node_count(); ++i) {
        const auto& node = topology.get_node(i);
        if (node.memory_bandwidth_gb_s > best_bandwidth) {
            best_bandwidth = node.memory_bandwidth_gb_s;
            best_node = i;
        }
    }
    
    return best_node;
}

uint32_t NUMAAllocator::choose_latency_optimal_node() const {
    auto& topology = NUMATopology::get_instance();
    uint32_t best_node = 0;
    double best_latency = std::numeric_limits<double>::max();
    
    for (uint32_t i = 0; i < topology.get_node_count(); ++i) {
        const auto& node = topology.get_node(i);
        if (node.memory_latency_ns < best_latency) {
            best_latency = node.memory_latency_ns;
            best_node = i;
        }
    }
    
    return best_node;
}

void NUMAAllocator::print_allocation_statistics() const {
    std::lock_guard<std::mutex> lock(allocator_mutex_);
    
    std::cout << "� NUMA ALLOCATION STATISTICS" << std::endl;
    std::cout << "=============================" << std::endl;
    std::cout << "Active allocations: " << allocations_.size() << std::endl;
    std::cout << "Current policy: " << static_cast<int>(current_policy_) << std::endl;
    
    auto& topology = NUMATopology::get_instance();
    
    for (uint32_t i = 0; i < topology.get_node_count(); ++i) {
        uint64_t allocated = node_allocated_bytes_[i];
        std::cout << "Node " << i << ": " << (allocated / (1024*1024)) << " MB allocated" << std::endl;
    }
    
    uint64_t total_allocated = 0;
    for (uint32_t i = 0; i < topology.get_node_count(); ++i) {
        total_allocated += node_allocated_bytes_[i];
    }
    std::cout << "Total allocated: " << (total_allocated / (1024*1024)) << " MB" << std::endl;
}

//=============================================================================
// NUMAMemoryManager Implementation
//=============================================================================

NUMAMemoryManager::NUMAMemoryManager() : auto_optimization_enabled_(false) {
    std::cout << "� NUMA MEMORY MANAGER INITIALIZED" << std::endl;
}

NUMAMemoryManager::~NUMAMemoryManager() {
    shutdown();
    std::cout << "� NUMA MEMORY MANAGER SHUTDOWN" << std::endl;
}

bool NUMAMemoryManager::initialize() {
    std::cout << "� Initializing NUMA memory management..." << std::endl;
    
    // Initialize topology
    auto& topology = NUMATopology::get_instance();
    if (!topology.detect_numa_topology()) {
        std::cout << "⚠️  NUMA not available, using fallback mode" << std::endl;
    }
    
    // Create components
    allocator_ = std::make_unique<NUMAAllocator>();
    thread_manager_ = std::make_unique<NUMAThreadManager>();
    performance_monitor_ = std::make_unique<NUMAPerformanceMonitor>();
    
    // Start performance monitoring
    performance_monitor_->start_monitoring();
    
    std::cout << " NUMA memory management initialized" << std::endl;
    topology.print_topology();
    
    return true;
}

void NUMAMemoryManager::shutdown() {
    if (auto_optimization_enabled_) {
        disable_auto_optimization();
    }
    
    if (performance_monitor_) {
        performance_monitor_->stop_monitoring();
    }
    
    allocator_.reset();
    thread_manager_.reset();
    performance_monitor_.reset();
}

void* NUMAMemoryManager::allocate(size_t size, uint32_t preferred_node) {
    return allocator_ ? allocator_->allocate(size, preferred_node) : nullptr;
}

void NUMAMemoryManager::deallocate(void* ptr) {
    if (allocator_) {
        allocator_->deallocate(ptr);
    }
}

void NUMAMemoryManager::print_numa_status() const {
    auto& topology = NUMATopology::get_instance();
    topology.print_topology();
    
    if (allocator_) {
        allocator_->print_allocation_statistics();
    }
    
    if (performance_monitor_) {
        performance_monitor_->print_performance_summary();
    }
}

std::string NUMAMemoryManager::get_numa_summary() const {
    auto& topology = NUMATopology::get_instance();
    
    std::string summary = "NUMA Summary:\n";
    summary += "- Available: " + std::string(topology.is_numa_available() ? "YES" : "NO") + "\n";
    summary += "- Nodes: " + std::to_string(topology.get_node_count()) + "\n";
    summary += "- Current node: " + std::to_string(topology.get_current_node()) + "\n";
    
    return summary;
}

NUMAMemoryManager& NUMAMemoryManager::get_instance() {
    static NUMAMemoryManager instance;
    return instance;
}

//=============================================================================
// NUMAThreadManager Implementation
//=============================================================================

NUMAThreadManager::NUMAThreadManager() {
    auto& topology = NUMATopology::get_instance();
    node_thread_counts_.resize(topology.get_node_count());
    
    for (auto& count : node_thread_counts_) {
        count = 0;
    }
    
    std::cout << "� NUMA THREAD MANAGER INITIALIZED" << std::endl;
}

NUMAThreadManager::~NUMAThreadManager() {
    print_thread_statistics();
    std::cout << "� NUMA THREAD MANAGER SHUTDOWN" << std::endl;
}

void NUMAThreadManager::print_thread_statistics() const {
    std::lock_guard<std::mutex> lock(manager_mutex_);
    
    std::cout << "� NUMA THREAD STATISTICS" << std::endl;
    std::cout << "=========================" << std::endl;
    std::cout << "Registered threads: " << threads_.size() << std::endl;
    
    auto& topology = NUMATopology::get_instance();
    for (uint32_t i = 0; i < topology.get_node_count(); ++i) {
        std::cout << "Node " << i << " threads: " << node_thread_counts_[i] << std::endl;
    }
}

//=============================================================================
// NUMAPerformanceMonitor Implementation
//=============================================================================

NUMAPerformanceMonitor::NUMAPerformanceMonitor() {
    auto& topology = NUMATopology::get_instance();
    node_metrics_.resize(topology.get_node_count());
    
    std::cout << "� NUMA PERFORMANCE MONITOR INITIALIZED" << std::endl;
}

NUMAPerformanceMonitor::~NUMAPerformanceMonitor() {
    stop_monitoring();
    print_performance_summary();
    std::cout << "� NUMA PERFORMANCE MONITOR SHUTDOWN" << std::endl;
}

void NUMAPerformanceMonitor::start_monitoring() {
    if (!should_stop_) return; // Already running
    
    should_stop_ = false;
    monitoring_thread_ = std::thread(&NUMAPerformanceMonitor::monitoring_loop, this);
    std::cout << "� NUMA performance monitoring started" << std::endl;
}

void NUMAPerformanceMonitor::stop_monitoring() {
    should_stop_ = true;
    if (monitoring_thread_.joinable()) {
        monitoring_thread_.join();
    }
    std::cout << "� NUMA performance monitoring stopped" << std::endl;
}

void NUMAPerformanceMonitor::monitoring_loop() {
    while (!should_stop_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        if (monitoring_enabled_) {
            update_bandwidth_utilization();
            analyze_access_patterns();
        }
    }
}

void NUMAPerformanceMonitor::update_bandwidth_utilization() {
    // This would collect real performance counters in a full implementation
}

void NUMAPerformanceMonitor::analyze_access_patterns() {
    // This would analyze memory access patterns in a full implementation
}

void NUMAPerformanceMonitor::print_performance_summary() const {
    std::cout << "� NUMA PERFORMANCE SUMMARY" << std::endl;
    std::cout << "===========================" << std::endl;
    
    for (size_t i = 0; i < node_metrics_.size(); ++i) {
        const auto& metrics = node_metrics_[i];
        std::cout << "Node " << i << ":" << std::endl;
        std::cout << "  Local accesses: " << metrics.local_memory_accesses << std::endl;
        std::cout << "  Remote accesses: " << metrics.remote_memory_accesses << std::endl;
        std::cout << "  Memory migrations: " << metrics.memory_migrations << std::endl;
        
        if (metrics.local_memory_accesses + metrics.remote_memory_accesses > 0) {
            double locality = static_cast<double>(metrics.local_memory_accesses) /
                             (metrics.local_memory_accesses + metrics.remote_memory_accesses);
            std::cout << "  Locality ratio: " << (locality * 100.0) << "%" << std::endl;
        }
    }
}

//=============================================================================
// NUMAIntegration Implementation
//=============================================================================

namespace NUMAIntegration {

void initialize_numa_system() {
    std::cout << "� INITIALIZING NUMA SYSTEM" << std::endl;
    
    auto& manager = NUMAMemoryManager::get_instance();
    manager.initialize();
    
    std::cout << " NUMA SYSTEM INITIALIZED" << std::endl;
    std::cout << "  � Topology detection: Complete" << std::endl;
    std::cout << "  � NUMA allocator: Ready" << std::endl;
    std::cout << "  � Thread manager: Ready" << std::endl;
    std::cout << "  � Performance monitor: Active" << std::endl;
}

void shutdown_numa_system() {
    std::cout << "� SHUTTING DOWN NUMA SYSTEM" << std::endl;
    
    auto& manager = NUMAMemoryManager::get_instance();
    manager.shutdown();
    
    std::cout << " NUMA SYSTEM SHUTDOWN COMPLETE" << std::endl;
}

bool is_numa_available() {
    auto& topology = NUMATopology::get_instance();
    return topology.is_numa_available();
}

uint32_t get_numa_node_count() {
    auto& topology = NUMATopology::get_instance();
    return topology.get_node_count();
}

void print_numa_recommendations() {
    std::cout << "� NUMA OPTIMIZATION RECOMMENDATIONS" << std::endl;
    std::cout << "====================================" << std::endl;
    
    auto& topology = NUMATopology::get_instance();
    
    if (!topology.is_numa_available()) {
        std::cout << "  No NUMA optimizations needed (single node system)" << std::endl;
        return;
    }
    
    std::cout << "  1. Bind threads to specific NUMA nodes" << std::endl;
    std::cout << "  2. Allocate memory on the same node as threads" << std::endl;
    std::cout << "  3. Minimize cross-node memory access" << std::endl;
    std::cout << "  4. Use NUMA-aware data structures" << std::endl;
    std::cout << "  5. Monitor memory access patterns" << std::endl;
    std::cout << "  6. Consider memory migration for hot data" << std::endl;
}

} // namespace NUMAIntegration

} // namespace Quanta