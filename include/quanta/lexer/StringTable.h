/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <string>
#include <unordered_set>
#include <string_view>

namespace Quanta {

/**
 * String interning table - stores unique strings only once
 * Returns const char* pointers to interned strings
 *
 * Benefits:
 * - O(1) string comparison (pointer equality)
 * - Reduced memory usage (no duplicate strings)
 * - Cache-friendly (strings grouped together)
 */
class StringTable {
private:
    std::unordered_set<std::string> interned_strings_;

public:
    StringTable() = default;
    ~StringTable() = default;

    // Non-copyable (singleton-like usage)
    StringTable(const StringTable&) = delete;
    StringTable& operator=(const StringTable&) = delete;

    /**
     * Intern a string - returns pointer to permanent storage
     * Multiple calls with same string return same pointer
     */
    const char* intern(std::string_view str) {
        auto [it, inserted] = interned_strings_.emplace(str);
        return it->c_str();
    }

    /**
     * Check if string is already interned
     */
    bool contains(std::string_view str) const {
        return interned_strings_.find(std::string(str)) != interned_strings_.end();
    }

    /**
     * Get statistics
     */
    size_t size() const { return interned_strings_.size(); }

    /**
     * Clear all interned strings (use with caution!)
     */
    void clear() {
        interned_strings_.clear();
    }
};

} // namespace Quanta
