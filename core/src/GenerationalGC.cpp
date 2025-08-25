/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../include/GenerationalGC.h"
#include "../include/PhotonCore/PhotonCoreQuantum.h"
#include "../include/PhotonCore/PhotonCoreSonic.h"
#include "../include/PhotonCore/PhotonCorePerformance.h"
#include <iostream>
#include <algorithm>
#include <cstdlib>

namespace Quanta {

//=============================================================================
// MemoryRegion Implementation - PHASE 2: Generational Memory Management
//=============================================================================

MemoryRegion::MemoryRegion(Generation gen, size_t size) 
    : generation_(gen), total_size_(size), used_size_(0) {
    
    // Allocate memory region (using malloc for compatibility)
    memory_start_ = std::malloc(size);
    if (!memory_start_) {
        throw std::bad_alloc();
    }
    
    memory_end_ = static_cast<char*>(memory_start_) + size;
    allocation_pointer_ = memory_start_;
    
    std::cout << "🧠 MEMORY REGION CREATED: " 
             << (gen == Generation::YOUNG ? "YOUNG" : 
                 gen == Generation::OLD ? "OLD" : "PERMANENT")
             << " (" << (size / 1024 / 1024) << " MB)" << std::endl;
}

MemoryRegion::~MemoryRegion() {
    if (memory_start_) {
        std::free(memory_start_);
    }
}

GCObjectHeader* MemoryRegion::allocate(size_t size) {
    // Align size to 8 bytes
    size_t aligned_size = (size + 7) & ~7;
    
    if (!can_allocate(aligned_size + sizeof(GCObjectHeader))) {
        return nullptr; // Out of memory
    }
    
    // Allocate header + object space
    GCObjectHeader* header = static_cast<GCObjectHeader*>(allocation_pointer_);
    void* object_space = static_cast<char*>(allocation_pointer_) + sizeof(GCObjectHeader);
    
    // Initialize header (object will be set later)
    new (header) GCObjectHeader(nullptr, aligned_size);
    header->generation = generation_;
    
    // Update allocation pointer
    allocation_pointer_ = static_cast<char*>(allocation_pointer_) + sizeof(GCObjectHeader) + aligned_size;
    used_size_ += sizeof(GCObjectHeader) + aligned_size;
    
    objects_.push_back(header);
    
    return header;
}

bool MemoryRegion::can_allocate(size_t size) const {
    return (static_cast<char*>(allocation_pointer_) + size) <= memory_end_;
}

void MemoryRegion::add_object(GCObjectHeader* header) {
    objects_.push_back(header);
}

void MemoryRegion::remove_object(GCObjectHeader* header) {
    auto it = std::find(objects_.begin(), objects_.end(), header);
    if (it != objects_.end()) {
        objects_.erase(it);
    }
}

void MemoryRegion::mark_objects() {
    for (GCObjectHeader* header : objects_) {
        if (header && header->object) {
            header->is_marked = true;
        }
    }
}

size_t MemoryRegion::sweep_objects() {
    size_t collected = 0;
    
    auto it = objects_.begin();
    while (it != objects_.end()) {
        GCObjectHeader* header = *it;
        
        if (!header->is_marked) {
            // Object is not marked - collect it
            collected++;
            it = objects_.erase(it);
        } else {
            // Reset mark for next GC cycle
            header->is_marked = false;
            header->age++;
            ++it;
        }
    }
    
    return collected;
}

void MemoryRegion::compact_memory() {
    // Simple compaction - in a full implementation this would be more sophisticated
    std::cout << "🗜️  COMPACTING " 
             << (generation_ == Generation::YOUNG ? "YOUNG" : 
                 generation_ == Generation::OLD ? "OLD" : "PERMANENT")
             << " GENERATION" << std::endl;
}

void MemoryRegion::print_statistics() const {
    std::cout << "📊 Memory Region Statistics (" 
             << (generation_ == Generation::YOUNG ? "YOUNG" : 
                 generation_ == Generation::OLD ? "OLD" : "PERMANENT") << "):" << std::endl;
    std::cout << "  Total Size: " << (total_size_ / 1024 / 1024) << " MB" << std::endl;
    std::cout << "  Used Size: " << (used_size_ / 1024 / 1024) << " MB" << std::endl;
    std::cout << "  Free Size: " << (get_free_size() / 1024 / 1024) << " MB" << std::endl;
    std::cout << "  Utilization: " << (get_utilization() * 100.0) << "%" << std::endl;
    std::cout << "  Object Count: " << objects_.size() << std::endl;
}

//=============================================================================
// RememberedSet Implementation - Inter-generational Reference Tracking
//=============================================================================

RememberedSet::RememberedSet() {
    // Initialize
}

RememberedSet::~RememberedSet() {
    // Cleanup
}

void RememberedSet::add_reference(GCObjectHeader* from, GCObjectHeader* to) {
    if (!from || !to) return;
    
    // Track references from older to younger generations
    if (from->generation == Generation::OLD && to->generation == Generation::YOUNG) {
        old_to_young_refs_.insert(from);
        from->is_remembered = true;
    } else if (from->generation == Generation::PERMANENT && to->generation == Generation::YOUNG) {
        permanent_to_young_refs_.insert(from);
        from->is_remembered = true;
    } else if (from->generation == Generation::PERMANENT && to->generation == Generation::OLD) {
        permanent_to_old_refs_.insert(from);
        from->is_remembered = true;
    }
}

void RememberedSet::remove_reference(GCObjectHeader* from, GCObjectHeader* to) {
    if (!from || !to) return;
    
    if (from->generation == Generation::OLD && to->generation == Generation::YOUNG) {
        old_to_young_refs_.erase(from);
    } else if (from->generation == Generation::PERMANENT && to->generation == Generation::YOUNG) {
        permanent_to_young_refs_.erase(from);
    } else if (from->generation == Generation::PERMANENT && to->generation == Generation::OLD) {
        permanent_to_old_refs_.erase(from);
    }
}

void RememberedSet::clear() {
    old_to_young_refs_.clear();
    permanent_to_young_refs_.clear();
    permanent_to_old_refs_.clear();
}

std::vector<GCObjectHeader*> RememberedSet::get_young_roots() const {
    std::vector<GCObjectHeader*> roots;
    
    for (GCObjectHeader* header : old_to_young_refs_) {
        roots.push_back(header);
    }
    
    for (GCObjectHeader* header : permanent_to_young_refs_) {
        roots.push_back(header);
    }
    
    return roots;
}

std::vector<GCObjectHeader*> RememberedSet::get_old_roots() const {
    std::vector<GCObjectHeader*> roots;
    
    for (GCObjectHeader* header : permanent_to_old_refs_) {
        roots.push_back(header);
    }
    
    return roots;
}

void RememberedSet::print_statistics() const {
    std::cout << "📋 Remembered Set Statistics:" << std::endl;
    std::cout << "  Old -> Young References: " << old_to_young_refs_.size() << std::endl;
    std::cout << "  Permanent -> Young References: " << permanent_to_young_refs_.size() << std::endl;
    std::cout << "  Permanent -> Old References: " << permanent_to_old_refs_.size() << std::endl;
}

//=============================================================================
// GenerationalGC Implementation - Main Garbage Collector
//=============================================================================

GenerationalGC::GenerationalGC() : GenerationalGC(GCConfig()) {
}

GenerationalGC::GenerationalGC(const GCConfig& config) 
    : config_(config), gc_in_progress_(false), write_barrier_enabled_(true) {
    
    // Create memory regions
    young_generation_ = std::make_unique<MemoryRegion>(Generation::YOUNG, config_.young_generation_size);
    old_generation_ = std::make_unique<MemoryRegion>(Generation::OLD, config_.old_generation_size);
    permanent_generation_ = std::make_unique<MemoryRegion>(Generation::PERMANENT, config_.permanent_generation_size);
    
    // Create remembered set
    remembered_set_ = std::make_unique<RememberedSet>();
    
    last_gc_time_ = std::chrono::steady_clock::now();
    
    std::cout << "🚀 GENERATIONAL GC INITIALIZED" << std::endl;
    std::cout << "  Young Generation: " << (config_.young_generation_size / 1024 / 1024) << " MB" << std::endl;
    std::cout << "  Old Generation: " << (config_.old_generation_size / 1024 / 1024) << " MB" << std::endl;
    std::cout << "  Permanent Generation: " << (config_.permanent_generation_size / 1024 / 1024) << " MB" << std::endl;
}

GenerationalGC::~GenerationalGC() {
    // Cleanup
}

GCObjectHeader* GenerationalGC::allocate_object(size_t size, Generation preferred_gen) {
    std::lock_guard<std::mutex> lock(gc_mutex_);
    
    GCObjectHeader* header = nullptr;
    
    // Try to allocate in preferred generation
    switch (preferred_gen) {
        case Generation::YOUNG:
            header = young_generation_->allocate(size);
            if (!header && old_generation_->can_allocate(size)) {
                header = old_generation_->allocate(size);
            }
            break;
            
        case Generation::OLD:
            header = old_generation_->allocate(size);
            break;
            
        case Generation::PERMANENT:
            header = permanent_generation_->allocate(size);
            break;
    }
    
    if (header) {
        stats_.total_allocation_bytes += size;
        
        // Check if GC is needed
        if (should_trigger_minor_gc() || should_trigger_major_gc()) {
            collect_auto();
        }
    }
    
    return header;
}

void GenerationalGC::deallocate_object(GCObjectHeader* header) {
    if (!header) return;
    
    // Remove from appropriate generation
    switch (header->generation) {
        case Generation::YOUNG:
            young_generation_->remove_object(header);
            break;
        case Generation::OLD:
            old_generation_->remove_object(header);
            break;
        case Generation::PERMANENT:
            permanent_generation_->remove_object(header);
            break;
    }
}

void GenerationalGC::add_root(Object** root_ptr) {
    if (root_ptr) {
        root_pointers_.push_back(root_ptr);
    }
}

void GenerationalGC::remove_root(Object** root_ptr) {
    auto it = std::find(root_pointers_.begin(), root_pointers_.end(), root_ptr);
    if (it != root_pointers_.end()) {
        root_pointers_.erase(it);
    }
}

void GenerationalGC::add_context(Context* ctx) {
    if (ctx) {
        active_contexts_.insert(ctx);
    }
}

void GenerationalGC::remove_context(Context* ctx) {
    active_contexts_.erase(ctx);
}

void GenerationalGC::collect_minor() {
    if (gc_in_progress_.exchange(true)) {
        return; // GC already in progress
    }
    
    auto start_time = std::chrono::steady_clock::now();
    
    std::cout << "🧹 MINOR GC STARTED (Young Generation)" << std::endl;
    
    // Mark and sweep young generation only
    mark_phase(Generation::YOUNG);
    sweep_phase(Generation::YOUNG);
    size_t collected = young_generation_->sweep_objects();
    promotion_phase();
    
    stats_.minor_gc_count++;
    stats_.objects_collected += collected;
    
    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    stats_.total_collection_time_ms += duration.count();
    
    std::cout << "✅ MINOR GC COMPLETED: " << collected << " objects collected in " 
             << duration.count() << "ms" << std::endl;
    
    gc_in_progress_ = false;
}

void GenerationalGC::collect_major() {
    if (gc_in_progress_.exchange(true)) {
        return; // GC already in progress
    }
    
    auto start_time = std::chrono::steady_clock::now();
    
    std::cout << "🧹 MAJOR GC STARTED (All Generations)" << std::endl;
    
    // Mark and sweep all generations
    mark_phase(Generation::PERMANENT);
    sweep_phase(Generation::PERMANENT);
    size_t collected = young_generation_->sweep_objects() + old_generation_->sweep_objects() + permanent_generation_->sweep_objects();
    compact_phase(Generation::OLD);
    
    stats_.major_gc_count++;
    stats_.objects_collected += collected;
    
    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    stats_.total_collection_time_ms += duration.count();
    
    std::cout << "✅ MAJOR GC COMPLETED: " << collected << " objects collected in " 
             << duration.count() << "ms" << std::endl;
    
    gc_in_progress_ = false;
}

void GenerationalGC::collect_auto() {
    if (should_trigger_major_gc()) {
        collect_major();
    } else if (should_trigger_minor_gc()) {
        collect_minor();
    }
}

void GenerationalGC::write_barrier(Object* from, Object* to) {
    if (!write_barrier_enabled_ || !from || !to) {
        return;
    }
    
    // Get object headers (simplified - in real implementation would be more efficient)
    GCObjectHeader* from_header = get_object_header(from);
    GCObjectHeader* to_header = get_object_header(to);
    
    if (from_header && to_header) {
        remembered_set_->add_reference(from_header, to_header);
    }
}

bool GenerationalGC::should_trigger_minor_gc() const {
    return young_generation_->get_utilization() >= config_.young_gc_trigger_ratio;
}

bool GenerationalGC::should_trigger_major_gc() const {
    return old_generation_->get_utilization() >= config_.old_gc_trigger_ratio;
}

void GenerationalGC::promote_object(GCObjectHeader* header) {
    if (!header || header->generation != Generation::YOUNG) {
        return;
    }
    
    if (header->age >= config_.promotion_age_threshold) {
        // Promote to old generation
        header->generation = Generation::OLD;
        young_generation_->remove_object(header);
        old_generation_->add_object(header);
        stats_.objects_promoted++;
        
        std::cout << "⬆️  OBJECT PROMOTED: Young -> Old (age: " << header->age << ")" << std::endl;
    }
}

void GenerationalGC::print_statistics() const {
    std::cout << "📊 GENERATIONAL GC STATISTICS:" << std::endl;
    std::cout << "  Minor GCs: " << stats_.minor_gc_count << std::endl;
    std::cout << "  Major GCs: " << stats_.major_gc_count << std::endl;
    std::cout << "  Total Allocation: " << (stats_.total_allocation_bytes / 1024 / 1024) << " MB" << std::endl;
    std::cout << "  Total Collection Time: " << stats_.total_collection_time_ms << " ms" << std::endl;
    std::cout << "  Objects Promoted: " << stats_.objects_promoted << std::endl;
    std::cout << "  Objects Collected: " << stats_.objects_collected << std::endl;
    
    if (stats_.minor_gc_count > 0) {
        double avg_minor = static_cast<double>(stats_.total_collection_time_ms) / stats_.minor_gc_count;
        std::cout << "  Avg Minor GC Time: " << avg_minor << " ms" << std::endl;
    }
    
    if (stats_.major_gc_count > 0) {
        double avg_major = static_cast<double>(stats_.total_collection_time_ms) / stats_.major_gc_count;
        std::cout << "  Avg Major GC Time: " << avg_major << " ms" << std::endl;
    }
}

void GenerationalGC::print_memory_usage() const {
    std::cout << "💾 MEMORY USAGE:" << std::endl;
    young_generation_->print_statistics();
    old_generation_->print_statistics();
    permanent_generation_->print_statistics();
    remembered_set_->print_statistics();
}

void GenerationalGC::analyze_allocation_patterns() const {
    std::cout << "🔍 ALLOCATION PATTERN ANALYSIS:" << std::endl;
    
    auto now = std::chrono::steady_clock::now();
    auto time_since_last_gc = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_gc_time_);
    
    if (time_since_last_gc.count() > 0) {
        double allocation_rate = static_cast<double>(stats_.total_allocation_bytes) / time_since_last_gc.count() * 1000.0;
        std::cout << "  Allocation Rate: " << (allocation_rate / 1024 / 1024) << " MB/sec" << std::endl;
    }
    
    std::cout << "  Young Gen Pressure: " << (young_generation_->get_utilization() * 100.0) << "%" << std::endl;
    std::cout << "  Old Gen Pressure: " << (old_generation_->get_utilization() * 100.0) << "%" << std::endl;
}

void GenerationalGC::tune_gc_parameters() {
    // Adaptive tuning based on performance metrics
    double young_utilization = young_generation_->get_utilization();
    double old_utilization = old_generation_->get_utilization();
    
    if (stats_.minor_gc_count > 10 && stats_.average_minor_gc_time_ms > 50.0) {
        // Minor GCs are taking too long - increase young generation size
        if (config_.young_generation_size < 32 * 1024 * 1024) { // Max 32MB
            config_.young_generation_size *= 1.5;
            std::cout << "🔧 GC TUNING: Increased young generation size to " 
                     << (config_.young_generation_size / 1024 / 1024) << " MB" << std::endl;
        }
    }
    
    if (old_utilization > 0.95) {
        // Old generation is nearly full - increase size
        if (config_.old_generation_size < 128 * 1024 * 1024) { // Max 128MB
            config_.old_generation_size *= 1.2;
            std::cout << "🔧 GC TUNING: Increased old generation size to " 
                     << (config_.old_generation_size / 1024 / 1024) << " MB" << std::endl;
        }
    }
}

GenerationalGC& GenerationalGC::get_instance() {
    static GenerationalGC instance;
    return instance;
}

// Private implementation methods
void GenerationalGC::mark_phase(Generation max_generation) {
    // Simplified mark phase - mark all objects in generations up to max_generation
    if (max_generation >= Generation::YOUNG) {
        young_generation_->mark_objects();
    }
    if (max_generation >= Generation::OLD) {
        old_generation_->mark_objects();
    }
    if (max_generation >= Generation::PERMANENT) {
        permanent_generation_->mark_objects();
    }
}

void GenerationalGC::sweep_phase(Generation max_generation) {
    if (max_generation >= Generation::YOUNG) {
        young_generation_->sweep_objects();
    }
    if (max_generation >= Generation::OLD) {
        old_generation_->sweep_objects();
    }
    if (max_generation >= Generation::PERMANENT) {
        permanent_generation_->sweep_objects();
    }
}

void GenerationalGC::compact_phase(Generation generation) {
    switch (generation) {
        case Generation::YOUNG:
            young_generation_->compact_memory();
            break;
        case Generation::OLD:
            old_generation_->compact_memory();
            break;
        case Generation::PERMANENT:
            permanent_generation_->compact_memory();
            break;
    }
}

void GenerationalGC::promotion_phase() {
    // Check young generation objects for promotion
    const auto& young_objects = young_generation_->get_objects();
    
    for (GCObjectHeader* header : young_objects) {
        if (header && header->age >= config_.promotion_age_threshold) {
            promote_object(header);
        }
    }
}

Generation GenerationalGC::get_object_generation(Object* obj) const {
    // Simplified - would need more efficient lookup in real implementation
    return Generation::YOUNG; // Default
}

GCObjectHeader* GenerationalGC::get_object_header(Object* obj) const {
    // Simplified - would need efficient object->header mapping in real implementation
    return nullptr;
}

//=============================================================================
// GCObjectAllocator Implementation - GC-aware allocation
//=============================================================================

GCObjectAllocator::GCObjectAllocator() : gc_(&GenerationalGC::get_instance()) {
    std::cout << "🏭 GC OBJECT ALLOCATOR INITIALIZED" << std::endl;
}

GCObjectAllocator::~GCObjectAllocator() {
    // Cleanup
}

void GCObjectAllocator::deallocate_object(Object* obj) {
    if (obj) {
        // Get header and deallocate through GC
        GCObjectHeader* header = gc_->get_object_header(obj);
        if (header) {
            gc_->deallocate_object(header);
        }
    }
}

void GCObjectAllocator::print_allocation_statistics() const {
    std::cout << "🏭 ALLOCATION STATISTICS:" << std::endl;
    std::cout << "  Young Allocations: " << alloc_stats_.young_allocations << std::endl;
    std::cout << "  Old Allocations: " << alloc_stats_.old_allocations << std::endl;
    std::cout << "  Permanent Allocations: " << alloc_stats_.permanent_allocations << std::endl;
    std::cout << "  Total Bytes: " << (alloc_stats_.total_bytes_allocated / 1024 / 1024) << " MB" << std::endl;
}

GCObjectAllocator& GCObjectAllocator::get_instance() {
    static GCObjectAllocator instance;
    return instance;
}

//=============================================================================
// GCIntegration Implementation - Engine hooks
//=============================================================================

void GCIntegration::initialize_gc() {
    GenerationalGC& gc = GenerationalGC::get_instance();
    std::cout << "🔗 GC INTEGRATION INITIALIZED" << std::endl;
}

void GCIntegration::shutdown_gc() {
    GenerationalGC& gc = GenerationalGC::get_instance();
    gc.print_statistics();
    gc.print_memory_usage();
    std::cout << "🔗 GC INTEGRATION SHUTDOWN" << std::endl;
}

void GCIntegration::on_object_allocation(Object* obj) {
    // Hook for automatic GC triggering
    GenerationalGC& gc = GenerationalGC::get_instance();
    if (gc.should_trigger_minor_gc()) {
        gc.collect_minor();
    }
}

void GCIntegration::on_context_creation(Context* ctx) {
    GenerationalGC& gc = GenerationalGC::get_instance();
    gc.add_context(ctx);
}

void GCIntegration::on_context_destruction(Context* ctx) {
    GenerationalGC& gc = GenerationalGC::get_instance();
    gc.remove_context(ctx);
}

void GCIntegration::force_gc(bool major) {
    GenerationalGC& gc = GenerationalGC::get_instance();
    if (major) {
        gc.collect_major();
    } else {
        gc.collect_minor();
    }
}

void GCIntegration::adapt_gc_frequency() {
    GenerationalGC& gc = GenerationalGC::get_instance();
    gc.tune_gc_parameters();
}

void GCIntegration::optimize_gc_timing() {
    GenerationalGC& gc = GenerationalGC::get_instance();
    gc.analyze_allocation_patterns();
}

} // namespace Quanta