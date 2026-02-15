/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_REGEXP_H
#define QUANTA_REGEXP_H

#include "quanta/core/runtime/Value.h"
#include <string>
#include <regex>

namespace Quanta {

/**
 * JavaScript RegExp object implementation
 */
class RegExp {
private:
    std::string pattern_;
    std::string flags_;
    std::regex regex_;
    bool global_;
    bool ignore_case_;
    bool multiline_;
    bool unicode_;
    bool sticky_;
    int last_index_;

public:
    RegExp(const std::string& pattern, const std::string& flags = "");
    
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
    int get_last_index() const { return last_index_; }
    void set_last_index(int index) { last_index_ = index; }
    
    std::string to_string() const;
    
private:
    void parse_flags(const std::string& flags);
    std::regex::flag_type get_regex_flags() const;
    std::string transform_pattern_for_multiline(const std::string& pattern) const;
    std::string transform_annex_b(const std::string& pattern) const;
};

}

#endif
