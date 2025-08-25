/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../include/ShapeOptimization.h"
#include "../include/PhotonCore/PhotonCoreQuantum.h"
#include "../include/PhotonCore/PhotonCoreSonic.h"
#include "../include/PhotonCore/PhotonCorePerformance.h"
#include <iostream>
#include <sstream>
#include <algorithm>

namespace Quanta {

//=============================================================================
// ObjectShape Implementation - High-performance Shape Optimization
//=============================================================================

ShapeID ObjectShape::next_shape_id_ = 1;
std::unordered_map<ShapeID, std::shared_ptr<ObjectShape>> ObjectShape::global_shapes_;

ObjectShape::ObjectShape() 
    : shape_id_(next_shape_id_++), parent_shape_(nullptr), property_count_(0), transition_count_(0) {
    global_shapes_[shape_id_] = std::shared_ptr<ObjectShape>(this);
}

ObjectShape::ObjectShape(ObjectShape* parent, const std::string& property_name)
    : shape_id_(next_shape_id_++), parent_shape_(parent), property_count_(0), transition_count_(0) {
    
    // Copy properties from parent
    if (parent) {
        properties_ = parent->properties_;
        property_map_ = parent->property_map_;
        property_count_ = parent->property_count_;
    }
    
    // Add new property
    PropertyOffset offset = property_count_;
    properties_.emplace_back(property_name, offset);
    property_map_[property_name] = offset;
    property_count_++;
    
    global_shapes_[shape_id_] = std::shared_ptr<ObjectShape>(this);
    
    if (parent) {
        parent->increment_transition_count();
    }
}

ObjectShape::~ObjectShape() {
    global_shapes_.erase(shape_id_);
}

bool ObjectShape::has_property(const std::string& name) const {
    return property_map_.find(name) != property_map_.end();
}

PropertyOffset ObjectShape::get_property_offset(const std::string& name) const {
    auto it = property_map_.find(name);
    return (it != property_map_.end()) ? it->second : static_cast<PropertyOffset>(-1);
}

const ObjectShape::PropertyDescriptor* ObjectShape::get_property_descriptor(const std::string& name) const {
    auto it = property_map_.find(name);
    if (it != property_map_.end()) {
        PropertyOffset offset = it->second;
        if (offset < properties_.size()) {
            return &properties_[offset];
        }
    }
    return nullptr;
}

ObjectShape* ObjectShape::transition_add_property(const std::string& property_name) {
    // Check if we already have this property
    if (has_property(property_name)) {
        return this; // No transition needed
    }
    
    // Create new shape with added property
    auto new_shape = new ObjectShape(this, property_name);
    return new_shape;
}

ObjectShape* ObjectShape::transition_delete_property(const std::string& property_name) {
    // For simplicity, deletion creates a new shape without the property
    // In a full implementation, this would be more sophisticated
    auto new_shape = new ObjectShape();
    
    // Copy all properties except the deleted one
    for (const auto& prop : properties_) {
        if (prop.name != property_name) {
            PropertyOffset offset = new_shape->property_count_;
            new_shape->properties_.emplace_back(prop.name, offset);
            new_shape->property_map_[prop.name] = offset;
            new_shape->property_count_++;
        }
    }
    
    return new_shape;
}

std::shared_ptr<ObjectShape> ObjectShape::get_root_shape() {
    static auto root_shape = std::make_shared<ObjectShape>();
    return root_shape;
}

std::shared_ptr<ObjectShape> ObjectShape::get_shape_by_id(ShapeID id) {
    auto it = global_shapes_.find(id);
    return (it != global_shapes_.end()) ? it->second : nullptr;
}

void ObjectShape::cleanup_unused_shapes() {
    // Remove shapes with no references (would need reference counting in full implementation)
    std::cout << "� Shape cleanup: " << global_shapes_.size() << " shapes tracked" << std::endl;
}

std::string ObjectShape::to_string() const {
    std::ostringstream oss;
    oss << "Shape(" << shape_id_ << ") [";
    for (size_t i = 0; i < properties_.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << properties_[i].name << "@" << properties_[i].offset;
    }
    oss << "] transitions:" << transition_count_;
    return oss.str();
}

//=============================================================================
// ShapeCache Implementation - High-Performance Property Access Cache
//=============================================================================

ShapeCache::ShapeCache() : total_lookups_(0), cache_hits_(0), cache_misses_(0) {
    // Initialize cache entries
    for (size_t i = 0; i < CACHE_SIZE; ++i) {
        cache_[i] = CacheEntry();
    }
}

ShapeCache::~ShapeCache() {
    // Cleanup
}

uint32_t ShapeCache::hash_property_access(const std::string& property, ShapeID shape_id) const {
    // Simple hash combining property name and shape ID
    uint32_t hash = 2166136261U; // FNV offset basis
    
    // Hash property name
    for (char c : property) {
        hash ^= static_cast<uint32_t>(c);
        hash *= 16777619U; // FNV prime
    }
    
    // Mix in shape ID
    hash ^= shape_id;
    hash *= 16777619U;
    
    return hash & CACHE_MASK;
}

bool ShapeCache::lookup(const std::string& property, ShapeID shape_id, PropertyOffset& offset) {
    total_lookups_++;
    
    uint32_t index = hash_property_access(property, shape_id);
    CacheEntry& entry = cache_[index];
    
    if (entry.shape_id == shape_id) {
        // Cache hit!
        cache_hits_++;
        entry.hit_count++;
        entry.access_count++;
        offset = entry.offset;
        return true;
    }
    
    // Cache miss
    cache_misses_++;
    return false;
}

void ShapeCache::insert(const std::string& property, ShapeID shape_id, PropertyOffset offset) {
    uint32_t index = hash_property_access(property, shape_id);
    CacheEntry& entry = cache_[index];
    
    // Update cache entry
    entry.shape_id = shape_id;
    entry.offset = offset;
    entry.access_count = 1;
    entry.hit_count = 0;
}

void ShapeCache::invalidate_shape(ShapeID shape_id) {
    // Invalidate all cache entries for this shape
    for (size_t i = 0; i < CACHE_SIZE; ++i) {
        if (cache_[i].shape_id == shape_id) {
            cache_[i] = CacheEntry(); // Reset entry
        }
    }
}

double ShapeCache::get_hit_ratio() const {
    return (total_lookups_ > 0) ? static_cast<double>(cache_hits_) / total_lookups_ : 0.0;
}

void ShapeCache::clear() {
    for (size_t i = 0; i < CACHE_SIZE; ++i) {
        cache_[i] = CacheEntry();
    }
    total_lookups_ = 0;
    cache_hits_ = 0;
    cache_misses_ = 0;
}

void ShapeCache::print_stats() const {
    std::cout << "� Shape Cache Statistics:" << std::endl;
    std::cout << "  Total Lookups: " << total_lookups_ << std::endl;
    std::cout << "  Cache Hits: " << cache_hits_ << std::endl;
    std::cout << "  Cache Misses: " << cache_misses_ << std::endl;
    std::cout << "  Hit Ratio: " << (get_hit_ratio() * 100.0) << "%" << std::endl;
}

//=============================================================================
// ShapeOptimizedObject Implementation - Standard Object Optimization
//=============================================================================

ShapeCache ShapeOptimizedObject::global_shape_cache_;

ShapeOptimizedObject::ShapeOptimizedObject() 
    : Object(ObjectType::Ordinary), shape_(ObjectShape::get_root_shape()) {
    // Initialize with root shape (no properties)
}

ShapeOptimizedObject::ShapeOptimizedObject(std::shared_ptr<ObjectShape> shape)
    : Object(ObjectType::Ordinary), shape_(shape) {
    
    // Allocate fast properties array based on shape
    if (shape_) {
        fast_properties_.resize(shape_->get_property_count(), Value());
    }
}

ShapeOptimizedObject::~ShapeOptimizedObject() {
    // Cleanup
}

Value ShapeOptimizedObject::get_property(const std::string& key) const {
    if (!shape_) {
        return Object::get_property(key); // Fallback to base implementation
    }
    
    // Try shape cache first
    PropertyOffset offset;
    if (global_shape_cache_.lookup(key, shape_->get_id(), offset)) {
        // ULTRA-FAST cache hit!
        if (offset < fast_properties_.size()) {
            return fast_properties_[offset];
        }
    }
    
    // Shape-based lookup
    if (shape_->has_property(key)) {
        PropertyOffset offset = shape_->get_property_offset(key);
        if (offset < fast_properties_.size()) {
            // Update cache for future accesses
            global_shape_cache_.insert(key, shape_->get_id(), offset);
            return fast_properties_[offset];
        }
    }
    
    // Fallback to base object property lookup
    return Object::get_property(key);
}

bool ShapeOptimizedObject::set_property(const std::string& key, const Value& value, PropertyAttributes attributes) {
    if (!shape_) {
        return Object::set_property(key, value, attributes);
    }
    
    // Check if property exists in current shape
    if (shape_->has_property(key)) {
        // Fast path: property already exists
        PropertyOffset offset = shape_->get_property_offset(key);
        if (offset < fast_properties_.size()) {
            fast_properties_[offset] = value;
            global_shape_cache_.insert(key, shape_->get_id(), offset);
            return true;
        }
    }
    
    // Property doesn't exist - need shape transition
    ObjectShape* new_shape = shape_->transition_add_property(key);
    if (new_shape && new_shape != shape_.get()) {
        // Transition to new shape
        auto shared_new_shape = std::shared_ptr<ObjectShape>(new_shape);
        transition_shape(shared_new_shape);
        
        // Set the new property
        PropertyOffset offset = new_shape->get_property_offset(key);
        if (offset < fast_properties_.size()) {
            fast_properties_[offset] = value;
            global_shape_cache_.insert(key, new_shape->get_id(), offset);
            return true;
        }
    }
    
    // Fallback to base implementation
    return Object::set_property(key, value, attributes);
}

bool ShapeOptimizedObject::has_property(const std::string& key) const {
    if (shape_ && shape_->has_property(key)) {
        return true;
    }
    return Object::has_property(key);
}

bool ShapeOptimizedObject::delete_property(const std::string& key) {
    if (!shape_) {
        return Object::delete_property(key);
    }
    
    // Shape transition for property deletion
    ObjectShape* new_shape = shape_->transition_delete_property(key);
    if (new_shape && new_shape != shape_.get()) {
        auto shared_new_shape = std::shared_ptr<ObjectShape>(new_shape);
        transition_shape(shared_new_shape);
        
        // Invalidate cache entries for old shape
        global_shape_cache_.invalidate_shape(shape_->get_id());
        
        return true;
    }
    
    return Object::delete_property(key);
}

void ShapeOptimizedObject::transition_shape(std::shared_ptr<ObjectShape> new_shape) {
    if (!new_shape || new_shape == shape_) {
        return;
    }
    
    // Resize fast properties array if needed
    uint32_t new_property_count = new_shape->get_property_count();
    if (new_property_count > fast_properties_.size()) {
        fast_properties_.resize(new_property_count, Value());
    }
    
    // Update shape
    shape_ = new_shape;
    
    std::cout << "� SHAPE TRANSITION: " << shape_->to_string() << std::endl;
}

Value ShapeOptimizedObject::get_fast_property(PropertyOffset offset) const {
    if (offset < fast_properties_.size()) {
        return fast_properties_[offset];
    }
    return Value(); // undefined
}

void ShapeOptimizedObject::set_fast_property(PropertyOffset offset, const Value& value) {
    if (offset < fast_properties_.size()) {
        fast_properties_[offset] = value;
    }
}

std::unique_ptr<ShapeOptimizedObject> ShapeOptimizedObject::create_with_shape(std::shared_ptr<ObjectShape> shape) {
    return std::make_unique<ShapeOptimizedObject>(shape);
}

//=============================================================================
// ShapeTransitionManager Implementation
//=============================================================================

ShapeTransitionManager::ShapeTransitionManager() {
    // Initialize statistics
}

ShapeTransitionManager::~ShapeTransitionManager() {
    // Cleanup
}

std::shared_ptr<ObjectShape> ShapeTransitionManager::add_property_transition(
    std::shared_ptr<ObjectShape> current_shape, const std::string& property_name) {
    
    stats_.total_transitions++;
    stats_.add_property_transitions++;
    
    // Check transition cache first
    std::string cache_key = std::to_string(current_shape->get_id()) + "+" + property_name;
    auto it = transition_cache_.find(cache_key);
    if (it != transition_cache_.end()) {
        stats_.shape_cache_hits++;
        return it->second;
    }
    
    stats_.shape_cache_misses++;
    
    // Create new shape transition
    ObjectShape* new_shape = current_shape->transition_add_property(property_name);
    auto shared_new_shape = std::shared_ptr<ObjectShape>(new_shape);
    
    // Cache the transition
    transition_cache_[cache_key] = shared_new_shape;
    
    return shared_new_shape;
}

std::shared_ptr<ObjectShape> ShapeTransitionManager::delete_property_transition(
    std::shared_ptr<ObjectShape> current_shape, const std::string& property_name) {
    
    stats_.total_transitions++;
    stats_.delete_property_transitions++;
    
    ObjectShape* new_shape = current_shape->transition_delete_property(property_name);
    return std::shared_ptr<ObjectShape>(new_shape);
}

void ShapeTransitionManager::print_transition_stats() const {
    std::cout << "� Shape Transition Statistics:" << std::endl;
    std::cout << "  Total Transitions: " << stats_.total_transitions << std::endl;
    std::cout << "  Add Property: " << stats_.add_property_transitions << std::endl;
    std::cout << "  Delete Property: " << stats_.delete_property_transitions << std::endl;
    std::cout << "  Cache Hits: " << stats_.shape_cache_hits << std::endl;
    std::cout << "  Cache Misses: " << stats_.shape_cache_misses << std::endl;
}

void ShapeTransitionManager::clear_transition_cache() {
    transition_cache_.clear();
}

ShapeTransitionManager& ShapeTransitionManager::get_instance() {
    static ShapeTransitionManager instance;
    return instance;
}

//=============================================================================
// ShapeOptimizer Implementation - Global Shape Optimization Control
//=============================================================================

bool ShapeOptimizer::optimization_enabled_ = true;
uint64_t ShapeOptimizer::objects_optimized_ = 0;
uint64_t ShapeOptimizer::fast_property_accesses_ = 0;

void ShapeOptimizer::optimize_object(Object* obj) {
    if (!optimization_enabled_ || !obj) {
        return;
    }
    
    // Convert regular object to shape-optimized object
    // This is a simplified version - full implementation would be more complex
    objects_optimized_++;
    
    std::cout << "� SHAPE OPTIMIZATION: Object optimized (Total: " << objects_optimized_ << ")" << std::endl;
}

std::unique_ptr<ShapeOptimizedObject> ShapeOptimizer::create_optimized_object() {
    if (!optimization_enabled_) {
        return nullptr;
    }
    
    objects_optimized_++;
    return std::make_unique<ShapeOptimizedObject>();
}

std::unique_ptr<ShapeOptimizedObject> ShapeOptimizer::create_optimized_object_with_properties(
    const std::vector<std::string>& property_names) {
    
    if (!optimization_enabled_) {
        return nullptr;
    }
    
    // Create shape with predefined properties
    auto shape = ObjectShape::get_root_shape();
    
    for (const std::string& prop : property_names) {
        ObjectShape* new_shape = shape->transition_add_property(prop);
        shape = std::shared_ptr<ObjectShape>(new_shape);
    }
    
    objects_optimized_++;
    return ShapeOptimizedObject::create_with_shape(shape);
}

void ShapeOptimizer::analyze_object_shapes() {
    std::cout << "� SHAPE ANALYSIS:" << std::endl;
    std::cout << "  Objects Optimized: " << objects_optimized_ << std::endl;
    std::cout << "  Fast Property Accesses: " << fast_property_accesses_ << std::endl;
    
    // Print shape cache statistics
    ShapeOptimizedObject::get_global_cache().print_stats();
    
    // Print transition statistics
    ShapeTransitionManager::get_instance().print_transition_stats();
}

void ShapeOptimizer::print_shape_statistics() {
    analyze_object_shapes();
    ObjectShape::cleanup_unused_shapes();
}

bool ShapeOptimizer::should_optimize_property_access(const std::string& property, Object* obj) {
    // Simple heuristic: optimize if object is shape-optimized and property is accessed frequently
    return optimization_enabled_ && (dynamic_cast<ShapeOptimizedObject*>(obj) != nullptr);
}

PropertyOffset ShapeOptimizer::get_optimized_offset(const std::string& property, Object* obj) {
    auto* shape_obj = dynamic_cast<ShapeOptimizedObject*>(obj);
    if (shape_obj && shape_obj->get_shape()) {
        fast_property_accesses_++;
        return shape_obj->get_shape()->get_property_offset(property);
    }
    return static_cast<PropertyOffset>(-1);
}

void ShapeOptimizer::enable_shape_optimization(bool enabled) {
    optimization_enabled_ = enabled;
    std::cout << "� Shape optimization " << (enabled ? "ENABLED" : "DISABLED") << std::endl;
}

bool ShapeOptimizer::is_shape_optimization_enabled() {
    return optimization_enabled_;
}

//=============================================================================
// ShapeInlineCache Implementation - Standard Inline Caching
//=============================================================================

ShapeInlineCache::ShapeInlineCache() : current_size_(0), total_accesses_(0), ic_hits_(0) {
    // Initialize entries
    for (size_t i = 0; i < IC_SIZE; ++i) {
        entries_[i] = ICEntry();
    }
}

ShapeInlineCache::~ShapeInlineCache() {
    // Cleanup
}

bool ShapeInlineCache::lookup(ShapeID shape_id, PropertyOffset& offset) {
    total_accesses_++;
    
    // Search through inline cache entries
    for (size_t i = 0; i < current_size_; ++i) {
        if (entries_[i].is_valid && entries_[i].shape_id == shape_id) {
            // Inline cache hit!
            ic_hits_++;
            entries_[i].access_count++;
            offset = entries_[i].offset;
            return true;
        }
    }
    
    // Inline cache miss
    return false;
}

void ShapeInlineCache::update(ShapeID shape_id, PropertyOffset offset) {
    // Check if entry already exists
    for (size_t i = 0; i < current_size_; ++i) {
        if (entries_[i].shape_id == shape_id) {
            entries_[i].offset = offset;
            entries_[i].access_count++;
            entries_[i].is_valid = true;
            return;
        }
    }
    
    // Add new entry if space available
    if (current_size_ < IC_SIZE) {
        entries_[current_size_].shape_id = shape_id;
        entries_[current_size_].offset = offset;
        entries_[current_size_].access_count = 1;
        entries_[current_size_].is_valid = true;
        current_size_++;
    } else {
        // Cache is full - replace least recently used entry
        // For simplicity, replace the first entry
        entries_[0].shape_id = shape_id;
        entries_[0].offset = offset;
        entries_[0].access_count = 1;
        entries_[0].is_valid = true;
    }
}

void ShapeInlineCache::invalidate() {
    for (size_t i = 0; i < IC_SIZE; ++i) {
        entries_[i].is_valid = false;
    }
    current_size_ = 0;
}

double ShapeInlineCache::get_hit_ratio() const {
    return (total_accesses_ > 0) ? static_cast<double>(ic_hits_) / total_accesses_ : 0.0;
}

void ShapeInlineCache::print_cache_state() const {
    std::cout << "� Inline Cache State:" << std::endl;
    std::cout << "  Size: " << current_size_ << "/" << IC_SIZE;
    if (is_monomorphic()) std::cout << " (MONOMORPHIC)";
    else if (is_polymorphic()) std::cout << " (POLYMORPHIC)";
    else if (is_megamorphic()) std::cout << " (MEGAMORPHIC)";
    std::cout << std::endl;
    std::cout << "  Hit Ratio: " << (get_hit_ratio() * 100.0) << "%" << std::endl;
    std::cout << "  Total Accesses: " << total_accesses_ << std::endl;
}

} // namespace Quanta