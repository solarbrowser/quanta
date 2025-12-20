/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "quanta/Value.h"
#include "quanta/SIMD.h"
#include <vector>
#include <functional>
#include <immintrin.h>

namespace Quanta {


enum class ArrayType : uint8_t {
    UNKNOWN = 0,
    PACKED_SMI,
    PACKED_DOUBLE,
    PACKED_ELEMENTS,
    HOLEY_SMI,
    HOLEY_DOUBLE,
    HOLEY_ELEMENTS
};

class NativeArrayOps {
public:
    static Value native_map(const Value& array, const std::function<Value(Value, int)>& callback);
    
    static Value native_reduce(const Value& array, const std::function<Value(Value, Value, int)>& callback, const Value& initial);
    
    static Value native_filter(const Value& array, const std::function<bool(Value, int)>& callback);
    
    static Value native_sum_numbers(const Value& array);
    static Value native_multiply_numbers(const Value& array, double factor);
    static Value native_find_max(const Value& array);
    static Value native_find_min(const Value& array);
    
    static Value simd_sum_double_array(const std::vector<double>& data);
    static std::vector<double> simd_multiply_double_array(const std::vector<double>& data, double factor);
    static std::vector<double> simd_add_double_arrays(const std::vector<double>& a, const std::vector<double>& b);
    
    static ArrayType detect_array_type(const Value& array);
    static bool is_packed_array(const Value& array);
    static bool can_use_simd(const Value& array);
    
    static inline bool bounds_check_elimination_safe(size_t index, size_t length) {
        return index < length;
    }
    
    static void native_for_of(const Value& array, const std::function<void(Value, int)>& callback);
    
private:
    static bool all_numbers(const Value& array);
    static bool all_integers(const Value& array);
    static std::vector<double> extract_numbers(const Value& array);
};


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
    
    static LoopInfo analyze_loop(const std::string& loop_body, size_t iterations);
    
    static std::string eliminate_bounds_checks(const std::string& loop_code);
    
    static std::string eliminate_type_guards(const std::string& loop_code);
    
    static bool can_vectorize_loop(const LoopInfo& info);
    static std::string vectorize_loop(const std::string& loop_code);
};


class EscapeAnalyzer {
public:
    struct ObjectInfo {
        bool escapes_function;
        bool escapes_loop;
        bool stored_in_heap;
        bool passed_to_function;
        size_t estimated_size;
        
        ObjectInfo() : escapes_function(false), escapes_loop(false),
                      stored_in_heap(false), passed_to_function(false), estimated_size(0) {}
        
        bool can_stack_allocate() const {
            return !escapes_function && !stored_in_heap && estimated_size < 1024;
        }
    };
    
    static ObjectInfo analyze_object_escape(const std::string& code, const std::string& var_name);
    
    static std::vector<std::string> find_stack_allocation_candidates(const std::string& code);
    
    static std::string optimize_for_stack_allocation(const std::string& code);
};


class ShapeOptimizer {
public:
    static bool predict_object_shape(const Value& object, const std::string& property);
    
    static inline Value ultra_fast_property_get(const Value& object, size_t offset) {
        return object.get_property_direct(offset);
    }
    
    static inline void ultra_fast_property_set(Value& object, size_t offset, const Value& value) {
        object.set_property_direct(offset, value);
    }
    
    static Value dispatch_method_by_shape(const Value& object, const std::string& method, const std::vector<Value>& args);
};

}
