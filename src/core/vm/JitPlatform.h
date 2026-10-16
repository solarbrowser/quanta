/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_VM_JITPLATFORM_H
#define QUANTA_VM_JITPLATFORM_H

// The only operating-system dependent part of the baseline JIT: where generated
// code lives. Declared here, with nothing from the engine in it, and defined in
// JitPlatform.cpp, which is the one place that includes <windows.h>: that header
// defines macros (IN, OUT, ERROR, DELETE, ...) that collide with identifiers
// throughout the engine's own headers, so it must never reach the translation
// unit that holds the interpreter.
//
// The calling convention is the other platform dependency, and it is not here: it
// belongs to the code generator (Jit.inc, namespace abi), because the two x86-64
// ABIs differ in which registers carry arguments, how much stack a call must
// reserve and which registers a function must preserve, and all of that is
// decided where the instructions are emitted.

#include <cstddef>

namespace Quanta::VM::jit::platform {

size_t page_size();

// Maps `size` bytes (a multiple of page_size()) as writable memory of their own,
// or returns null. Windows commits as it reserves; a mapping is never committed
// without being used, so there is no separate reserve step to manage.
//
// Apple Silicon does not let ordinary pages become executable once written; code
// has to live in a MAP_JIT mapping, which is writable or executable per thread
// depending on pthread_jit_write_protect_np. The mapping is returned with writing
// enabled for this thread and make_executable() turns it back, so the pair works
// the same as the mprotect form elsewhere.
void* map_writable(size_t size);

// Flips a finished mapping to read+execute. Code is written once and is never
// writable while it can run (W^X): a bug that corrupts a heap object cannot be
// turned into new code, and a system that forbids writable+executable pages works
// unchanged. An instruction cache that is not coherent with stores (AArch64, and
// Windows by its documented contract) is told about the new code here, after the
// last write and before anything can run it.
bool make_executable(void* mem, size_t size);

void unmap(void* mem, size_t size);

}  // namespace Quanta::VM::jit::platform

#endif
