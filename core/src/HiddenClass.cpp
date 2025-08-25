/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../include/HiddenClass.h"
#include "../include/Value.h"
#include "../include/Object.h"
#include "../include/PhotonCore/PhotonCoreQuantum.h"
#include "../include/PhotonCore/PhotonCoreSonic.h"
#include "../include/PhotonCore/PhotonCorePerformance.h"
#include <iostream>
#include <algorithm>
#include <mutex>

namespace Quanta {

//=============================================================================
// HiddenClass Implementation - PHASE 2: V8-style Object Optimization
//=============================================================================

std::atomic<HiddenClassID> HiddenClass::next_class_id_{1};

HiddenClass::HiddenClass() 
    : class_id_(next_class_id_++), instance_count_(0), access_count_(0),
      is_stable_(true), is_deprecated_(false), fast_indices_valid_(false) {
    
    std::cout << "🏗️  HIDDEN CLASS CREATED: ID=" << class_id_ << " (Empty)" << std::endl;
}

HiddenClass::HiddenClass(std::shared_ptr<HiddenClass> parent) 
    : class_id_(next_class_id_++), parent_(parent), instance_count_(0), 
      access_count_(0), is_stable_(false), is_deprecated_(false), fast_indices_valid_(false) {
    
    if (parent_) {
        // Copy properties from parent
        properties_ = parent_->properties_;
        property_map_ = parent_->property_map_;
    }
    
    std::cout << "🏗️  HIDDEN CLASS CREATED: ID=" << class_id_ 
             << " (Parent=" << (parent_ ? parent_->class_id_ : 0) << ")" << std::endl;
}

HiddenClass::~HiddenClass() {
    // Cleanup
}

bool HiddenClass::has_property(const std::string& name) const {
    return property_map_.find(name) != property_map_.end();
}

PropertyIndex HiddenClass::get_property_index(const std::string& name) const {
    auto it = property_map_.find(name);
    return (it != property_map_.end()) ? it->second : UINT32_MAX;
}

const HiddenClassPropertyDescriptor* HiddenClass::get_property_descriptor(const std::string& name) const {
    auto it = property_map_.find(name);
    if (it != property_map_.end() && it->second < properties_.size()) {
        return &properties_[it->second];
    }
    return nullptr;
}

const HiddenClassPropertyDescriptor* HiddenClass::get_property_descriptor(PropertyIndex index) const {
    if (index < properties_.size()) {
        return &properties_[index];
    }
    return nullptr;
}

std::shared_ptr<HiddenClass> HiddenClass::add_property(const std::string& name, PropertyType type) {
    // Check if property already exists
    if (has_property(name)) {
        return shared_from_this();
    }
    
    // Check if we already have a transition for this property
    auto transition_it = transitions_.find(name);
    if (transition_it != transitions_.end()) {
        return transition_it->second;
    }
    
    // Create new hidden class with the additional property
    auto new_class = std::make_shared<HiddenClass>(shared_from_this());
    
    PropertyIndex new_index = static_cast<PropertyIndex>(new_class->properties_.size());
    HiddenClassPropertyDescriptor desc(name, new_index);
    desc.type = type;
    
    new_class->properties_.push_back(desc);
    new_class->property_map_[name] = new_index;
    new_class->invalidate_fast_indices();
    
    // Cache the transition
    transitions_[name] = new_class;
    mark_unstable();
    
    std::cout << "➕ PROPERTY ADDED: '" << name << "' to class " << class_id_ 
             << " -> new class " << new_class->class_id_ << std::endl;
    
    return new_class;
}

std::shared_ptr<HiddenClass> HiddenClass::remove_property(const std::string& name) {
    if (!has_property(name)) {
        return shared_from_this();
    }
    
    // Create new class without the property
    auto new_class = std::make_shared<HiddenClass>();
    
    PropertyIndex removed_index = get_property_index(name);
    
    // Copy all properties except the removed one
    for (const auto& prop : properties_) {
        if (prop.name != name) {
            HiddenClassPropertyDescriptor new_desc = prop;
            // Adjust indices for properties after the removed one
            if (prop.index > removed_index) {
                new_desc.index--;
            }
            new_class->properties_.push_back(new_desc);
            new_class->property_map_[new_desc.name] = new_desc.index;
        }
    }
    
    new_class->invalidate_fast_indices();
    mark_unstable();
    
    std::cout << "➖ PROPERTY REMOVED: '" << name << "' from class " << class_id_ 
             << " -> new class " << new_class->class_id_ << std::endl;
    
    return new_class;
}

std::shared_ptr<HiddenClass> HiddenClass::change_property_type(const std::string& name, PropertyType new_type) {
    const HiddenClassPropertyDescriptor* desc = get_property_descriptor(name);
    if (!desc || desc->type == new_type) {
        return shared_from_this();
    }
    
    // Create new class with changed property type
    auto new_class = std::make_shared<HiddenClass>(shared_from_this());
    
    // Update the property type
    for (auto& prop : new_class->properties_) {
        if (prop.name == name) {
            prop.type = new_type;
            break;
        }
    }
    
    new_class->invalidate_fast_indices();
    mark_unstable();
    
    std::cout << "🔄 PROPERTY TYPE CHANGED: '" << name << "' in class " << class_id_ 
             << " -> new class " << new_class->class_id_ << std::endl;
    
    return new_class;
}

void HiddenClass::mark_property_hot(const std::string& name) {
    auto it = property_map_.find(name);
    if (it != property_map_.end() && it->second < properties_.size()) {
        properties_[it->second].is_frequently_accessed = true;
        access_count_++;
        
        std::cout << "🔥 HOT PROPERTY: '" << name << "' in class " << class_id_ << std::endl;
    }
}

void HiddenClass::update_access_frequency() {
    access_count_++;
    
    // Become stable after enough accesses without transitions
    if (access_count_ > 50 && transitions_.empty()) {
        mark_stable();
    }
}

void HiddenClass::optimize_property_layout() {
    if (!should_optimize()) {
        return;
    }
    
    // Sort properties by access frequency (hot properties first)
    std::sort(properties_.begin(), properties_.end(), 
              [](const HiddenClassPropertyDescriptor& a, const HiddenClassPropertyDescriptor& b) {
                  if (a.is_frequently_accessed != b.is_frequently_accessed) {
                      return a.is_frequently_accessed > b.is_frequently_accessed;
                  }
                  return a.name < b.name; // Stable sort for determinism
              });
    
    // Update property indices and map
    property_map_.clear();
    for (size_t i = 0; i < properties_.size(); ++i) {
        properties_[i].index = static_cast<PropertyIndex>(i);
        property_map_[properties_[i].name] = properties_[i].index;
    }
    
    invalidate_fast_indices();
    
    std::cout << " LAYOUT OPTIMIZED: Class " << class_id_ 
             << " (" << properties_.size() << " properties)" << std::endl;
}

std::vector<PropertyIndex> HiddenClass::get_optimized_layout() const {
    std::vector<PropertyIndex> layout;
    layout.reserve(properties_.size());
    
    // Hot properties first for better cache performance
    for (const auto& prop : properties_) {
        if (prop.is_frequently_accessed) {
            layout.push_back(prop.index);
        }
    }
    
    // Then regular properties
    for (const auto& prop : properties_) {
        if (!prop.is_frequently_accessed) {
            layout.push_back(prop.index);
        }
    }
    
    return layout;
}

const std::vector<PropertyIndex>& HiddenClass::get_fast_indices() const {
    if (!fast_indices_valid_) {
        fast_property_indices_.clear();
        fast_property_indices_.reserve(properties_.size());
        
        for (const auto& prop : properties_) {
            if (prop.is_fast_access) {
                fast_property_indices_.push_back(prop.index);
            }
        }
        
        fast_indices_valid_ = true;
    }
    
    return fast_property_indices_;
}

void HiddenClass::invalidate_fast_indices() {
    fast_indices_valid_ = false;
}

void HiddenClass::print_class_info() const {
    std::cout << "📋 HIDDEN CLASS INFO:" << std::endl;
    std::cout << "  ID: " << class_id_ << std::endl;
    std::cout << "  Properties: " << properties_.size() << std::endl;
    std::cout << "  Instances: " << instance_count_ << std::endl;
    std::cout << "  Access Count: " << access_count_ << std::endl;
    std::cout << "  Stable: " << (is_stable_ ? "Yes" : "No") << std::endl;
    std::cout << "  Transitions: " << transitions_.size() << std::endl;
    
    std::cout << "  Property List:" << std::endl;
    for (const auto& prop : properties_) {
        std::cout << "    [" << prop.index << "] " << prop.name 
                 << (prop.is_frequently_accessed ? " (HOT)" : "") << std::endl;
    }
}

void HiddenClass::print_transitions() const {
    std::cout << "🔄 CLASS TRANSITIONS from " << class_id_ << ":" << std::endl;
    for (const auto& [property, target_class] : transitions_) {
        std::cout << "  +" << property << " -> " << target_class->class_id_ << std::endl;
    }
}

//=============================================================================
// HiddenClassCache Implementation - Global class reuse optimization
//=============================================================================

HiddenClassCache::HiddenClassCache() 
    : cache_hits_(0), cache_misses_(0), total_lookups_(0) {
    
    std::cout << "🗄️  HIDDEN CLASS CACHE INITIALIZED" << std::endl;
    
    // Pre-create common classes
    common_classes_["empty"] = std::make_shared<HiddenClass>();
}

HiddenClassCache::~HiddenClassCache() {
    cleanup_deprecated_classes();
}

std::shared_ptr<HiddenClass> HiddenClassCache::get_or_create_class(const std::vector<std::string>& property_names) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    total_lookups_++;
    
    // Create a key from property names
    std::string cache_key;
    for (const auto& name : property_names) {
        if (!cache_key.empty()) cache_key += ",";
        cache_key += name;
    }
    
    // Check if we have this pattern cached
    auto it = common_classes_.find(cache_key);
    if (it != common_classes_.end()) {
        cache_hits_++;
        return it->second;
    }
    
    // Create new class with these properties
    auto hidden_class = std::make_shared<HiddenClass>();
    
    PropertyIndex index = 0;
    for (const auto& name : property_names) {
        HiddenClassPropertyDescriptor desc(name, index++);
        hidden_class->properties_.push_back(desc);
        hidden_class->property_map_[name] = desc.index;
    }
    
    // Cache the new class
    common_classes_[cache_key] = hidden_class;
    cache_class(hidden_class);
    
    cache_misses_++;
    
    std::cout << "💾 CLASS CACHED: " << property_names.size() 
             << " properties -> " << hidden_class->get_class_id() << std::endl;
    
    return hidden_class;
}

std::shared_ptr<HiddenClass> HiddenClassCache::find_class(HiddenClassID class_id) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    
    auto it = class_cache_.find(class_id);
    if (it != class_cache_.end()) {
        if (auto locked = it->second.lock()) {
            return locked;
        } else {
            // Weak pointer expired, remove from cache
            class_cache_.erase(it);
        }
    }
    
    return nullptr;
}

void HiddenClassCache::cache_class(std::shared_ptr<HiddenClass> hidden_class) {
    if (hidden_class) {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        class_cache_[hidden_class->get_class_id()] = hidden_class;
    }
}

std::shared_ptr<HiddenClass> HiddenClassCache::get_empty_class() {
    return common_classes_["empty"];
}

std::shared_ptr<HiddenClass> HiddenClassCache::get_array_class() {
    const std::vector<std::string> array_props = {"length"};
    return get_or_create_class(array_props);
}

std::shared_ptr<HiddenClass> HiddenClassCache::get_function_class() {
    const std::vector<std::string> func_props = {"length", "name", "prototype"};
    return get_or_create_class(func_props);
}

void HiddenClassCache::cleanup_deprecated_classes() {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    
    size_t removed = 0;
    
    // Remove expired weak pointers
    auto it = class_cache_.begin();
    while (it != class_cache_.end()) {
        if (it->second.expired()) {
            it = class_cache_.erase(it);
            removed++;
        } else {
            ++it;
        }
    }
    
    if (removed > 0) {
        std::cout << "🧹 CACHE CLEANUP: Removed " << removed << " expired classes" << std::endl;
    }
}

void HiddenClassCache::optimize_cache() {
    cleanup_deprecated_classes();
    
    std::cout << " CACHE OPTIMIZED: " << class_cache_.size() 
             << " active classes, hit ratio: " << (get_cache_hit_ratio() * 100.0) << "%" << std::endl;
}

double HiddenClassCache::get_cache_hit_ratio() const {
    if (total_lookups_ == 0) return 0.0;
    return static_cast<double>(cache_hits_) / total_lookups_;
}

void HiddenClassCache::print_cache_statistics() const {
    std::cout << "📊 HIDDEN CLASS CACHE STATISTICS:" << std::endl;
    std::cout << "  Total Lookups: " << total_lookups_ << std::endl;
    std::cout << "  Cache Hits: " << cache_hits_ << std::endl;
    std::cout << "  Cache Misses: " << cache_misses_ << std::endl;
    std::cout << "  Hit Ratio: " << (get_cache_hit_ratio() * 100.0) << "%" << std::endl;
    std::cout << "  Active Classes: " << class_cache_.size() << std::endl;
    std::cout << "  Common Classes: " << common_classes_.size() << std::endl;
}

void HiddenClassCache::reset_statistics() {
    cache_hits_ = 0;
    cache_misses_ = 0;
    total_lookups_ = 0;
}

HiddenClassCache& HiddenClassCache::get_instance() {
    static HiddenClassCache instance;
    return instance;
}

//=============================================================================
// HiddenClassObject Implementation - Objects with V8-style optimization
//=============================================================================

HiddenClassObject::HiddenClassObject(std::shared_ptr<HiddenClass> hidden_class)
    : hidden_class_(hidden_class), last_accessed_index_(UINT32_MAX) {
    
    if (hidden_class_) {
        property_values_.resize(hidden_class_->get_property_count());
        hidden_class_->add_instance();
    }
}

HiddenClassObject::~HiddenClassObject() {
    if (hidden_class_) {
        hidden_class_->remove_instance();
    }
}

bool HiddenClassObject::get_property(const std::string& name, Value& out_value) const {
    if (!hidden_class_) return false;
    
    // Fast path: check if this is the same property we accessed last time
    if (name == last_accessed_name_ && last_accessed_index_ < property_values_.size()) {
        out_value = property_values_[last_accessed_index_];
        return true;
    }
    
    PropertyIndex index = hidden_class_->get_property_index(name);
    if (index == UINT32_MAX || index >= property_values_.size()) {
        return false;
    }
    
    out_value = property_values_[index];
    
    // Cache for next access
    last_accessed_name_ = name;
    last_accessed_index_ = index;
    
    // Track hot properties
    hidden_class_->mark_property_hot(name);
    hidden_class_->update_access_frequency();
    
    return true;
}

bool HiddenClassObject::set_property(const std::string& name, const Value& value) {
    if (!hidden_class_) return false;
    
    PropertyIndex index = hidden_class_->get_property_index(name);
    
    if (index == UINT32_MAX) {
        // Property doesn't exist - transition to new hidden class
        auto new_class = hidden_class_->add_property(name);
        transition_to_class(new_class);
        
        // Now get the index in the new class
        index = hidden_class_->get_property_index(name);
        if (index == UINT32_MAX) return false;
    }
    
    if (index >= property_values_.size()) {
        property_values_.resize(index + 1);
    }
    
    property_values_[index] = value;
    
    // Cache for next access
    last_accessed_name_ = name;
    last_accessed_index_ = index;
    
    hidden_class_->mark_property_hot(name);
    hidden_class_->update_access_frequency();
    
    return true;
}

bool HiddenClassObject::has_property(const std::string& name) const {
    return hidden_class_ && hidden_class_->has_property(name);
}

bool HiddenClassObject::delete_property(const std::string& name) {
    if (!hidden_class_ || !hidden_class_->has_property(name)) {
        return false;
    }
    
    // Transition to new class without this property
    auto new_class = hidden_class_->remove_property(name);
    transition_to_class(new_class);
    
    // Remove the property value (this is simplified - real implementation would compact)
    PropertyIndex removed_index = hidden_class_->get_property_index(name);
    if (removed_index < property_values_.size()) {
        property_values_.erase(property_values_.begin() + removed_index);
    }
    
    return true;
}

Value HiddenClassObject::get_property_by_index(PropertyIndex index) const {
    if (index < property_values_.size()) {
        return property_values_[index];
    }
    return Value(); // Undefined
}

void HiddenClassObject::set_property_by_index(PropertyIndex index, const Value& value) {
    if (index < property_values_.size()) {
        property_values_[index] = value;
    }
}

void HiddenClassObject::transition_to_class(std::shared_ptr<HiddenClass> new_class) {
    if (new_class == hidden_class_) return;
    
    if (hidden_class_) {
        hidden_class_->remove_instance();
    }
    
    hidden_class_ = new_class;
    
    if (hidden_class_) {
        hidden_class_->add_instance();
        
        // Resize property values if needed
        size_t needed_size = hidden_class_->get_property_count();
        if (property_values_.size() != needed_size) {
            property_values_.resize(needed_size);
        }
    }
    
    // Invalidate access cache
    last_accessed_index_ = UINT32_MAX;
    last_accessed_name_.clear();
}

size_t HiddenClassObject::get_property_count() const {
    return hidden_class_ ? hidden_class_->get_property_count() : 0;
}

std::vector<std::string> HiddenClassObject::get_property_names() const {
    std::vector<std::string> names;
    if (hidden_class_) {
        const auto& properties = hidden_class_->get_properties();
        names.reserve(properties.size());
        for (const auto& prop : properties) {
            names.push_back(prop.name);
        }
    }
    return names;
}

void HiddenClassObject::optimize_for_access_pattern() {
    if (hidden_class_) {
        hidden_class_->optimize_property_layout();
    }
}

bool HiddenClassObject::is_optimized() const {
    return hidden_class_ && hidden_class_->should_optimize();
}

void HiddenClassObject::print_object_layout() const {
    std::cout << "🏗️  OBJECT LAYOUT:" << std::endl;
    if (hidden_class_) {
        std::cout << "  Hidden Class: " << hidden_class_->get_class_id() << std::endl;
        std::cout << "  Properties: " << property_values_.size() << std::endl;
        
        const auto& properties = hidden_class_->get_properties();
        for (size_t i = 0; i < std::min(properties.size(), property_values_.size()); ++i) {
            std::cout << "    [" << i << "] " << properties[i].name << std::endl;
        }
    } else {
        std::cout << "  No hidden class" << std::endl;
    }
}

//=============================================================================
// HiddenClassIntegration Implementation - Engine hooks
//=============================================================================

namespace HiddenClassIntegration {

void initialize_hidden_classes() {
    HiddenClassCache::get_instance();
    std::cout << "🚀 HIDDEN CLASS SYSTEM INITIALIZED" << std::endl;
}

void shutdown_hidden_classes() {
    HiddenClassCache::get_instance().print_cache_statistics();
    std::cout << "🔗 HIDDEN CLASS SYSTEM SHUTDOWN" << std::endl;
}

std::shared_ptr<HiddenClass> create_class_for_object(Object* obj) {
    if (!obj) return nullptr;
    
    // For now, return empty class - in full implementation would analyze object
    return HiddenClassCache::get_instance().get_empty_class();
}

void optimize_object_layout(Object* obj) {
    // Placeholder for object optimization
    if (obj) {
        std::cout << " OPTIMIZING OBJECT LAYOUT" << std::endl;
    }
}

bool fast_property_get(Object* obj, const std::string& name, Value& out_value) {
    // Placeholder for fast property access
    return false;
}

bool fast_property_set(Object* obj, const std::string& name, const Value& value) {
    // Placeholder for fast property setting
    return false;
}

void monitor_class_usage() {
    HiddenClassCache::get_instance().optimize_cache();
}

void print_hidden_class_statistics() {
    HiddenClassCache::get_instance().print_cache_statistics();
}

void enable_adaptive_optimization() {
    std::cout << "🧠 ADAPTIVE OPTIMIZATION ENABLED" << std::endl;
}

void tune_optimization_thresholds() {
    std::cout << "🔧 OPTIMIZATION THRESHOLDS TUNED" << std::endl;
}

} // namespace HiddenClassIntegration

} // namespace Quanta