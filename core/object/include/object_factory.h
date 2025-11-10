/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "../../include/Value.h"
#include <memory>
#include <vector>

namespace Quanta {

class Object;

/**
 * ObjectCreator namespace for creating and managing JavaScript objects
 * Includes memory pool optimization for high-performance object allocation
 */
namespace ObjectCreator {

// Memory pool management
void initialize_memory_pools();
void cleanup_memory_pools();

// Object creation (optimized with pools)
std::unique_ptr<Object> create_object();
std::unique_ptr<Object> create_array();
std::unique_ptr<Object> create_array(size_t length);
std::unique_ptr<Object> create_array(const std::vector<Value>& elements);

// Function creation (placeholders for now)
void* create_function(const std::string& name, void* native_func);
void* create_native_function(const std::string& name, void* native_func);

// Specialized object creation
std::unique_ptr<Object> create_error(const std::string& message);
std::unique_ptr<Object> create_date();
std::unique_ptr<Object> create_regexp(const std::string& pattern);

// Prototype access
Object* get_object_prototype();
Object* get_array_prototype();
Object* get_function_prototype();

// Pool statistics
size_t get_pool_size();
size_t get_available_objects();
size_t get_available_arrays();

} // namespace ObjectCreator

} // namespace Quanta