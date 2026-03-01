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

// Unicode simple case folding for characters outside ASCII that fold to ASCII word chars.
// Per ECMAScript spec, /\w/ui matches chars whose CaseFold maps into [A-Za-z0-9_].
// Only two non-ASCII Unicode code points fold into ASCII letters:
//   U+017F LATIN SMALL LETTER LONG S  (UTF-8: C5 BF) → 's'
//   U+212A KELVIN SIGN                (UTF-8: E2 84 AA) → 'k'
static std::string apply_unicode_word_case_fold(const std::string& str) {
    std::string result;
    result.reserve(str.size());
    size_t i = 0;
    while (i < str.size()) {
        unsigned char c0 = static_cast<unsigned char>(str[i]);
        if (c0 == 0xC5 && i + 1 < str.size() &&
            static_cast<unsigned char>(str[i+1]) == 0xBF) {
            // U+017F → 's'
            result += 's';
            i += 2;
        } else if (c0 == 0xE2 && i + 2 < str.size() &&
                   static_cast<unsigned char>(str[i+1]) == 0x84 &&
                   static_cast<unsigned char>(str[i+2]) == 0xAA) {
            // U+212A → 'k'
            result += 'k';
            i += 3;
        } else {
            result += str[i];
            i++;
        }
    }
    return result;
}

RegExp::RegExp(const std::string& pattern, const std::string& flags)
    : pattern_(pattern), flags_(flags), global_(false), ignore_case_(false),
      multiline_(false), unicode_(false), sticky_(false), last_index_(0) {

    parse_flags(flags);

    try {
        // Apply Annex B transformations for non-unicode patterns
        std::string annex_b_pattern = unicode_ ? pattern_ : transform_annex_b(pattern_);
        std::string transformed_pattern = multiline_ ? transform_pattern_for_multiline(annex_b_pattern) : annex_b_pattern;
        regex_ = std::regex(transformed_pattern, get_regex_flags());
    } catch (const std::regex_error& e) {
        regex_ = std::regex("(?!)");
    }
}

bool RegExp::test(const std::string& str) {
    try {
        std::smatch match;
        std::string effective_str = (unicode_ && ignore_case_) ? apply_unicode_word_case_fold(str) : str;
        const std::string& s = effective_str;
        std::string::const_iterator start = s.begin();

        if (global_ && last_index_ > 0) {
            if (last_index_ >= static_cast<int>(s.length())) {
                last_index_ = 0;
                return false;
            }
            start = s.begin() + last_index_;
        }

        bool result = std::regex_search(start, s.end(), match, regex_);

        if (global_) {
            if (result) {
                size_t actual_position = (start - s.begin()) + match.position();
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
        std::string effective_str = (unicode_ && ignore_case_) ? apply_unicode_word_case_fold(str) : str;
        const std::string& s = effective_str;

        std::string::const_iterator start = s.begin();
        if (advances_index && last_index_ > 0 && last_index_ < s.length()) {
            start = s.begin() + last_index_;
        } else if (advances_index && last_index_ >= s.length()) {
            last_index_ = 0;
            return Value::null();
        }

        bool found = false;
        if (sticky_) {
            // Sticky: must match exactly at lastIndex (use regex_search with match_continuous)
            found = std::regex_search(start, s.cend(), match, regex_,
                                      std::regex_constants::match_continuous);
        } else {
            found = std::regex_search(start, s.cend(), match, regex_);
        }

        if (found) {
            size_t actual_position = (start - s.begin()) + match.position();
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

void RegExp::compile(const std::string& pattern, const std::string& flags) {
    pattern_ = pattern;
    flags_ = flags;
    global_ = false;
    ignore_case_ = false;
    multiline_ = false;
    unicode_ = false;
    sticky_ = false;
    last_index_ = 0;

    parse_flags(flags_);

    try {
        std::string annex_b_pattern = unicode_ ? pattern_ : transform_annex_b(pattern_);
        std::string transformed_pattern = multiline_ ? transform_pattern_for_multiline(annex_b_pattern) : annex_b_pattern;
        regex_ = std::regex(transformed_pattern, get_regex_flags());
    } catch (const std::regex_error& e) {
        regex_ = std::regex("(?!)");
    }
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

// ES6 Annex B: Transform legacy regex patterns to std::regex compatible form
std::string RegExp::transform_annex_b(const std::string& pattern) const {
    std::string result;
    bool in_char_class = false;

    auto is_shorthand_class = [](char c) -> bool {
        return c == 'w' || c == 'W' || c == 'd' || c == 'D' || c == 's' || c == 'S';
    };

    auto is_valid_escape = [](char c) -> bool {
        if (c == 'd' || c == 'D' || c == 'w' || c == 'W' || c == 's' || c == 'S') return true;
        if (c == 'b' || c == 'B') return true;
        if (c == 'n' || c == 'r' || c == 't' || c == 'f' || c == 'v') return true;
        if (c == '0') return true;
        if (c == 'x' || c == 'u' || c == 'c') return true;
        if (c >= '1' && c <= '9') return true;
        if (c == '.' || c == '*' || c == '+' || c == '?' || c == '(' || c == ')' ||
            c == '[' || c == ']' || c == '{' || c == '}' || c == '\\' ||
            c == '^' || c == '$' || c == '|' || c == '/' || c == '-') return true;
        return false;
    };

    // Count capture groups to distinguish backreferences from octals
    int num_groups = 0;
    {
        bool esc = false, in_cc = false;
        for (size_t j = 0; j < pattern.length(); ++j) {
            if (esc) { esc = false; continue; }
            if (pattern[j] == '\\') { esc = true; continue; }
            if (pattern[j] == '[') in_cc = true;
            else if (pattern[j] == ']') in_cc = false;
            else if (pattern[j] == '(' && !in_cc) {
                if (j + 1 < pattern.length() && pattern[j + 1] == '?') continue; // non-capturing
                num_groups++;
            }
        }
    }

    for (size_t i = 0; i < pattern.length(); ++i) {
        char ch = pattern[i];

        if (ch == '\\' && i + 1 < pattern.length()) {
            char next = pattern[i + 1];

            // Annex B: \N where N is 1-9 digits
            // If N <= num_groups, it's a backreference (pass through)
            // If N > num_groups, treat as octal escape
            if (next >= '1' && next <= '9') {
                // Parse the full decimal number
                size_t start = i + 1;
                size_t end = start;
                while (end < pattern.length() && pattern[end] >= '0' && pattern[end] <= '9') end++;
                int ref_num = 0;
                for (size_t k = start; k < end; k++) ref_num = ref_num * 10 + (pattern[k] - '0');

                if (ref_num <= num_groups) {
                    // Valid backreference - pass through
                    for (size_t k = i; k < end; k++) result += pattern[k];
                    i = end - 1;
                    continue;
                }
                // Invalid backreference - treat as octal
                end = start;
                int val = 0;
                while (end < pattern.length() && end < start + 3 && pattern[end] >= '0' && pattern[end] <= '7') {
                    int new_val = val * 8 + (pattern[end] - '0');
                    if (new_val > 255) break;
                    val = new_val;
                    end++;
                }
                if (val > 0 && val <= 255) {
                    char hex[8];
                    snprintf(hex, sizeof(hex), "\\x%02x", val);
                    result += hex;
                    i = end - 1;
                    continue;
                }
                // Fallback: pass through
                result += ch;
                continue;
            }

            // \0nn octal starting with 0
            if (next == '0' && i + 2 < pattern.length() && pattern[i + 2] >= '0' && pattern[i + 2] <= '7') {
                size_t start = i + 1;
                size_t end = start;
                int val = 0;
                while (end < pattern.length() && end < start + 3 && pattern[end] >= '0' && pattern[end] <= '7') {
                    int new_val = val * 8 + (pattern[end] - '0');
                    if (new_val > 255) break;
                    val = new_val;
                    end++;
                }
                char hex[8];
                snprintf(hex, sizeof(hex), "\\x%02x", val);
                result += hex;
                i = end - 1;
                continue;
            }

            // Invalid hex escape: \xN (less than 2 hex digits)
            if (next == 'x') {
                bool valid = (i + 3 < pattern.length());
                if (valid) {
                    for (int j = 0; j < 2; j++) {
                        if (i + 2 + j >= pattern.length()) { valid = false; break; }
                        char c = pattern[i + 2 + j];
                        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) { valid = false; break; }
                    }
                }
                if (!valid) {
                    result += 'x';
                    i++;
                    continue;
                }
                result += ch;
                continue;
            }

            // Invalid unicode escape: \uN (less than 4 hex digits)
            if (next == 'u') {
                if (i + 2 < pattern.length() && pattern[i + 2] != '{') {
                    bool valid = true;
                    for (int j = 0; j < 4; j++) {
                        if (i + 2 + j >= pattern.length()) { valid = false; break; }
                        char c = pattern[i + 2 + j];
                        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) { valid = false; break; }
                    }
                    if (!valid) {
                        result += 'u';
                        i++;
                        continue;
                    }
                }
                result += ch;
                continue;
            }

            // Invalid control escape: \cN where N is not a letter
            if (next == 'c') {
                if (i + 2 < pattern.length()) {
                    char ctrl = pattern[i + 2];
                    if (!((ctrl >= 'a' && ctrl <= 'z') || (ctrl >= 'A' && ctrl <= 'Z'))) {
                        result += "\\\\c";
                        i++;
                        continue;
                    }
                } else {
                    result += "\\\\c";
                    i++;
                    continue;
                }
            }

            // Shorthand class in char class: escape following hyphen to prevent invalid range
            if (in_char_class && is_shorthand_class(next)) {
                result += ch;
                result += next;
                i++;
                if (i + 1 < pattern.length() && pattern[i + 1] == '-' &&
                    i + 2 < pattern.length() && pattern[i + 2] != ']') {
                    result += "\\x2d";
                    i++;
                }
                continue;
            }

            // Unknown/invalid escape like \z, \a - treat as literal character
            if (!is_valid_escape(next)) {
                result += next;
                i++;
                continue;
            }

            // Normal escape - pass through
            result += ch;
            continue;
        }

        // Lone ] outside character class - treat as literal (check BEFORE updating state)
        if (ch == ']' && !in_char_class) {
            result += "\\]";
            continue;
        }

        // Track character class state
        if (ch == '[' && !in_char_class) {
            in_char_class = true;
        } else if (ch == ']' && in_char_class) {
            in_char_class = false;
        }

        // Incomplete quantifier: { not followed by valid quantifier syntax
        if (ch == '{' && !in_char_class) {
            size_t j = i + 1;
            bool has_digit = false;
            while (j < pattern.length() && pattern[j] >= '0' && pattern[j] <= '9') { has_digit = true; j++; }
            if (has_digit && j < pattern.length() && (pattern[j] == '}' || pattern[j] == ',')) {
                result += ch;
                continue;
            }
            result += "\\{";
            continue;
        }

        result += ch;
    }

    return result;
}

}
