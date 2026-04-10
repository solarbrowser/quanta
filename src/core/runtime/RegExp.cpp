/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#define PCRE2_CODE_UNIT_WIDTH 8
#include "quanta/core/runtime/RegExp.h"
#include "quanta/core/runtime/Object.h"
#include <pcre2.h>
#include <vector>
#include <cstring>

namespace Quanta {

RegExp::RegExp(const std::string& pattern, const std::string& flags)
    : pattern_(pattern), flags_(flags), code_(nullptr),
      global_(false), ignore_case_(false), multiline_(false),
      unicode_(false), sticky_(false), dotall_(false), last_index_(0) {
    parse_flags(flags);
    do_compile();
}

RegExp::~RegExp() {
    if (code_) {
        pcre2_code_free(static_cast<pcre2_code*>(code_));
        code_ = nullptr;
    }
}

void RegExp::parse_flags(const std::string& flags) {
    for (char flag : flags) {
        switch (flag) {
            case 'g': global_ = true; break;
            case 'i': ignore_case_ = true; break;
            case 'm': multiline_ = true; break;
            case 'u': unicode_ = true; break;
            case 'y': sticky_ = true; break;
            case 's': dotall_ = true; break;
            default: break;
        }
    }
}

// For non-unicode mode: strip unknown escapes (Annex B identity escapes).
// PCRE2_EXTRA_BAD_ESCAPE_IS_LITERAL handles most cases, but lone ] needs fixing.
// Convert \uXXXX and \u{XXXXX} to \x{XXXX} for PCRE2, in both unicode and non-unicode mode.
// Also convert \uD800\uDC00 surrogate pairs to the combined codepoint.
static std::string convert_unicode_escapes(const std::string& pat) {
    std::string result;
    result.reserve(pat.size());
    size_t i = 0;
    auto is_hex = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    };
    auto hex_val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return c - 'A' + 10;
    };
    while (i < pat.size()) {
        if (pat[i] == '\\' && i + 1 < pat.size() && pat[i+1] == 'u') {
            if (i + 2 < pat.size() && pat[i+2] == '{') {
                // \u{XXXXX}
                size_t end = pat.find('}', i+3);
                if (end != std::string::npos) {
                    result += "\\x{";
                    result += pat.substr(i+3, end - (i+3));
                    result += '}';
                    i = end + 1;
                    continue;
                }
            } else if (i + 5 < pat.size() &&
                       is_hex(pat[i+2]) && is_hex(pat[i+3]) && is_hex(pat[i+4]) && is_hex(pat[i+5])) {
                int cp = (hex_val(pat[i+2]) << 12) | (hex_val(pat[i+3]) << 8) |
                         (hex_val(pat[i+4]) << 4) | hex_val(pat[i+5]);
                // Check for surrogate pair
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 11 < pat.size() &&
                    pat[i+6] == '\\' && pat[i+7] == 'u' &&
                    is_hex(pat[i+8]) && is_hex(pat[i+9]) && is_hex(pat[i+10]) && is_hex(pat[i+11])) {
                    int lo = (hex_val(pat[i+8]) << 12) | (hex_val(pat[i+9]) << 8) |
                             (hex_val(pat[i+10]) << 4) | hex_val(pat[i+11]);
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        int full = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        char buf[16];
                        snprintf(buf, sizeof(buf), "\\x{%X}", full);
                        result += buf;
                        i += 12;
                        continue;
                    }
                }
                char buf[12];
                snprintf(buf, sizeof(buf), "\\x{%04X}", cp);
                result += buf;
                i += 6;
                continue;
            }
        }
        result += pat[i++];
    }
    return result;
}

static bool is_hex_ch(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// Find groups that are "optional" (may not capture): groups followed by ?/* quantifier,
// or groups defined inside a lookahead/lookbehind assertion.
// ECMAScript spec: backreference to an undefined group always succeeds (matches empty string).
// PCRE2 doesn't support this, so we replace \N for optional groups with (?:\N|).
static std::string fix_optional_backrefs(const std::string& pat) {
    int n_groups = 0;
    std::vector<bool> optional_group;

    // Pass 1: find which groups are optional
    {
        struct StackEntry { int group_num; bool in_lookahead; };
        std::vector<StackEntry> stack;
        int lookahead_depth = 0;
        bool esc = false, in_cc = false;

        for (size_t i = 0; i < pat.size(); i++) {
            if (esc) { esc = false; continue; }
            if (pat[i] == '\\') { esc = true; continue; }
            if (pat[i] == '[' && !in_cc) { in_cc = true; continue; }
            if (pat[i] == ']' && in_cc) { in_cc = false; continue; }
            if (in_cc) continue;

            if (pat[i] == '(') {
                bool is_lookahead = false;
                bool is_capture = true;
                if (i+1 < pat.size() && pat[i+1] == '?') {
                    if (i+2 < pat.size() && (pat[i+2] == '=' || pat[i+2] == '!')) {
                        is_lookahead = true; is_capture = false;
                    } else if (i+3 < pat.size() && pat[i+2] == '<' &&
                               (pat[i+3] == '=' || pat[i+3] == '!')) {
                        is_lookahead = true; is_capture = false;
                    } else {
                        is_capture = false; // non-capturing group (?:...) etc.
                    }
                }
                if (is_lookahead) lookahead_depth++;
                int gnum = -1;
                if (is_capture) {
                    n_groups++;
                    while ((int)optional_group.size() < n_groups) optional_group.push_back(false);
                    if (lookahead_depth > 0) optional_group[n_groups-1] = true;
                    gnum = n_groups;
                }
                stack.push_back({gnum, is_lookahead});
            } else if (pat[i] == ')') {
                if (!stack.empty()) {
                    auto top = stack.back();
                    stack.pop_back();
                    if (top.in_lookahead) lookahead_depth--;
                    // Check quantifier after ')'
                    if (top.group_num > 0 && i+1 < pat.size()) {
                        char q = pat[i+1];
                        if (q == '?' || q == '*') optional_group[top.group_num-1] = true;
                        else if (q == '{' && i+2 < pat.size() && pat[i+2] == '0')
                            optional_group[top.group_num-1] = true;
                    }
                }
            }
        }
    }

    // Check if any backrefs to optional groups exist
    bool any = false;
    {
        bool esc = false;
        for (size_t i = 0; i < pat.size() && !any; i++) {
            if (esc) {
                if (pat[i] >= '1' && pat[i] <= '9') {
                    int n = pat[i] - '0';
                    if (n <= (int)optional_group.size() && optional_group[n-1]) any = true;
                }
                esc = false; continue;
            }
            if (pat[i] == '\\') esc = true;
        }
    }
    if (!any) return pat;

    // Pass 2: replace \N for optional groups with (?:\N|)
    std::string result;
    result.reserve(pat.size() * 2);
    bool esc = false;
    for (size_t i = 0; i < pat.size(); i++) {
        if (esc) {
            if (pat[i] >= '1' && pat[i] <= '9') {
                int n = pat[i] - '0';
                if (n <= (int)optional_group.size() && optional_group[n-1]) {
                    result.pop_back(); // remove the backslash 
                    result += "(?:\\";
                    result += pat[i];
                    result += "|)";
                    esc = false; continue;
                }
            }
            esc = false;
            result += pat[i];
            continue;
        }
        if (pat[i] == '\\') { esc = true; result += pat[i]; continue; }
        result += pat[i];
    }
    return result;
}

std::string RegExp::preprocess_pattern(const std::string& pat) const {
    std::string result;
    result.reserve(pat.size() * 2);
    bool in_char_class = false;
    size_t i = 0;
    while (i < pat.size()) {
        char ch = pat[i];

        if (ch == '\\' && i + 1 < pat.size()) {
            char next = pat[i+1];

            // \x with < 2 hex digits → literal 'x' + next chars (Annex B)
            if (next == 'x') {
                bool valid = (i+3 < pat.size() && is_hex_ch(pat[i+2]) && is_hex_ch(pat[i+3]));
                if (!valid) {
                    result += 'x';
                    i += 2;
                    continue;
                }
            }

            // \c followed by non-letter → literal backslash + c + char (Annex B)
            if (next == 'c' && i+2 < pat.size()) {
                char ctrl = pat[i+2];
                if (!((ctrl >= 'a' && ctrl <= 'z') || (ctrl >= 'A' && ctrl <= 'Z'))) {
                    result += "\\\\c";
                    i += 2;
                    continue;
                }
            }

            // Shorthand class (\w, \d, \s etc.) followed by '-' in char class → escape the hyphen
            if (in_char_class &&
                (next == 'w' || next == 'W' || next == 'd' || next == 'D' || next == 's' || next == 'S') &&
                i+2 < pat.size() && pat[i+2] == '-' && i+3 < pat.size() && pat[i+3] != ']') {
                result += ch; result += next;
                result += "\\-";
                i += 3;
                continue;
            }

            // Annex B identity escapes: unknown escape chars become their literal char.
            // But some letters are valid PCRE2 sequences with different semantics (e.g. \z, \Z, \A, \G).
            // ECMAScript only allows specific escape sequences; unknown ones → literal.
            static const char* js_valid_escapes = "dDwWsSnrtfvbBxu0123456789().[]{}\\^$|*+?/";
            bool is_valid = false;
            if (next == 'k' || next == 'p' || next == 'P') {
                is_valid = true; // named backrefs, unicode properties
            } else {
                for (const char* p = js_valid_escapes; *p; ++p) {
                    if (next == *p) { is_valid = true; break; }
                }
            }
            if (!is_valid) {
                // Unknown escape: output literal char (not the backslash)
                result += next;
                i += 2;
                continue;
            }

            result += ch;
            result += next;
            i += 2;
            continue;
        }

        if (ch == ']' && !in_char_class) {
            result += "\\]";
            i++;
            continue;
        }
        if (ch == '[') in_char_class = true;
        else if (ch == ']') in_char_class = false;
        result += ch;
        i++;
    }
    return result;
}

static std::string expand_gc_aliases(const std::string& pattern) {
    static const std::pair<const char*, const char*> aliases[] = {
        {"Other_Symbol", "So"}, {"Uppercase_Letter", "Lu"}, {"Lowercase_Letter", "Ll"},
        {"Titlecase_Letter", "Lt"}, {"Modifier_Letter", "Lm"}, {"Other_Letter", "Lo"},
        {"Nonspacing_Mark", "Mn"}, {"Spacing_Mark", "Mc"}, {"Enclosing_Mark", "Me"},
        {"Decimal_Number", "Nd"}, {"Letter_Number", "Nl"}, {"Other_Number", "No"},
        {"Connector_Punctuation", "Pc"}, {"Dash_Punctuation", "Pd"},
        {"Open_Punctuation", "Ps"}, {"Close_Punctuation", "Pe"},
        {"Initial_Punctuation", "Pi"}, {"Final_Punctuation", "Pf"},
        {"Other_Punctuation", "Po"}, {"Math_Symbol", "Sm"}, {"Currency_Symbol", "Sc"},
        {"Modifier_Symbol", "Sk"}, {"Space_Separator", "Zs"}, {"Line_Separator", "Zl"},
        {"Paragraph_Separator", "Zp"}, {"Control", "Cc"}, {"Format", "Cf"},
        {"Surrogate", "Cs"}, {"Private_Use", "Co"}, {"Unassigned", "Cn"},
    };
    std::string result;
    result.reserve(pattern.size());
    size_t i = 0;
    while (i < pattern.size()) {
        if (pattern[i] == '\\' && i+1 < pattern.size() &&
            (pattern[i+1] == 'p' || pattern[i+1] == 'P') &&
            i+2 < pattern.size() && pattern[i+2] == '{') {
            size_t end = pattern.find('}', i+3);
            if (end != std::string::npos) {
                std::string prop = pattern.substr(i+3, end - (i+3));
                bool replaced = false;
                for (auto& a : aliases) {
                    if (prop == a.first) {
                        result += pattern[i]; result += pattern[i+1];
                        result += '{'; result += a.second; result += '}';
                        i = end + 1; replaced = true; break;
                    }
                }
                if (!replaced) { result += pattern[i++]; }
            } else { result += pattern[i++]; }
        } else { result += pattern[i++]; }
    }
    return result;
}

void RegExp::do_compile() {
    if (code_) {
        pcre2_code_free(static_cast<pcre2_code*>(code_));
        code_ = nullptr;
    }

    std::string pat = unicode_
        ? fix_optional_backrefs(convert_unicode_escapes(expand_gc_aliases(pattern_)))
        : fix_optional_backrefs(convert_unicode_escapes(preprocess_pattern(pattern_)));

    uint32_t options = PCRE2_UTF;
    if (unicode_)    options |= PCRE2_UCP;
    if (ignore_case_) options |= PCRE2_CASELESS;
    if (multiline_)  options |= PCRE2_MULTILINE;
    if (dotall_)     options |= PCRE2_DOTALL;

    int errcode = 0;
    PCRE2_SIZE erroffset = 0;

    pcre2_compile_context* cctx = nullptr;
    if (!unicode_) {
        cctx = pcre2_compile_context_create(nullptr);
        if (cctx)
            pcre2_set_compile_extra_options(cctx, PCRE2_EXTRA_BAD_ESCAPE_IS_LITERAL);
    }

    pcre2_code* re = pcre2_compile(
        reinterpret_cast<PCRE2_SPTR>(pat.c_str()),
        PCRE2_ZERO_TERMINATED,
        options,
        &errcode,
        &erroffset,
        cctx
    );

    if (cctx) pcre2_compile_context_free(cctx);

    if (!re) {
        re = pcre2_compile(
            reinterpret_cast<PCRE2_SPTR>("(?!)"),
            PCRE2_ZERO_TERMINATED, 0, &errcode, &erroffset, nullptr
        );
    } else {
        pcre2_jit_compile(re, PCRE2_JIT_COMPLETE | PCRE2_JIT_PARTIAL_SOFT);
    }

    code_ = re;
}

bool RegExp::test(const std::string& str) {
    if (!code_) return false;

    pcre2_code* re = static_cast<pcre2_code*>(code_);
    pcre2_match_data* md = pcre2_match_data_create_from_pattern(re, nullptr);
    if (!md) return false;

    PCRE2_SIZE start = 0;
    if ((global_ || sticky_) && last_index_ > 0) {
        start = static_cast<PCRE2_SIZE>(last_index_);
        if (start > str.length()) {
            last_index_ = 0;
            pcre2_match_data_free(md);
            return false;
        }
    }

    uint32_t opts = sticky_ ? PCRE2_ANCHORED : 0;
    int rc = pcre2_match(re,
        reinterpret_cast<PCRE2_SPTR>(str.c_str()), str.length(),
        start, opts, md, nullptr);

    bool found = (rc >= 0);
    if (found && global_) {
        last_index_ = static_cast<int>(pcre2_get_ovector_pointer(md)[1]);
    } else if (!found && (global_ || sticky_)) {
        last_index_ = 0;
    }

    pcre2_match_data_free(md);
    return found;
}

Value RegExp::exec(const std::string& str) {
    if (!code_) return Value::null();

    pcre2_code* re = static_cast<pcre2_code*>(code_);
    pcre2_match_data* md = pcre2_match_data_create_from_pattern(re, nullptr);
    if (!md) return Value::null();

    bool advances = global_ || sticky_;
    PCRE2_SIZE start = 0;
    if (advances && last_index_ > 0) {
        start = static_cast<PCRE2_SIZE>(last_index_);
        if (start > str.length()) {
            last_index_ = 0;
            pcre2_match_data_free(md);
            return Value::null();
        }
    }

    uint32_t opts = sticky_ ? PCRE2_ANCHORED : 0;
    int rc = pcre2_match(re,
        reinterpret_cast<PCRE2_SPTR>(str.c_str()), str.length(),
        start, opts, md, nullptr);

    if (rc < 0) {
        if (advances) last_index_ = 0;
        pcre2_match_data_free(md);
        return Value::null();
    }

    PCRE2_SIZE* ov = pcre2_get_ovector_pointer(md);

    uint32_t capture_count = 0;
    pcre2_pattern_info(re, PCRE2_INFO_CAPTURECOUNT, &capture_count);

    size_t match_start = ov[0];
    size_t match_end   = ov[1];
    std::vector<PCRE2_SIZE> saved(ov, ov + (capture_count + 1) * 2);

    if (advances) last_index_ = static_cast<int>(match_end);
    pcre2_match_data_free(md);

    auto result = new Object();
    result->set_property("0", Value(str.substr(match_start, match_end - match_start)));
    result->set_property("index", Value(static_cast<double>(match_start)));
    result->set_property("input", Value(str));
    result->set_property("length", Value(static_cast<double>(capture_count + 1)));

    for (uint32_t i = 1; i <= capture_count; ++i) {
        if (saved[2 * i] == PCRE2_UNSET)
            result->set_property(std::to_string(i), Value());
        else
            result->set_property(std::to_string(i),
                Value(str.substr(saved[2*i], saved[2*i+1] - saved[2*i])));
    }

    uint32_t name_count = 0;
    pcre2_pattern_info(re, PCRE2_INFO_NAMECOUNT, &name_count);
    if (name_count > 0) {
        uint32_t entry_size = 0;
        PCRE2_SPTR name_table = nullptr;
        pcre2_pattern_info(re, PCRE2_INFO_NAMEENTRYSIZE, &entry_size);
        pcre2_pattern_info(re, PCRE2_INFO_NAMETABLE, &name_table);

        auto groups = new Object();
        const unsigned char* tbl = reinterpret_cast<const unsigned char*>(name_table);
        for (uint32_t i = 0; i < name_count; ++i) {
            const unsigned char* e = tbl + i * entry_size;
            uint32_t gn = (e[0] << 8) | e[1];
            const char* name = reinterpret_cast<const char*>(e + 2);
            if (gn <= capture_count && saved[2*gn] != PCRE2_UNSET)
                groups->set_property(name, Value(str.substr(saved[2*gn], saved[2*gn+1] - saved[2*gn])));
            else
                groups->set_property(name, Value());
        }
        result->set_property("groups", Value(groups));
    }

    return Value(result);
}

void RegExp::compile(const std::string& pattern, const std::string& flags) {
    pattern_ = pattern;
    flags_ = flags;
    global_ = ignore_case_ = multiline_ = unicode_ = sticky_ = dotall_ = false;
    last_index_ = 0;
    parse_flags(flags_);
    do_compile();
}

std::string RegExp::to_string() const {
    return "/" + pattern_ + "/" + flags_;
}

}
