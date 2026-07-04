/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_GC_CELLKIND_H
#define QUANTA_GC_CELLKIND_H

#include <cstddef>
#include <cstdint>

namespace Quanta {

// Every GC-managed allocation is a cell of exactly one kind. A heap block
// holds cells of a single kind, so sweep/trace/destructor dispatch reads the
// kind from the block header instead of a per-cell header -- cells carry
// zero GC metadata of their own.
enum class CellKind : uint8_t {
    Object,   // Object and every subclass (Function, Promise, ...)
    String,
    Symbol,
    BigInt,
    kCount
};

constexpr size_t kNumCellKinds = static_cast<size_t>(CellKind::kCount);

// Heap segment tag, reserved for future use; every cell is Core today.
enum class HeapSegment : uint8_t {
    Core,
    Dom,
    Layout,
    Style,
    Network,
    Wasm
};

}

#endif
