/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

// The one translation unit that points C++ allocation at mimalloc. The header
// below defines every form of operator new and operator delete, so it may be
// included exactly once in the program -- which is the only reason this file
// exists.
//
// The C library's own malloc is deliberately left alone (mimalloc is built
// without MI_MALLOC_OVERRIDE). Everything the engine allocates for itself goes
// through operator new -- cells, butterflies, the parser's containers, every
// std:: type -- so redirecting that is nearly all of it, and it costs no
// symbol interposition, no redirect DLL on Windows, and no risk of a pointer
// crossing between two allocators.
//
// Fiber stacks are the one thing that bypasses both: they are mapped straight
// from the OS, for the reasons in FiberStackPool.cpp.
//
// mimalloc itself is the submodule under third_party, built as the single
// object its own src/static.c is there to produce.
//
// This file has to be named on the link line, not left to the static archive:
// an archive member is pulled in only to resolve an undefined symbol, and
// operator new is never undefined because libstdc++ already defines it. Left
// to -lquanta it is dropped without a word and the program keeps the standard
// allocator, which measures as no change at all.
#include <mimalloc-new-delete.h>

// Transparent huge pages off for the allocator's own memory. With them on, the
// kernel backs mimalloc's arena with 2MB pages that stay fully resident however
// little of each is in use: measured with THP=always, 32MB of a 99MB mimalloc
// mapping was huge-page backed at a moment when the live allocations were 62MB.
// Turning it off lowered peak RSS on every benchmark for +-3% time: code.js
// 211 -> 199MB, typescript.js 111 -> 98MB, markdown.js 31 -> 18MB, vdom.js
// 27.5 -> 19MB. Only the default is set, so MIMALLOC_ALLOW_THP still wins.
// Priority 101 so it runs before any allocation that could fix the option.
__attribute__((constructor(101))) static void quanta_mimalloc_defaults() {
    mi_option_set_default(mi_option_allow_thp, 0);
}
