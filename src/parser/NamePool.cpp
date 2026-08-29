/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/parser/NamePool.h"

#include <deque>
#include <string_view>
#include <unordered_map>

namespace Quanta {

namespace {

// A deque, not a vector: an entry's address has to stay put, both for the
// reference intern() hands back and for the views the index is keyed on.
struct Pool {
    std::deque<std::string> texts;
    std::unordered_map<std::string_view, uint32_t> index;
};

Pool& pool() {
    static thread_local Pool p = [] {
        Pool fresh;
        // Id zero is the empty name, which the parser uses for anonymous
        // things, so it costs no lookup.
        fresh.texts.emplace_back();
        fresh.index.emplace(std::string_view(fresh.texts.back()), 0u);
        return fresh;
    }();
    return p;
}

}  // namespace

uint32_t NamePool::intern(const std::string& text) {
    Pool& p = pool();
    auto it = p.index.find(std::string_view(text));
    if (it != p.index.end()) return it->second;
    const uint32_t id = static_cast<uint32_t>(p.texts.size());
    p.texts.push_back(text);
    p.index.emplace(std::string_view(p.texts.back()), id);
    return id;
}

const std::string& NamePool::text(uint32_t id) {
    Pool& p = pool();
    if (id >= p.texts.size()) return p.texts.front();
    return p.texts[id];
}

}  // namespace Quanta
