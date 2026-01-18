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

class StringTable {
private:
    std::unordered_set<std::string> interned_strings_;

public:
    StringTable() = default;
    ~StringTable() = default;

    StringTable(const StringTable&) = delete;
    StringTable& operator=(const StringTable&) = delete;

    
    const char* intern(std::string_view str) {
        auto [it, inserted] = interned_strings_.emplace(str);
        return it->c_str();
    }


    bool contains(std::string_view str) const {
        return interned_strings_.find(std::string(str)) != interned_strings_.end();
    }


    size_t size() const { return interned_strings_.size(); }


    void clear() {
        interned_strings_.clear();
    }
};

} // namespace Quanta
