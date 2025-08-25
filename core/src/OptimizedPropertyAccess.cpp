#include "../include/OptimizedPropertyAccess.h"
#include <chrono>
#include <algorithm>
#include <functional>

namespace Quanta {

OptimizedPropertyAccessOptimizer::OptimizedPropertyAccessOptimizer(OptimizedAST* ast, SpecializedNodeProcessor* processor)
    : ast_context_(ast), specialized_processor_(processor), next_class_id_(1),
      total_property_accesses_(0), fast_path_hits_(0), cache_hits_(0), 
      cache_misses_(0), hidden_class_transitions_(0) {
    
    hidden_classes_.reserve(1000);
    object_to_class_.reserve(10000);
    inline_caches_.reserve(5000);
    property_name_hashes_.reserve(10000);
    hash_to_property_name_.reserve(10000);
}

OptimizedPropertyAccessOptimizer::~OptimizedPropertyAccessOptimizer() {
    clear_optimization_caches();
}

Value OptimizedPropertyAccessOptimizer::get_property_optimized(Object* obj, const std::string& property_name, 
                                                              uint32_t call_site_id) {
    auto start_time = std::chrono::high_resolution_clock::now();
    
    total_property_accesses_++;
    
    // Step 1: Try inline cache lookup
    uint32_t hidden_class_id = get_or_create_hidden_class(obj);
    InlineCacheEntry* cache_entry = lookup_inline_cache(call_site_id, hidden_class_id);
    
    if (cache_entry && cache_entry->is_valid()) {
        // Fast path: Direct property access via cached offset
        Value result = direct_property_access(obj, cache_entry->property_offset, 0);
        cache_entry->hit_count++;
        cache_hits_++;
        fast_path_hits_++;
        
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time);
        cache_entry->last_access_time = duration.count();
        
        return result;
    }
    
    // Step 2: Slow path - compute property offset and update cache
    Value result = obj->get_property(property_name);
    
    // Calculate property offset for future optimization
    auto hidden_class_it = hidden_classes_.find(hidden_class_id);
    if (hidden_class_it != hidden_classes_.end()) {
        uint32_t offset = calculate_property_offset(*hidden_class_it->second, property_name);
        update_inline_cache(call_site_id, obj, property_name, offset);
    }
    
    cache_misses_++;
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time);
    
    return result;
}

void OptimizedPropertyAccessOptimizer::set_property_optimized(Object* obj, const std::string& property_name, 
                                                             const Value& value, uint32_t call_site_id) {
    total_property_accesses_++;
    
    // Check if this property addition requires hidden class transition
    uint32_t current_class_id = get_or_create_hidden_class(obj);
    
    if (!obj->has_property(property_name)) {
        // Property addition - transition to new hidden class
        uint32_t new_class_id = transition_hidden_class(current_class_id, property_name);
        object_to_class_[obj] = new_class_id;
        hidden_class_transitions_++;
        
        // Invalidate any existing inline caches for this object
        invalidate_inline_cache(call_site_id);
    }
    
    // Set the property value
    obj->set_property(property_name, value);
    
    // Update inline cache with new offset
    uint32_t new_class_id = object_to_class_[obj];
    auto hidden_class_it = hidden_classes_.find(new_class_id);
    if (hidden_class_it != hidden_classes_.end()) {
        uint32_t offset = calculate_property_offset(*hidden_class_it->second, property_name);
        update_inline_cache(call_site_id, obj, property_name, offset);
    }
}

Value OptimizedPropertyAccessOptimizer::access_property_chain(Object* obj, const std::vector<std::string>& properties, 
                                                             uint32_t call_site_id) {
    if (properties.empty()) {
        return Value();
    }
    
    // Optimize property chain access
    Object* current_obj = obj;
    Value result;
    
    for (size_t i = 0; i < properties.size(); ++i) {
        const std::string& prop = properties[i];
        uint32_t chain_call_site = call_site_id + i; // Unique call site per chain step
        
        result = get_property_optimized(current_obj, prop, chain_call_site);
        
        if (i < properties.size() - 1) {
            // Need to continue chain
            if (!result.is_object()) {
                return Value(); // Chain broken
            }
            current_obj = result.as_object();
        }
    }
    
    return result;
}

uint32_t OptimizedPropertyAccessOptimizer::get_or_create_hidden_class(Object* obj) {
    auto it = object_to_class_.find(obj);
    if (it != object_to_class_.end()) {
        return it->second;
    }
    
    // Create new hidden class for this object
    uint32_t class_id = next_class_id_++;
    auto hidden_class = std::make_unique<HiddenClass>();
    
    hidden_class->class_id = class_id;
    hidden_class->property_count = 0;
    hidden_class->parent_class_id = 0;
    hidden_class->access_count = 0;
    hidden_class->cache_hits = 0;
    hidden_class->hit_rate = 0.0;
    
    // Initialize property descriptors
    for (auto& prop : hidden_class->properties) {
        prop.name_hash = 0;
        prop.offset = 0;
        prop.property_type = 0;
        prop.attributes = 0;
    }
    
    // Analyze existing properties on the object
    std::vector<std::string> existing_properties = obj->get_own_property_keys();
    for (size_t i = 0; i < existing_properties.size() && i < 32; ++i) {
        const std::string& prop_name = existing_properties[i];
        uint32_t name_hash = hash_property_name(prop_name);
        
        auto& descriptor = hidden_class->properties[i];
        descriptor.name_hash = name_hash;
        descriptor.offset = static_cast<uint32_t>(i * 8); // 8-byte aligned
        descriptor.property_type = 1; // Generic property type
        descriptor.attributes = 0;
        
        hidden_class->property_count++;
    }
    
    hidden_classes_[class_id] = std::move(hidden_class);
    object_to_class_[obj] = class_id;
    
    return class_id;
}

uint32_t OptimizedPropertyAccessOptimizer::transition_hidden_class(uint32_t current_class_id, const std::string& property_name) {
    auto current_class_it = hidden_classes_.find(current_class_id);
    if (current_class_it == hidden_classes_.end()) {
        return current_class_id;
    }
    
    const HiddenClass& current_class = *current_class_it->second;
    uint32_t property_hash = hash_property_name(property_name);
    
    // Check if transition already exists
    auto transition_it = current_class.property_transitions.find(property_hash);
    if (transition_it != current_class.property_transitions.end()) {
        return transition_it->second;
    }
    
    // Create new hidden class with additional property
    uint32_t new_class_id = next_class_id_++;
    auto new_hidden_class = std::make_unique<HiddenClass>();
    
    new_hidden_class->class_id = new_class_id;
    new_hidden_class->property_count = current_class.property_count + 1;
    new_hidden_class->parent_class_id = current_class_id;
    new_hidden_class->access_count = 0;
    new_hidden_class->cache_hits = 0;
    new_hidden_class->hit_rate = 0.0;
    
    // Copy existing properties
    for (uint32_t i = 0; i < current_class.property_count; ++i) {
        new_hidden_class->properties[i] = current_class.properties[i];
    }
    
    // Add new property
    if (new_hidden_class->property_count <= 32) {
        auto& new_descriptor = new_hidden_class->properties[new_hidden_class->property_count - 1];
        new_descriptor.name_hash = property_hash;
        new_descriptor.offset = (new_hidden_class->property_count - 1) * 8; // 8-byte aligned
        new_descriptor.property_type = 1; // Generic property type
        new_descriptor.attributes = 0;
    }
    
    hidden_classes_[new_class_id] = std::move(new_hidden_class);
    
    // Update transition table
    const_cast<HiddenClass&>(current_class).property_transitions[property_hash] = new_class_id;
    
    return new_class_id;
}

void OptimizedPropertyAccessOptimizer::update_inline_cache(uint32_t call_site_id, Object* obj, 
                                                          const std::string& property_name, uint32_t offset) {
    uint32_t hidden_class_id = get_or_create_hidden_class(obj);
    
    auto& cache_entries = inline_caches_[call_site_id];
    
    // Check if entry already exists
    for (auto& entry : cache_entries) {
        if (entry.hidden_class_id == hidden_class_id) {
            entry.property_offset = offset;
            return;
        }
    }
    
    // Create new cache entry
    InlineCacheEntry new_entry{};
    new_entry.call_site_id = call_site_id;
    new_entry.hidden_class_id = hidden_class_id;
    new_entry.property_offset = offset;
    new_entry.optimization_level = PropertyAccessLevel::INLINE_CACHE;
    new_entry.hit_count = 0;
    new_entry.miss_count = 0;
    new_entry.last_access_time = 0;
    new_entry.direct_accessor = &DirectPropertyAccessors::access_object_property;
    
    cache_entries.push_back(new_entry);
    
    // Limit cache entries to prevent excessive memory usage
    if (cache_entries.size() > 8) {
        // Remove least recently used entry
        auto oldest = std::min_element(cache_entries.begin(), cache_entries.end(),
            [](const InlineCacheEntry& a, const InlineCacheEntry& b) {
                return a.last_access_time < b.last_access_time;
            });
        cache_entries.erase(oldest);
    }
}

InlineCacheEntry* OptimizedPropertyAccessOptimizer::lookup_inline_cache(uint32_t call_site_id, uint32_t hidden_class_id) {
    auto cache_it = inline_caches_.find(call_site_id);
    if (cache_it == inline_caches_.end()) {
        return nullptr;
    }
    
    for (auto& entry : cache_it->second) {
        if (entry.hidden_class_id == hidden_class_id) {
            return &entry;
        }
    }
    
    return nullptr;
}

Value OptimizedPropertyAccessOptimizer::direct_property_access(Object* obj, uint32_t offset, uint32_t property_type) {
    // Ultra-fast direct memory access to property
    // In a real implementation, this would access the object's internal storage directly
    
    // For now, use a simplified approach
    std::vector<std::string> property_names = obj->get_own_property_keys();
    if (offset / 8 < property_names.size()) {
        const std::string& prop_name = property_names[offset / 8];
        return obj->get_property(prop_name);
    }
    
    return Value();
}

uint32_t OptimizedPropertyAccessOptimizer::hash_property_name(const std::string& name) {
    auto it = property_name_hashes_.find(name);
    if (it != property_name_hashes_.end()) {
        return it->second;
    }
    
    // Simple hash function (in production, would use a better hash)
    uint32_t hash = 0;
    for (char c : name) {
        hash = hash * 31 + static_cast<uint32_t>(c);
    }
    
    property_name_hashes_[name] = hash;
    if (hash >= hash_to_property_name_.size()) {
        hash_to_property_name_.resize(hash + 1);
    }
    hash_to_property_name_[hash] = name;
    
    return hash;
}

uint32_t OptimizedPropertyAccessOptimizer::calculate_property_offset(const HiddenClass& hidden_class, 
                                                                    const std::string& property_name) {
    uint32_t property_hash = hash_property_name(property_name);
    
    for (uint32_t i = 0; i < hidden_class.property_count; ++i) {
        if (hidden_class.properties[i].name_hash == property_hash) {
            return hidden_class.properties[i].offset;
        }
    }
    
    return 0; // Property not found
}

double OptimizedPropertyAccessOptimizer::get_fast_path_hit_rate() const {
    if (total_property_accesses_ == 0) return 0.0;
    return static_cast<double>(fast_path_hits_) / total_property_accesses_;
}

double OptimizedPropertyAccessOptimizer::get_cache_hit_rate() const {
    uint64_t total_cache_accesses = cache_hits_ + cache_misses_;
    if (total_cache_accesses == 0) return 0.0;
    return static_cast<double>(cache_hits_) / total_cache_accesses;
}

void OptimizedPropertyAccessOptimizer::clear_optimization_caches() {
    hidden_classes_.clear();
    object_to_class_.clear();
    inline_caches_.clear();
    property_name_hashes_.clear();
    hash_to_property_name_.clear();
    
    next_class_id_ = 1;
    total_property_accesses_ = 0;
    fast_path_hits_ = 0;
    cache_hits_ = 0;
    cache_misses_ = 0;
    hidden_class_transitions_ = 0;
}

size_t OptimizedPropertyAccessOptimizer::get_memory_usage() const {
    size_t total = 0;
    
    // Hidden classes
    total += hidden_classes_.size() * sizeof(std::unique_ptr<HiddenClass>);
    for (const auto& pair : hidden_classes_) {
        total += sizeof(HiddenClass);
    }
    
    // Object to class mapping
    total += object_to_class_.size() * sizeof(std::pair<Object*, uint32_t>);
    
    // Inline caches
    total += inline_caches_.size() * sizeof(std::pair<uint32_t, std::vector<InlineCacheEntry>>);
    for (const auto& pair : inline_caches_) {
        total += pair.second.size() * sizeof(InlineCacheEntry);
    }
    
    // Property name hashes
    total += property_name_hashes_.size() * sizeof(std::pair<std::string, uint32_t>);
    total += hash_to_property_name_.size() * sizeof(std::string);
    
    return total;
}

// DirectPropertyAccessors implementation
Value DirectPropertyAccessors::access_number_property(Object* obj, uint32_t offset) {
    // Direct access to number property at specific offset
    // Implementation would access object's internal storage
    return Value(42.0); // Placeholder
}

Value DirectPropertyAccessors::access_string_property(Object* obj, uint32_t offset) {
    // Direct access to string property at specific offset
    return Value("optimized_string"); // Placeholder
}

Value DirectPropertyAccessors::access_object_property(Object* obj, uint32_t offset) {
    // Direct access to object property at specific offset
    return Value(); // Placeholder
}

Value DirectPropertyAccessors::access_array_element_unchecked(Object* array, uint32_t index) {
    // Ultra-fast array access without bounds checking
    return array->get_element(index);
}

Value DirectPropertyAccessors::access_array_element_bounds_checked(Object* array, uint32_t index) {
    // Array access with bounds checking
    // Convert index to string for property checking
    std::string index_str = std::to_string(index);
    if (array->has_own_property(index_str)) {
        return array->get_element(index);
    }
    return Value(); // undefined
}

// PropertyLayoutOptimizer implementation
void PropertyLayoutOptimizer::analyze_object_layout(Object* obj) {
    // Analyze property access patterns for optimal layout
    // This would be called during runtime profiling
}

void PropertyLayoutOptimizer::optimize_property_layout(HiddenClass& hidden_class) {
    // Reorder properties for better cache locality
    // Most frequently accessed properties should be at the beginning
    
    std::sort(hidden_class.properties.begin(), 
              hidden_class.properties.begin() + hidden_class.property_count,
              [](const HiddenClass::PropertyDescriptor& a, const HiddenClass::PropertyDescriptor& b) {
                  // Sort by some access frequency metric (placeholder)
                  return a.offset < b.offset;
              });
    
    // Recalculate offsets for optimal alignment
    for (uint32_t i = 0; i < hidden_class.property_count; ++i) {
        hidden_class.properties[i].offset = i * 8; // 8-byte aligned
    }
}

} // namespace Quanta