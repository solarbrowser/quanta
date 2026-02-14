/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/runtime/RegExp.h"
#include "quanta/core/runtime/Object.h"
#include <iostream>
#include <sstream>

namespace Quanta {

RegExp::RegExp(const std::string& pattern, const std::string& flags)
    : pattern_(pattern), flags_(flags), global_(false), ignore_case_(false),
      multiline_(false), unicode_(false), sticky_(false), last_index_(0) {

    parse_flags(flags);

    try {
        std::string transformed_pattern = multiline_ ? transform_pattern_for_multiline(pattern_) : pattern_;
        regex_ = std::regex(transformed_pattern, get_regex_flags());
    } catch (const std::regex_error& e) {
        regex_ = std::regex("(?!)");
    }
}

bool RegExp::test(const std::string& str) {
    try {
        std::smatch match;
        std::string::const_iterator start = str.begin();

        if (global_ && last_index_ > 0) {
            if (last_index_ >= static_cast<int>(str.length())) {
                last_index_ = 0;
                return false;
            }
            start = str.begin() + last_index_;
        }

        bool result = std::regex_search(start, str.end(), match, regex_);

        if (global_) {
            if (result) {
                size_t actual_position = (start - str.begin()) + match.position();
                size_t match_len = match.length();

                if (multiline_ && match_len > 0 && match[0].str()[0] == '\n') {
                    actual_position++;
                    match_len--;
                }

                last_index_ = actual_position + match_len;
            } else {
                last_index_ = 0;
            }
        }

        return result;
    } catch (const std::regex_error& e) {
        if (global_) {
            last_index_ = 0;
        }
        return false;
    }
}

Value RegExp::exec(const std::string& str) {
    try {
        std::smatch match;
        bool advances_index = global_ || sticky_;

        std::string::const_iterator start = str.begin();
        if (advances_index && last_index_ > 0 && last_index_ < str.length()) {
            start = str.begin() + last_index_;
        } else if (advances_index && last_index_ >= str.length()) {
            last_index_ = 0;
            return Value::null();
        }

        bool found = false;
        if (sticky_) {
            // Sticky: must match exactly at lastIndex (use regex_search with match_continuous)
            found = std::regex_search(start, str.cend(), match, regex_,
                                      std::regex_constants::match_continuous);
        } else {
            found = std::regex_search(start, str.cend(), match, regex_);
        }

        if (found) {
            size_t actual_position = (start - str.begin()) + match.position();
            std::string matched_str = match[0].str();

            if (multiline_ && !matched_str.empty() && matched_str[0] == '\n') {
                actual_position++;
                matched_str = matched_str.substr(1);
            }

            if (advances_index) {
                last_index_ = actual_position + matched_str.length();
            }

            auto result = new Object();
            result->set_property("0", Value(matched_str));
            result->set_property("index", Value(static_cast<double>(actual_position)));
            result->set_property("input", Value(str));
            result->set_property("length", Value(static_cast<double>(match.size())));

            for (size_t i = 1; i < match.size(); ++i) {
                if (match[i].matched) {
                    result->set_property(std::to_string(i), Value(match[i].str()));
                } else {
                    result->set_property(std::to_string(i), Value());
                }
            }

            return Value(result);
        } else if (advances_index) {
            last_index_ = 0;
        }
    } catch (const std::regex_error& e) {
    }

    return Value::null();
}

std::string RegExp::to_string() const {
    return "/" + pattern_ + "/" + flags_;
}

void RegExp::parse_flags(const std::string& flags) {
    for (char flag : flags) {
        switch (flag) {
            case 'g':
                global_ = true;
                break;
            case 'i':
                ignore_case_ = true;
                break;
            case 'm':
                multiline_ = true;
                break;
            case 'u':
                unicode_ = true;
                break;
            case 'y':
                sticky_ = true;
                break;
            default:
                break;
        }
    }
}

std::regex::flag_type RegExp::get_regex_flags() const {
    std::regex::flag_type flags = std::regex::ECMAScript;

    if (ignore_case_) {
        flags |= std::regex::icase;
    }

    return flags;
}

std::string RegExp::transform_pattern_for_multiline(const std::string& pattern) const {
    std::string result;
    bool in_char_class = false;
    bool escaped = false;

    for (size_t i = 0; i < pattern.length(); ++i) {
        char ch = pattern[i];

        if (escaped) {
            result += ch;
            escaped = false;
            continue;
        }

        if (ch == '\\') {
            result += ch;
            escaped = true;
            continue;
        }

        if (ch == '[') {
            in_char_class = true;
            result += ch;
            continue;
        }

        if (ch == ']') {
            in_char_class = false;
            result += ch;
            continue;
        }

        if (!in_char_class && ch == '^') {
            result += "(?:(?:^)|(?:\\n))";
            continue;
        }

        if (!in_char_class && ch == '$') {
            result += "(?:$|(?=\\n))";
            continue;
        }

        result += ch;
    }

    return result;
}

}
