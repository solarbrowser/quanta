/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_WASM_DECODER_H
#define QUANTA_WASM_DECODER_H

#include <cstddef>
#include <cstdint>
#include <string>

#include "quanta/wasm/Module.h"

namespace Quanta::Wasm {

// Reads the binary format into `out` (which takes a copy of the bytes: the ranges of the module refer to them). What is wrong with
// the structure of the bytes is told in `error`; what is wrong with the meaning of the module is for Validate.
bool Decode(const uint8_t* data, size_t size, Module& out, std::string& error);

// The checks of the meaning of a decoded module: its types, its index spaces, the constant expressions and the bodies of its functions.
bool Validate(Module& module, std::string& error);

}  // namespace Quanta::Wasm

#endif
