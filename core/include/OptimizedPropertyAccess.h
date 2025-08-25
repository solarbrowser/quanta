#ifndef QUANTA_OPTIMIZED_PROPERTY_ACCESS_H
#define QUANTA_OPTIMIZED_PROPERTY_ACCESS_H

#include "OptimizedAST.h"
#include "SpecializedNodes.h"
#include "Value.h"
#include "Object.h"
#include "Context.h"
#include <vector>
#include <unordered_map>
#include <array>
#include <cstdint>
#include <memory>

namespace Quanta {

// Property access optimization levels
enum class PropertyAccessLevel : uint8_t {
    INTERPRETED,         // Standard property lookup
    CACHED_LOOKUP,       // Cached property name to offset
    INLINE_CACHE,        // Polymorphic inline cache
    HIDDEN_CLASS,        // Hidden class optimization
    DIRECT_OFFSET,       // Direct memory offset access
    NATIVE_COMPILED      // JIT compiled property access
};

// Property access pattern types
enum class PropertyPattern : uint8_t {
    SINGLE_PROPERTY,     // obj.prop
    PROPERTY_CHAIN,      // obj.a.b.c
    ARRAY_INDEX,         // obj[0], obj[1], etc.
    DYNAMIC_PROPERTY,    // obj[variable]
    METHOD_CALL,         // obj.method()
    PROTOTYPE_CHAIN      // Prototype chain traversal
};

// Hidden class for shape-based optimization
struct alignas(64) HiddenClass {
    uint32_t class_id;
    uint32_t property_count;
    uint32_t parent_class_id;
    
    // Property layout information
    struct PropertyDescriptor {
        uint32_t name_hash;
        uint32_t offset;
        uint8_t property_type;
        uint8_t attributes;
    };
    
    std::array<PropertyDescriptor, 32> properties; // Max 32 properties per class
    
    // Transition information
    std::unordered_map<uint32_t, uint32_t> property_transitions;
    
    // Performance metrics
    uint64_t access_count;
    uint64_t cache_hits;
    double hit_rate;
};

// Polymorphic inline cache entry
struct alignas(32) InlineCacheEntry {
    uint32_t call_site_id;
    uint32_t hidden_class_id;
    uint32_t property_offset;
    PropertyAccessLevel optimization_level;
    
    // Performance data
    uint64_t hit_count;
    uint64_t miss_count;
    uint64_t last_access_time;
    
    // Function pointer for direct dispatch
    Value (*direct_accessor)(Object* obj, uint32_t offset);
    
    bool is_valid() const { return hidden_class_id != 0; }
    double get_hit_rate() const { 
        uint64_t total = hit_count + miss_count;
        return total > 0 ? static_cast<double>(hit_count) / total : 0.0; 
    }
};

class OptimizedPropertyAccessOptimizer {
private:
    OptimizedAST* ast_context_;
    SpecializedNodeProcessor* specialized_processor_;
    
    // Hidden class management
    std::unordered_map<uint32_t, std::unique_ptr<HiddenClass>> hidden_classes_;
    std::unordered_map<Object*, uint32_t> object_to_class_;
    uint32_t next_class_id_;
    
    // Inline caches
    std::unordered_map<uint32_t, std::vector<InlineCacheEntry>> inline_caches_;
    std::unordered_map<uint32_t, PropertyPattern> access_patterns_;
    
    // Property name interning
    std::unordered_map<std::string, uint32_t> property_name_hashes_;
    std::vector<std::string> hash_to_property_name_;
    
    // Performance counters
    uint64_t total_property_accesses_;
    uint64_t fast_path_hits_;
    uint64_t cache_hits_;
    uint64_t cache_misses_;
    uint64_t hidden_class_transitions_;
    
public:
    OptimizedPropertyAccessOptimizer(OptimizedAST* ast, SpecializedNodeProcessor* processor);
    ~OptimizedPropertyAccessOptimizer();
    
    // Property access optimization
    Value get_property_optimized(Object* obj, const std::string& property_name, uint32_t call_site_id);
    void set_property_optimized(Object* obj, const std::string& property_name, const Value& value, uint32_t call_site_id);
    Value access_property_chain(Object* obj, const std::vector<std::string>& properties, uint32_t call_site_id);
    
    // Hidden class management
    uint32_t get_or_create_hidden_class(Object* obj);
    uint32_t transition_hidden_class(uint32_t current_class_id, const std::string& property_name);
    void update_object_shape(Object* obj, const std::string& property_name);
    
    // Inline cache management
    void update_inline_cache(uint32_t call_site_id, Object* obj, const std::string& property_name, uint32_t offset);
    InlineCacheEntry* lookup_inline_cache(uint32_t call_site_id, uint32_t hidden_class_id);
    void invalidate_inline_cache(uint32_t call_site_id);
    
    // Property pattern optimization
    PropertyPattern detect_access_pattern(uint32_t call_site_id, const std::string& property_name);
    void optimize_for_pattern(uint32_t call_site_id, PropertyPattern pattern);
    
    // Direct memory access
    Value direct_property_access(Object* obj, uint32_t offset, uint32_t property_type);
    void generate_direct_accessor(uint32_t call_site_id, uint32_t offset);
    
    // Specialized accessors
    Value access_array_index(Object* array_obj, int32_t index);
    Value access_method_property(Object* obj, const std::string& method_name, uint32_t call_site_id);
    Value traverse_prototype_chain(Object* obj, const std::string& property_name);
    
    // Performance analysis
    bool should_optimize_access_site(uint32_t call_site_id);
    void identify_hot_property_accesses();
    PropertyAccessLevel determine_optimization_level(uint32_t call_site_id);
    
    // Batch optimization
    void optimize_property_chain_batch(const std::vector<uint32_t>& chain_nodes);
    void precompute_property_offsets(const std::vector<std::string>& common_properties);
    
    // Statistics and monitoring
    double get_fast_path_hit_rate() const;
    double get_cache_hit_rate() const;
    uint64_t get_total_time_saved() const;
    void print_optimization_statistics() const;
    
    // Memory management
    void garbage_collect_hidden_classes();
    void clear_optimization_caches();
    size_t get_memory_usage() const;
    
private:
    uint32_t hash_property_name(const std::string& name);
    uint32_t calculate_property_offset(const HiddenClass& hidden_class, const std::string& property_name);
    void create_property_transition(uint32_t from_class, uint32_t to_class, const std::string& property_name);
};

// Direct property access functions for maximum speed
class DirectPropertyAccessors {
public:
    // Type-specialized accessors
    static Value access_number_property(Object* obj, uint32_t offset);
    static Value access_string_property(Object* obj, uint32_t offset);
    static Value access_object_property(Object* obj, uint32_t offset);
    static Value access_function_property(Object* obj, uint32_t offset);
    static Value access_boolean_property(Object* obj, uint32_t offset);
    
    // Array-optimized accessors
    static Value access_array_element_unchecked(Object* array, uint32_t index);
    static Value access_array_element_bounds_checked(Object* array, uint32_t index);
    static void set_array_element_unchecked(Object* array, uint32_t index, const Value& value);
    
    // Method call optimizers
    static Value call_cached_method(Object* obj, uint32_t method_offset, const std::vector<Value>& args, Context& ctx);
    static bool is_method_cached(Object* obj, uint32_t method_offset);
};

// Property layout optimizer
class PropertyLayoutOptimizer {
private:
    struct LayoutAnalysis {
        std::vector<std::string> property_access_order;
        std::unordered_map<std::string, uint64_t> access_frequencies;
        std::unordered_map<std::string, uint32_t> property_types;
        double cache_friendliness_score;
    };
    
    std::unordered_map<uint32_t, LayoutAnalysis> layout_analyses_;
    
public:
    void analyze_object_layout(Object* obj);
    void optimize_property_layout(HiddenClass& hidden_class);
    std::vector<std::string> get_optimal_property_order(const LayoutAnalysis& analysis);
    void pack_properties_for_cache_efficiency(HiddenClass& hidden_class);
    
    uint32_t calculate_optimal_alignment(uint32_t property_type);
    void minimize_memory_fragmentation(HiddenClass& hidden_class);
};

// Prototype chain optimizer
class PrototypeChainOptimizer {
private:
    struct PrototypeCache {
        Object* prototype_object;
        uint32_t property_offset;
        uint64_t cache_generation;
        bool is_valid;
    };
    
    std::unordered_map<uint32_t, PrototypeCache> prototype_cache_;
    uint64_t cache_generation_;
    
public:
    PrototypeChainOptimizer();
    
    Value lookup_in_prototype_chain(Object* obj, const std::string& property_name);
    void cache_prototype_lookup(Object* obj, const std::string& property_name, Object* prototype, uint32_t offset);
    void invalidate_prototype_cache(Object* prototype);
    
    bool can_skip_prototype_lookup(Object* obj, const std::string& property_name);
    void optimize_prototype_access_pattern(Object* obj);
};

// Specialized property access strategies
class PropertyAccessStrategy {
public:
    // Strategy selection
    static PropertyAccessLevel select_optimal_strategy(Object* obj, const std::string& property_name, uint32_t call_site_id);
    static bool should_use_direct_access(const InlineCacheEntry& cache_entry);
    static bool should_use_hidden_class_optimization(Object* obj);
    
    // Strategy execution
    static Value execute_cached_access(Object* obj, const InlineCacheEntry& cache_entry);
    static Value execute_direct_access(Object* obj, uint32_t offset, uint32_t property_type);
    static Value execute_fallback_access(Object* obj, const std::string& property_name);
};

// Runtime property profiling
class PropertyAccessProfiler {
private:
    struct AccessProfile {
        uint64_t total_accesses;
        std::unordered_map<std::string, uint64_t> property_frequencies;
        std::unordered_map<uint32_t, uint64_t> call_site_frequencies;
        uint64_t average_access_time;
        PropertyPattern dominant_pattern;
    };
    
    std::unordered_map<Object*, AccessProfile> object_profiles_;
    bool profiling_enabled_;
    
public:
    PropertyAccessProfiler();
    
    void start_profiling();
    void stop_profiling();
    void profile_property_access(Object* obj, const std::string& property_name, uint32_t call_site_id, uint64_t access_time);
    
    std::vector<Object*> get_hot_objects() const;
    std::vector<std::string> get_hot_properties(Object* obj) const;
    PropertyPattern get_dominant_pattern(Object* obj) const;
    
    void export_profile_data(const std::string& filename) const;
    void import_profile_data(const std::string& filename);
};

} // namespace Quanta

#endif // QUANTA_ULTRA_FAST_PROPERTY_ACCESS_H