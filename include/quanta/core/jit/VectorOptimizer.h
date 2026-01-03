#pragma once

#include "quanta/parser/AST.h"
#include <immintrin.h>

namespace Quanta {

class VectorOptimizer {
public:
    static bool can_vectorize(ForStatement* loop);
    static void vectorize_loop(ForStatement* loop);

    static bool is_simple_array_loop(ForStatement* loop);
    static bool has_dependencies(ForStatement* loop);

private:
    static void emit_avx2_add_loop(int start, int end);
    static void emit_sse_mul_loop(int start, int end);
};

extern "C" {
    void jit_vector_add_f64(double* dest, const double* src1, const double* src2, size_t count);
    void jit_vector_mul_f64(double* dest, const double* src1, const double* src2, size_t count);
    void jit_vector_sum_f64(const double* arr, size_t count, double* result);
}

}
