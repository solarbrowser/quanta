/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_STRING_H
#define QUANTA_STRING_H

#include <string>
#include <memory>

namespace Quanta {

/**
 * JavaScript string implementation
 * Features:
 * - String interning for common strings
 * - Copy-on-write semantics
 * - UTF-8 support
 */
class String {
private:
    std::shared_ptr<std::string> data_;
    size_t hash_;
    bool interned_;

public:
    String();
    explicit String(const std::string& str);
    explicit String(const char* str);
    String(const String& other) = default;
    String(String&& other) noexcept = default;
    
    String& operator=(const String& other) = default;
    String& operator=(String&& other) noexcept = default;
    
    const std::string& str() const { return *data_; }
    const char* c_str() const { return data_->c_str(); }
    size_t length() const { return data_->length(); }
    size_t size() const { return data_->size(); }
    bool empty() const { return data_->empty(); }
    
    size_t hash() const { return hash_; }
    
    bool operator==(const String& other) const;
    bool operator!=(const String& other) const { return !(*this == other); }
    bool operator<(const String& other) const { return str() < other.str(); }
    
    String concat(const String& other) const;
    String substring(size_t start, size_t length = std::string::npos) const;
    
    static String intern(const std::string& str);

private:
    void calculate_hash();
};

}

#endif
