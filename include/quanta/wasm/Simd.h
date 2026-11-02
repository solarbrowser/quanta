/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_WASM_SIMD_H
#define QUANTA_WASM_SIMD_H

#include <cstdint>

namespace Quanta::Wasm {

// How many words of immediates the instruction (the number after 0xFD) has in the code: the lane of an extract or replace, the 16 lanes of a shuffle.
uint32_t SimdImmediateWords(uint32_t sub);

// Runs a vector instruction that works on the stack alone: the operands are taken off and the result put on, and the new top is returned.
uint64_t* SimdExec(uint32_t sub, uint64_t* sp, const uint32_t* immediates);

}  // namespace Quanta::Wasm

#endif
