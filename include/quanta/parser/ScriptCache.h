/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_PARSER_SCRIPT_CACHE_H
#define QUANTA_PARSER_SCRIPT_CACHE_H

#include "quanta/parser/ScriptUnit.h"
#include <cstdint>
#include <string_view>
#include <vector>

namespace Quanta {

// What the parse of a script learned about each of its function bodies (where it closes, what it names and what
// it needs from outside, whether it is strict), which is what a later parse of the same text needs to step over
// the bodies instead of reading them. The bodies are read back out of the source when something first runs them,
// as the engine already does for those it lets go of after a parse.
//
// A cache says nothing about the text it came from but its hash and length, and a parse that uses one does not
// look inside the bodies it skips: it is for text that was parsed successfully once, by this version of the
// engine, and is meant to be kept by whoever kept the text.
class ScriptCache {
public:
    struct Entry {
        uint32_t open;          // where the body's `{` is
        BodyScopeInfo info;
    };
    std::vector<Entry> entries;

    // The cache for a unit that has just been parsed from `source`.
    static std::vector<uint8_t> serialize(const ScriptUnit& unit, std::string_view source);
    // False if `bytes` is not a cache this engine wrote for exactly this `source`.
    static bool deserialize(const uint8_t* bytes, size_t size, std::string_view source, ScriptCache& out);
};

}

#endif
