/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_AST_ARENA_H
#define QUANTA_AST_ARENA_H

#include <cstddef>

namespace Quanta {

// Where AST nodes live.
//
// A parse makes hundreds of thousands of them, they are small and all of one
// of a handful of sizes, and the tree dies all at once -- when the body that
// owns it is dropped, or when the parse that built it is abandoned. Through
// the general allocator that pattern costs twice over. Each node pays a header
// and rounding it does not need, and the nodes end up interleaved with
// everything else the parse allocates, so when the tree goes the pages it
// occupied stay resident: measured on a script of nine thousand functions,
// releasing everything the parse had freed gave back one megabyte of a
// hundred, because a few live nodes were holding every page down.
//
// So: chunks, one size class each, aligned so a node's chunk is found by
// masking its address. A freed node goes on its chunk's free list, and a chunk
// whose last node is freed is handed back to the system whole. What the tree
// stops using, the process stops holding.
//
// Thread-local, on the same grounds ScriptUnit's reference count is not atomic:
// a unit and the executables that borrow from it never leave the thread that
// parsed them, so a node is always freed on the thread that made it.
class AstArena {
public:
    // Sizes past this go to the general allocator -- there are a handful of
    // such nodes and giving each its own class would waste more than it saves.
    static constexpr size_t kMaxNodeSize = 192;

    static void* take(size_t bytes);
    static void give(void* p) noexcept;

    struct Stats {
        size_t chunks = 0;
        size_t live_nodes = 0;
        size_t bytes_reserved = 0;
    };
    static Stats stats();
};

}  // namespace Quanta

#endif
