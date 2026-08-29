/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_PARSER_NAME_POOL_H
#define QUANTA_PARSER_NAME_POOL_H

#include <cstdint>
#include <string>

namespace Quanta {

// The names a parse mentions, kept once each. A real script says the same few
// thousand names hundreds of thousands of times -- a bundle measured at half a
// million identifier nodes against twenty-seven thousand distinct names, whose
// text together comes to well under a megabyte while the copies came to
// several. An identifier node holds the number instead of the text, which also
// takes it down a size class.
//
// Per thread, and never shrinks: names are bounded by the code that has been
// read, and an identifier may be read long after the parse that made it.
class NamePool {
public:
    static uint32_t intern(const std::string& text);
    // Stable for the life of the thread: the storage never moves an entry.
    static const std::string& text(uint32_t id);
};

}  // namespace Quanta

#endif
