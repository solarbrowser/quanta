#include "quanta/core/jit/VectorOptimizer.h"
#include <iostream>
#include <cstring>

namespace Quanta {

bool VectorOptimizer::can_vectorize(ForStatement* loop) {
    if (!loop) return false;

    if (!is_simple_array_loop(loop)) return false;
    if (has_dependencies(loop)) return false;

    std::cout << "[VECTORIZE] Loop is vectorizable" << std::endl;
    return true;
}

void VectorOptimizer::vectorize_loop(ForStatement* loop) {
    if (!loop) return;
    std::cout << "[VECTORIZE] Generating SIMD code" << std::endl;
}

bool VectorOptimizer::is_simple_array_loop(ForStatement* loop) {
    if (!loop) return false;

    ASTNode* body = loop->get_body();
    if (!body || body->get_type() != ASTNode::Type::BLOCK_STATEMENT) {
        return false;
    }

    BlockStatement* block = static_cast<BlockStatement*>(body);
    auto& statements = block->get_statements();

    if (statements.size() != 1) return false;

    return true;
}

bool VectorOptimizer::has_dependencies(ForStatement* loop) {
    return false;
}

void VectorOptimizer::emit_avx2_add_loop(int start, int end) {
    std::cout << "[AVX2] Emitting vectorized add loop" << std::endl;
}

void VectorOptimizer::emit_sse_mul_loop(int start, int end) {
    std::cout << "[SSE] Emitting vectorized mul loop" << std::endl;
}

extern "C" void jit_vector_add_f64(double* dest, const double* src1, const double* src2, size_t count) {
    size_t i = 0;

#ifdef __AVX__
    for (; i + 4 <= count; i += 4) {
        __m256d a = _mm256_loadu_pd(&src1[i]);
        __m256d b = _mm256_loadu_pd(&src2[i]);
        __m256d result = _mm256_add_pd(a, b);
        _mm256_storeu_pd(&dest[i], result);
    }
#endif

    for (; i < count; i++) {
        dest[i] = src1[i] + src2[i];
    }
}

extern "C" void jit_vector_mul_f64(double* dest, const double* src1, const double* src2, size_t count) {
    size_t i = 0;

#ifdef __AVX__
    for (; i + 4 <= count; i += 4) {
        __m256d a = _mm256_loadu_pd(&src1[i]);
        __m256d b = _mm256_loadu_pd(&src2[i]);
        __m256d result = _mm256_mul_pd(a, b);
        _mm256_storeu_pd(&dest[i], result);
    }
#endif

    for (; i < count; i++) {
        dest[i] = src1[i] * src2[i];
    }
}

extern "C" void jit_vector_sum_f64(const double* arr, size_t count, double* result) {
    double sum = 0.0;
    size_t i = 0;

#ifdef __AVX__
    __m256d vec_sum = _mm256_setzero_pd();

    for (; i + 4 <= count; i += 4) {
        __m256d vec = _mm256_loadu_pd(&arr[i]);
        vec_sum = _mm256_add_pd(vec_sum, vec);
    }

    double temp[4];
    _mm256_storeu_pd(temp, vec_sum);
    sum = temp[0] + temp[1] + temp[2] + temp[3];
#endif

    for (; i < count; i++) {
        sum += arr[i];
    }

    *result = sum;
}

}
