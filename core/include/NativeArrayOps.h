/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "Value.h"
#include "SIMD.h"
#include <vector>
#include <functional>
#include <immintrin.h>

namespace Quanta {

//=============================================================================
// Native Array Operations - High-performance Implementation
//=============================================================================

enum class ArrayType : uint8_t {
    UNKNOWN = 0,
    PACKED_SMI,        // [1, 2, 3] - integers only
    PACKED_DOUBLE,     // [1.1, 2.2, 3.3] - doubles only
    PACKED_ELEMENTS,   // [1, "str", obj] - mixed
    HOLEY_SMI,         // [1, , 3] - with holes
    HOLEY_DOUBLE,      // [1.1, , 3.3] - holes
    HOLEY_ELEMENTS     // Mixed with holes
};

class NativeArrayOps {
public:
    // € ULTRA-FAST MAP OPERATION
    static Value native_map(const Value& array, const std::function<Value(Value, int)>& callback);
    
    // ¥ ULTRA-FAST REDUCE OPERATION
    static Value native_reduce(const Value& array, const std::function<Value(Value, Value, int)>& callback, const Value& initial);
    
    // âš¡ ULTRA-FAST FILTER OPERATION
    static Value native_filter(const Value& array, const std::function<bool(Value, int)>& callback);
    
    // ¯ SPECIALIZED NUMERIC OPERATIONS
    static Value native_sum_numbers(const Value& array);
    static Value native_multiply_numbers(const Value& array, double factor);
    static Value native_find_max(const Value& array);
    static Value native_find_min(const Value& array);
    
    // SIMD ACCELERATED OPERATIONS
    static Value simd_sum_double_array(const std::vector<double>& data);
    static std::vector<double> simd_multiply_double_array(const std::vector<double>& data, double factor);
    static std::vector<double> simd_add_double_arrays(const std::vector<double>& a, const std::vector<double>& b);
    
    // TYPE DETECTION & OPTIMIZATION
    static ArrayType detect_array_type(const Value& array);
    static bool is_packed_array(const Value& array);
    static bool can_use_simd(const Value& array);
    
    // € BOUND CHECK ELIMINATION
    static inline bool bounds_check_elimination_safe(size_t index, size_t length) {
        return index < length; // This will be optimized away in hot loops
    }
    
    // ¯ FAST PATH FOR-OF ITERATION
    static void native_for_of(const Value& array, const std::function<void(Value, int)>& callback);
    
private:
    // Helper functions for type specialization
    static bool all_numbers(const Value& array);
    static bool all_integers(const Value& array);
    static std::vector<double> extract_numbers(const Value& array);
};

//=============================================================================
// Loop Optimization Engine
//=============================================================================

class LoopOptimizer {
public:
    struct LoopInfo {
        bool has_bounds_checks;
        bool has_type_guards;
        bool is_numeric_only;
        bool can_vectorize;
        size_t iteration_count;
        
        LoopInfo() : has_bounds_checks(false), has_type_guards(false), 
                    is_numeric_only(false), can_vectorize(false), iteration_count(0) {}
    };
    
    // ¥ HOT LOOP DETECTION
    static LoopInfo analyze_loop(const std::string& loop_body, size_t iterations);
    
    // BOUND CHECK ELIMINATION
    static std::string eliminate_bounds_checks(const std::string& loop_code);
    
    // âš¡ TYPE GUARD ELIMINATION  
    static std::string eliminate_type_guards(const std::string& loop_code);
    
    // € VECTORIZATION OPPORTUNITIES
    static bool can_vectorize_loop(const LoopInfo& info);
    static std::string vectorize_loop(const std::string& loop_code);
};

//=============================================================================
// Escape Analysis - Stack Allocation Optimizer
//=============================================================================

class EscapeAnalyzer {
public:
    struct ObjectInfo {
        bool escapes_function;      // Object pointer leaves function scope
        bool escapes_loop;          // Object pointer leaves loop scope
        bool stored_in_heap;        // Assigned to heap object property
        bool passed_to_function;    // Passed as function argument
        size_t estimated_size;      // Estimated object size
        
        ObjectInfo() : escapes_function(false), escapes_loop(false),
                      stored_in_heap(false), passed_to_function(false), estimated_size(0) {}
        
        bool can_stack_allocate() const {
            return !escapes_function && !stored_in_heap && estimated_size < 1024;
        }
    };
    
    // ESCAPE ANALYSIS
    static ObjectInfo analyze_object_escape(const std::string& code, const std::string& var_name);
    
    // € STACK ALLOCATION OPPORTUNITIES
    static std::vector<std::string> find_stack_allocation_candidates(const std::string& code);
    
    // HEAP ALLOCATION ELIMINATION
    static std::string optimize_for_stack_allocation(const std::string& code);
};

//=============================================================================
// Shape-based Fast Path Object Access
//=============================================================================

class ShapeOptimizer {
public:
    // ¯ SHAPE PREDICTION
    static bool predict_object_shape(const Value& object, const std::string& property);
    
    // € FAST PROPERTY ACCESS (BYPASS ALL CHECKS)
    static inline Value ultra_fast_property_get(const Value& object, size_t offset) {
        // DIRECT MEMORY ACCESS - NO BOUNDS CHECKING
        // This assumes shape validation already passed
        return object.get_property_direct(offset);
    }
    
    // âš¡ FAST PROPERTY SET (BYPASS ALL CHECKS) 
    static inline void ultra_fast_property_set(Value& object, size_t offset, const Value& value) {
        // DIRECT MEMORY WRITE - NO VALIDATION
        object.set_property_direct(offset, value);
    }
    
    // ¥ SHAPE-BASED METHOD DISPATCH
    static Value dispatch_method_by_shape(const Value& object, const std::string& method, const std::vector<Value>& args);
};

} // namespace Quanta