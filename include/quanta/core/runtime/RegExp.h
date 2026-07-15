/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_REGEXP_H
#define QUANTA_REGEXP_H

#include "quanta/core/runtime/Value.h"
#include <string>
#include <vector>
#include <cstdint>
#include <memory>

namespace Quanta {

// WTF-8 <-> UTF-16 converters shared by the regexp engine and builtins: JS
// string indices are UTF-16 units and may fall inside a surrogate pair.
std::u16string wtf8_to_utf16(const std::string& s);
std::string utf16_to_wtf8(const char16_t* p, size_t len);

class RegexBacktrackEngine;

class RegExp {
private:
    std::string pattern_;
    std::string flags_;
    void* code_;  // pcre2_code* -- raw view; code_owner_ keeps it alive
    std::shared_ptr<void> code_owner_;  // owns code_ (frees it via pcre2_code_free);
                                         // shared across every RegExp with the same
                                         // (pattern,flags) via the compiled-pattern cache
    bool global_;
    bool ignore_case_;
    bool multiline_;
    bool unicode_;
    bool sticky_;
    bool dotall_;
    bool unicode_sets_;
    bool has_indices_;
    int last_index_;
    // Named capture groups in source order: (original JS name, capture numbers).
    // PCRE2 restricts group names to ASCII \w, so JS names (Unicode, $, long or
    // duplicate names) are renamed to synthetic names before compilation and
    // resolved back through this table.
    std::vector<std::pair<std::string, std::vector<uint32_t>>> named_groups_;
    // Non-null when PCRE2's lookbehind is known to get this pattern wrong; test()/exec()
    // use it instead. shared_ptr (not unique_ptr): exec() is const/reentrant, so this is
    // safely shared across every RegExp with the same (pattern,flags) via the cache too.
    std::shared_ptr<RegexBacktrackEngine> backtrack_engine_;

public:
    RegExp(const std::string& pattern, const std::string& flags = "");
    ~RegExp();
    RegExp(const RegExp&) = delete;
    RegExp& operator=(const RegExp&) = delete;

    bool test(const std::string& str);
    Value exec(const std::string& str);
    void compile(const std::string& pattern, const std::string& flags = "");

    std::string get_source() const { return pattern_; }
    std::string get_flags() const { return flags_; }
    bool get_global() const { return global_; }
    bool get_ignore_case() const { return ignore_case_; }
    bool get_multiline() const { return multiline_; }
    bool get_unicode() const { return unicode_; }
    bool get_sticky() const { return sticky_; }
    bool get_dotall() const { return dotall_; }
    bool get_unicode_sets() const { return unicode_sets_; }
    bool get_has_indices() const { return has_indices_; }
    int get_last_index() const { return last_index_; }
    void set_last_index(int index) { last_index_ = index; }

    std::string to_string() const;

    static bool is_valid_unicode_pattern(const std::string& pattern, const std::string& flags);

private:
    void parse_flags(const std::string& flags);
    void do_compile();
    std::string preprocess_pattern(const std::string& pattern) const;
    std::string rename_named_groups(const std::string& pattern);
};

}

#endif
