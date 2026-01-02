/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <immintrin.h>
#include <vector>
#include <array>
#include <memory>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

namespace Quanta {


class Value;


using simd_f32x4 = __m128;
using simd_f64x2 = __m128d;
using simd_i32x4 = __m128i;

using simd_f32x8 = __m256;
using simd_f64x4 = __m256d;
using simd_i32x8 = __m256i;

using simd_f32x16 = __m512;
using simd_f64x8 = __m512d;
using simd_i32x16 = __m512i;


struct SIMDCapabilities {
    bool has_sse;
    bool has_sse2;
    bool has_sse3;
    bool has_ssse3;
    bool has_sse4_1;
    bool has_sse4_2;
    bool has_avx;
    bool has_avx2;
    bool has_avx512f;
    bool has_avx512dq;
    bool has_fma;
    bool has_fma4;
    
    uint32_t cache_line_size;
    uint32_t simd_width_bits;
    uint32_t max_vector_elements;
    
    SIMDCapabilities();
    
    void print_capabilities() const;
    const char* get_best_instruction_set() const;
};


template<typename T, size_t Alignment = 64>
class SIMDArray {
private:
    T* data_;
    size_t size_;
    size_t capacity_;
    
    static constexpr size_t SIMD_ALIGNMENT = Alignment;
    
public:
    explicit SIMDArray(size_t size);
    ~SIMDArray();
    
    SIMDArray(const SIMDArray&) = delete;
    SIMDArray& operator=(const SIMDArray&) = delete;
    
    SIMDArray(SIMDArray&& other) noexcept;
    SIMDArray& operator=(SIMDArray&& other) noexcept;
    
    T& operator[](size_t index) { return data_[index]; }
    const T& operator[](size_t index) const { return data_[index]; }
    
    T* data() { return data_; }
    const T* data() const { return data_; }
    
    size_t size() const { return size_; }
    size_t capacity() const { return capacity_; }
    bool empty() const { return size_ == 0; }
    
    void fill(T value);
    void add_scalar(T scalar);
    void multiply_scalar(T scalar);
    void add_array(const SIMDArray<T, Alignment>& other);
    void multiply_array(const SIMDArray<T, Alignment>& other);
    
    T sum() const;
    T average() const;
    T min() const;
    T max() const;
    
    void resize(size_t new_size);
    void reserve(size_t new_capacity);
    SIMDArray<T, Alignment> clone() const;
    
    T* begin() { return data_; }
    T* end() { return data_ + size_; }
    const T* begin() const { return data_; }
    const T* end() const { return data_ + size_; }
};


class SIMDMathEngine {
private:
    SIMDCapabilities capabilities_;
    
    mutable std::atomic<uint64_t> operations_count_;
    mutable std::atomic<uint64_t> total_elements_processed_;
    mutable std::atomic<uint64_t> total_execution_time_ns_;

public:
    SIMDMathEngine();
    ~SIMDMathEngine();
    
    void add_arrays_f32(const float* a, const float* b, float* result, size_t count) const;
    void subtract_arrays_f32(const float* a, const float* b, float* result, size_t count) const;
    void multiply_arrays_f32(const float* a, const float* b, float* result, size_t count) const;
    void divide_arrays_f32(const float* a, const float* b, float* result, size_t count) const;
    
    void add_arrays_f64(const double* a, const double* b, double* result, size_t count) const;
    void subtract_arrays_f64(const double* a, const double* b, double* result, size_t count) const;
    void multiply_arrays_f64(const double* a, const double* b, double* result, size_t count) const;
    void divide_arrays_f64(const double* a, const double* b, double* result, size_t count) const;
    
    void sin_array_f32(const float* input, float* output, size_t count) const;
    void cos_array_f32(const float* input, float* output, size_t count) const;
    void exp_array_f32(const float* input, float* output, size_t count) const;
    void log_array_f32(const float* input, float* output, size_t count) const;
    void sqrt_array_f32(const float* input, float* output, size_t count) const;
    void pow_array_f32(const float* base, const float* exponent, float* result, size_t count) const;
    
    float sum_array_f32(const float* array, size_t count) const;
    double sum_array_f64(const double* array, size_t count) const;
    float min_array_f32(const float* array, size_t count) const;
    float max_array_f32(const float* array, size_t count) const;
    
    float dot_product_f32(const float* a, const float* b, size_t count) const;
    double dot_product_f64(const double* a, const double* b, size_t count) const;
    void matrix_multiply_f32(const float* a, const float* b, float* result, 
                            size_t rows_a, size_t cols_a, size_t cols_b) const;
    
    void copy_array_f32(const float* source, float* dest, size_t count) const;
    void fill_array_f32(float* array, float value, size_t count) const;
    void zero_array_f32(float* array, size_t count) const;
    
    uint64_t get_operations_count() const { return operations_count_.load(); }
    uint64_t get_total_elements_processed() const { return total_elements_processed_.load(); }
    uint64_t get_average_execution_time_ns() const;
    double get_throughput_elements_per_second() const;
    
    void reset_performance_counters();
    void print_performance_report() const;
    
    const SIMDCapabilities& get_capabilities() const { return capabilities_; }
    size_t get_optimal_vector_size_f32() const;
    size_t get_optimal_vector_size_f64() const;
    
    static SIMDMathEngine& get_instance();
};


class SIMDJavaScriptArrays {
private:
    SIMDMathEngine* math_engine_;
    
    struct ArrayOpStats {
        uint64_t array_operations;
        uint64_t elements_processed;
        uint64_t total_time_ns;
        uint64_t simd_accelerated_ops;
        uint64_t fallback_ops;
    };
    
    mutable ArrayOpStats stats_;

public:
    SIMDJavaScriptArrays();
    ~SIMDJavaScriptArrays();
    
    bool simd_array_map(const std::vector<Value>& input, std::vector<Value>& output, 
                       const std::string& operation) const;
    bool simd_array_reduce(const std::vector<Value>& input, Value& result, 
                          const std::string& operation) const;
    bool simd_array_filter(const std::vector<Value>& input, std::vector<Value>& output, 
                          const std::string& condition) const;
    
    bool simd_array_add(const std::vector<Value>& a, const std::vector<Value>& b, 
                       std::vector<Value>& result) const;
    bool simd_array_multiply(const std::vector<Value>& a, const std::vector<Value>& b, 
                            std::vector<Value>& result) const;
    bool simd_array_dot_product(const std::vector<Value>& a, const std::vector<Value>& b, 
                               Value& result) const;
    
    bool simd_array_sum(const std::vector<Value>& input, Value& result) const;
    bool simd_array_average(const std::vector<Value>& input, Value& result) const;
    bool simd_array_min_max(const std::vector<Value>& input, Value& min, Value& max) const;
    
    void print_array_operation_stats() const;
    double get_simd_acceleration_ratio() const;
    
    bool should_use_simd(size_t array_size) const;
    size_t get_optimal_chunk_size() const;
    
    static SIMDJavaScriptArrays& get_instance();
};


class SIMDVectorMath {
public:
    struct Vector3f {
        alignas(16) float data[4];
        
        Vector3f() : data{0.0f, 0.0f, 0.0f, 0.0f} {}
        Vector3f(float x, float y, float z) : data{x, y, z, 0.0f} {}
        
        float& x() { return data[0]; }
        float& y() { return data[1]; }
        float& z() { return data[2]; }
        
        const float& x() const { return data[0]; }
        const float& y() const { return data[1]; }
        const float& z() const { return data[2]; }
    };
    
    struct Vector4f {
        alignas(16) float data[4];
        
        Vector4f() : data{0.0f, 0.0f, 0.0f, 0.0f} {}
        Vector4f(float x, float y, float z, float w) : data{x, y, z, w} {}
        
        float& x() { return data[0]; }
        float& y() { return data[1]; }
        float& z() { return data[2]; }
        float& w() { return data[3]; }
        
        const float& x() const { return data[0]; }
        const float& y() const { return data[1]; }
        const float& z() const { return data[2]; }
        const float& w() const { return data[3]; }
    };
    
    static Vector3f add(const Vector3f& a, const Vector3f& b);
    static Vector3f subtract(const Vector3f& a, const Vector3f& b);
    static Vector3f multiply(const Vector3f& a, float scalar);
    static float dot_product(const Vector3f& a, const Vector3f& b);
    static Vector3f cross_product(const Vector3f& a, const Vector3f& b);
    static float length(const Vector3f& v);
    static Vector3f normalize(const Vector3f& v);
    
    static void add_batch(const Vector3f* a, const Vector3f* b, Vector3f* result, size_t count);
    static void transform_batch(const Vector3f* vectors, const float* matrix4x4, Vector3f* result, size_t count);
    
    struct Matrix4f {
        alignas(64) float data[16];
        
        Matrix4f();
        static Matrix4f identity();
        static Matrix4f translation(float x, float y, float z);
        static Matrix4f rotation_x(float angle);
        static Matrix4f rotation_y(float angle);
        static Matrix4f rotation_z(float angle);
        static Matrix4f scale(float x, float y, float z);
    };
    
    static Matrix4f multiply_matrices(const Matrix4f& a, const Matrix4f& b);
    static Vector4f multiply_matrix_vector(const Matrix4f& m, const Vector4f& v);
};


class SIMDPerformanceProfiler {
private:
    struct ProfileData {
        std::string operation_name;
        uint64_t call_count;
        uint64_t total_time_ns;
        uint64_t min_time_ns;
        uint64_t max_time_ns;
        uint64_t elements_processed;
        
        ProfileData() : call_count(0), total_time_ns(0), min_time_ns(UINT64_MAX), 
                       max_time_ns(0), elements_processed(0) {}
    };
    
    std::unordered_map<std::string, ProfileData> profile_data_;
    mutable std::mutex profile_mutex_;
    
    static thread_local std::chrono::high_resolution_clock::time_point start_time_;

public:
    SIMDPerformanceProfiler();
    ~SIMDPerformanceProfiler();
    
    void start_operation(const std::string& operation_name);
    void end_operation(const std::string& operation_name, uint64_t elements_processed = 0);
    
    class ScopedProfiler {
    private:
        std::string operation_name_;
        uint64_t elements_;
        std::chrono::high_resolution_clock::time_point start_time_;
        SIMDPerformanceProfiler* profiler_;
        
    public:
        ScopedProfiler(const std::string& operation_name, uint64_t elements = 0);
        ~ScopedProfiler();
    };
    
    void print_performance_report() const;
    void print_top_operations(size_t count = 10) const;
    double get_operation_throughput(const std::string& operation_name) const;
    uint64_t get_operation_average_time_ns(const std::string& operation_name) const;
    
    void reset_all_statistics();
    void export_statistics_json(const std::string& filename) const;
    
    static SIMDPerformanceProfiler& get_instance();
};


namespace SIMDIntegration {
    void initialize_simd_engine();
    void shutdown_simd_engine();
    
    void register_simd_functions();
    bool try_simd_acceleration(const std::string& operation, const std::vector<Value>& args, Value& result);
    
    void enable_simd_profiling();
    void disable_simd_profiling();
    void print_simd_performance_report();
    
    void set_simd_optimization_level(int level);
    
    void detect_and_optimize_for_cpu();
    void print_cpu_capabilities();
}

#define SIMD_PROFILE_OPERATION(name, elements) \
    SIMDPerformanceProfiler::ScopedProfiler _prof(name, elements)

#define SIMD_PROFILE_SIMPLE(name) \
    SIMDPerformanceProfiler::ScopedProfiler _prof(name)

}
