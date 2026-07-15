/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#define PCRE2_CODE_UNIT_WIDTH 16
#include "quanta/core/runtime/RegExp.h"
#include "quanta/core/runtime/RegExpBacktrack.h"
#include "quanta/core/runtime/Object.h"
#include "utf8proc.h"
#include <pcre2.h>
#include <vector>
#include <cstring>
#include <climits>
#include <cctype>
#include <cstdlib>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace Quanta {

// Set while a pattern is compiled purely to check validity (parser literals);
// skips JIT compilation of the probe.
static thread_local bool g_regexp_validation_only = false;

// Compiled-pattern cache: a regex literal evaluates to a fresh RegExp object
// every time (spec -- each has its own mutable lastIndex), but the underlying
// pcre2_code is immutable once built and PCRE2_JIT_COMPLETE is expensive, so
// re-running pcre2_compile+JIT for the same (pattern,flags) on every
// evaluation was pure waste. thread_local: no cross-thread pcre2_code
// sharing, matching this codebase's per-thread-everything architecture.
// Validation-only probes (is_valid_unicode_pattern) never read or write this
// -- they skip JIT, and caching an un-JIT'd entry would stick a real regex
// with a slower match path forever.
namespace {
struct CompiledRegexEntry {
    std::shared_ptr<void> code;  // pcre2_code*
    std::vector<std::pair<std::string, std::vector<uint32_t>>> named_groups;
    std::shared_ptr<RegexBacktrackEngine> backtrack_engine;
};
constexpr size_t kRegexCacheCap = 512;
thread_local std::unordered_map<std::string, CompiledRegexEntry> g_regex_cache;
}

// PCRE2 runs in 16-bit mode: match offsets ARE JS string indices, and
// non-unicode patterns get real UTF-16 code-unit semantics.

// Decode WTF-8 (UTF-8 plus 3-byte-encoded lone surrogates) to UTF-16 code units.
std::u16string wtf8_to_utf16(const std::string& s) {
    std::u16string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { out += (char16_t)c; i++; continue; }
        size_t len = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
        if (i + len > s.size()) { out += u'�'; break; }
        uint32_t cp = len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
        for (size_t k = 1; k < len; k++)
            cp = (cp << 6) | ((unsigned char)s[i+k] & 0x3F);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out += (char16_t)(0xD800 + (cp >> 10));
            out += (char16_t)(0xDC00 + (cp & 0x3FF));
        } else {
            out += (char16_t)cp; // includes WTF-8 lone surrogates as-is
        }
        i += len;
    }
    return out;
}

// Encode UTF-16 back to WTF-8; unpaired surrogates get the 3-byte WTF-8 form.
std::string utf16_to_wtf8(const char16_t* p, size_t len) {
    std::string out;
    out.reserve(len * 3);
    for (size_t i = 0; i < len; i++) {
        uint32_t cp = p[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < len &&
            p[i+1] >= 0xDC00 && p[i+1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (p[i+1] - 0xDC00);
            i++;
        }
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

// Replace raw WTF-8 lone surrogates in a PATTERN with \x{D8xx} escapes so the
// normalization pass below sees every surrogate in one uniform form.
static std::string escape_wtf8_surrogates(const std::string& s) {
    if (s.find('\xED') == std::string::npos) return s;
    std::string out;
    out.reserve(s.size() + 16);
    for (size_t i = 0; i < s.size(); ) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0xED && i + 2 < s.size()) {
            unsigned char c1 = (unsigned char)s[i+1];
            if (c1 >= 0xA0 && c1 <= 0xBF) {
                uint32_t cp = 0xD000 | ((c1 & 0x3F) << 6) | ((unsigned char)s[i+2] & 0x3F);
                char buf[12];
                snprintf(buf, sizeof(buf), "\\x{%X}", cp);
                out += buf;
                i += 3;
                continue;
            }
        }
        out += s[i++];
    }
    return out;
}

static bool is_hex_ch(char c);

// PCRE2_UTF in 16-bit mode rejects surrogate escapes outright, so a high+low
// \x{} pair is combined to the astral code point and an unpaired one becomes
// U+FFFD (mirrors the subject-side sanitization).
static std::string normalize_u_surrogate_escapes(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    auto parse_x = [&](size_t at, uint32_t& val, size_t& end) -> bool {
        // expects at -> backslash of "\x{...}"
        if (at + 3 >= s.size() || s[at] != '\\' || s[at+1] != 'x' || s[at+2] != '{') return false;
        size_t close = s.find('}', at + 3);
        if (close == std::string::npos || close == at + 3) return false;
        uint32_t v = 0;
        for (size_t k = at + 3; k < close; k++) {
            char c = s[k];
            if (!is_hex_ch(c)) return false;
            v = v * 16 + (c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
            if (v > 0x10FFFF) return false;
        }
        val = v;
        end = close + 1;
        return true;
    };
    for (size_t i = 0; i < s.size(); ) {
        if (s[i] == '\\') {
            uint32_t hi;
            size_t hi_end;
            if (parse_x(i, hi, hi_end) && hi >= 0xD800 && hi <= 0xDFFF) {
                uint32_t lo;
                size_t lo_end;
                if (hi <= 0xDBFF && parse_x(hi_end, lo, lo_end) && lo >= 0xDC00 && lo <= 0xDFFF) {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "\\x{%X}", 0x10000 + ((hi - 0xD800) << 10) + (lo - 0xDC00));
                    out += buf;
                    i = lo_end;
                    continue;
                }
                out += "\\x{FFFD}";
                i = hi_end;
                continue;
            }
            out += s[i];
            if (i + 1 < s.size()) out += s[i+1];
            i += 2;
            continue;
        }
        out += s[i++];
    }
    return out;
}

RegExp::RegExp(const std::string& pattern, const std::string& flags)
    : pattern_(pattern), flags_(flags), code_(nullptr),
      global_(false), ignore_case_(false), multiline_(false),
      unicode_(false), sticky_(false), dotall_(false), unicode_sets_(false),
      has_indices_(false), last_index_(0) {
    parse_flags(flags);
    do_compile();
}

RegExp::~RegExp() {
    // code_owner_'s deleter (if this was the last reference) frees code_.
}

void RegExp::parse_flags(const std::string& flags) {
    bool seen[8] = {};
    for (char flag : flags) {
        int idx = -1;
        switch (flag) {
            case 'd': has_indices_ = true;   idx = 0; break;
            case 'g': global_ = true;        idx = 1; break;
            case 'i': ignore_case_ = true;   idx = 2; break;
            case 'm': multiline_ = true;     idx = 3; break;
            case 's': dotall_ = true;        idx = 4; break;
            case 'u': unicode_ = true;       idx = 5; break;
            case 'v': unicode_sets_ = true; unicode_ = true; idx = 6; break;
            case 'y': sticky_ = true;        idx = 7; break;
            default:
                throw std::runtime_error("Invalid regular expression flags");
        }
        if (idx >= 0 && seen[idx])
            throw std::runtime_error("Invalid regular expression flags");
        if (idx >= 0) seen[idx] = true;
    }
}

// Convert \uXXXX and \u{XXXXX} to \x{XXXX} for PCRE2. Surrogate pairs combine
// to one code point only in unicode mode -- \x{} above 0xFFFF needs PCRE2_UTF,
// and non-unicode mode keeps the two escapes as separate JS-semantics units.
static std::string convert_unicode_escapes(const std::string& pat, bool unicode) {
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
                if (unicode && cp >= 0xD800 && cp <= 0xDBFF && i + 11 < pat.size() &&
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

static void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// ES RegExpIdentifierStart/Part: UnicodeIDStart/IDContinue plus $, _, ZWNJ, ZWJ.
static bool is_id_start_cp(uint32_t cp) {
    if (cp < 0x80)
        return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || cp == '_' || cp == '$';
    if (cp >= 0xD800 && cp <= 0xDFFF) return false;
    switch (utf8proc_category(static_cast<utf8proc_int32_t>(cp))) {
        case UTF8PROC_CATEGORY_LU: case UTF8PROC_CATEGORY_LL: case UTF8PROC_CATEGORY_LT:
        case UTF8PROC_CATEGORY_LM: case UTF8PROC_CATEGORY_LO: case UTF8PROC_CATEGORY_NL:
            return true;
        default: break;
    }
    // Other_ID_Start
    return cp == 0x1885 || cp == 0x1886 || cp == 0x2118 || cp == 0x212E ||
           cp == 0x309B || cp == 0x309C;
}

static bool is_id_continue_cp(uint32_t cp) {
    if (cp < 0x80)
        return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') ||
               (cp >= '0' && cp <= '9') || cp == '_' || cp == '$';
    if (is_id_start_cp(cp)) return true;
    switch (utf8proc_category(static_cast<utf8proc_int32_t>(cp))) {
        case UTF8PROC_CATEGORY_MN: case UTF8PROC_CATEGORY_MC:
        case UTF8PROC_CATEGORY_ND: case UTF8PROC_CATEGORY_PC:
            return true;
        default: break;
    }
    // Other_ID_Continue plus ZWNJ/ZWJ
    return cp == 0x00B7 || cp == 0x0387 || (cp >= 0x1369 && cp <= 0x1371) ||
           cp == 0x19DA || cp == 0x200C || cp == 0x200D;
}

// Decode \uXXXX and \u{...} escapes in a group name to UTF-8, combining surrogate
// pairs, and validate it as a RegExpIdentifierName. Throws on malformed input.
static std::string decode_group_name(const std::string& raw) {
    std::string out;
    std::vector<uint32_t> cps;
    size_t i = 0;
    auto hex_val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    auto parse_u = [&](uint32_t& cp) -> bool {
        if (i >= raw.size()) return false;
        if (raw[i] == '{') {
            size_t end = raw.find('}', i + 1);
            if (end == std::string::npos || end == i + 1) return false;
            uint32_t v = 0;
            for (size_t k = i + 1; k < end; k++) {
                int h = hex_val(raw[k]);
                if (h < 0) return false;
                v = v * 16 + h;
                if (v > 0x10FFFF) return false;
            }
            cp = v;
            i = end + 1;
            return true;
        }
        if (i + 4 > raw.size()) return false;
        uint32_t v = 0;
        for (size_t k = 0; k < 4; k++) {
            int h = hex_val(raw[i + k]);
            if (h < 0) return false;
            v = v * 16 + h;
        }
        cp = v;
        i += 4;
        return true;
    };
    while (i < raw.size()) {
        if (raw[i] == '\\') {
            if (i + 1 >= raw.size() || raw[i+1] != 'u')
                throw std::runtime_error("Invalid regular expression: invalid escape in capture group name");
            i += 2;
            uint32_t cp = 0;
            if (!parse_u(cp))
                throw std::runtime_error("Invalid regular expression: invalid unicode escape in capture group name");
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < raw.size() && raw[i] == '\\' && raw[i+1] == 'u') {
                size_t save = i;
                i += 2;
                uint32_t lo = 0;
                if (parse_u(lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else {
                    i = save;
                }
            }
            cps.push_back(cp);
            continue;
        }
        unsigned char c = (unsigned char)raw[i];
        size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        uint32_t cp = len == 1 ? c : len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
        for (size_t k = 1; k < len && i + k < raw.size(); k++)
            cp = (cp << 6) | ((unsigned char)raw[i+k] & 0x3F);
        // WTF-8 surrogate pair halves in source text combine like escaped pairs.
        if (cp >= 0xD800 && cp <= 0xDBFF && i + len < raw.size()) {
            size_t j = i + len;
            unsigned char c2 = (unsigned char)raw[j];
            if (c2 >= 0xE0 && c2 < 0xF0 && j + 2 < raw.size()) {
                uint32_t lo = ((c2 & 0x0F) << 12) | (((unsigned char)raw[j+1] & 0x3F) << 6) |
                              ((unsigned char)raw[j+2] & 0x3F);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    len += 3;
                }
            }
        }
        cps.push_back(cp);
        i += len;
    }
    for (size_t k = 0; k < cps.size(); k++) {
        bool ok = (k == 0) ? is_id_start_cp(cps[k]) : is_id_continue_cp(cps[k]);
        if (!ok)
            throw std::runtime_error("Invalid regular expression: invalid capture group name");
        append_utf8(out, cps[k]);
    }
    return out;
}

// Rewrites JS group names to synthetic PCRE2-safe names (q0, q1, ...), recording
// the mapping in named_groups_. Duplicate names share one synthetic name.
std::string RegExp::rename_named_groups(const std::string& pat) {
    named_groups_.clear();
    if (pat.find("(?<") == std::string::npos && pat.find("\\k<") == std::string::npos)
        return pat;

    auto find_group = [this](const std::string& name) -> int {
        for (size_t k = 0; k < named_groups_.size(); k++)
            if (named_groups_[k].first == name) return static_cast<int>(k);
        return -1;
    };

    // Pass 1: collect names with capture numbers. Duplicate names are only legal
    // across different disjunction alternatives; the frame stack tracks names that
    // can participate in the same match (child frames merge into their parent on
    // group close, '|' clears the current frame).
    {
        int cc_depth = 0;
        uint32_t ncap = 0;
        std::vector<std::unordered_set<std::string>> frames(1);
        auto in_any_frame = [&](const std::string& n) {
            for (auto& f : frames) if (f.count(n)) return true;
            return false;
        };
        for (size_t i = 0; i < pat.size(); i++) {
            if (pat[i] == '\\') { i++; continue; }
            if (pat[i] == '[') { cc_depth++; continue; }
            if (pat[i] == ']') { if (cc_depth > 0) cc_depth--; continue; }
            if (cc_depth > 0) continue;
            if (pat[i] == ')') {
                if (frames.size() > 1) {
                    auto top = std::move(frames.back());
                    frames.pop_back();
                    frames.back().insert(top.begin(), top.end());
                }
                continue;
            }
            if (pat[i] == '|') {
                frames.back().clear();
                continue;
            }
            if (pat[i] != '(') continue;
            if (i + 1 < pat.size() && pat[i+1] == '?') {
                if (i + 2 < pat.size() && pat[i+2] == '<' &&
                    i + 3 < pat.size() && pat[i+3] != '=' && pat[i+3] != '!') {
                    size_t end = pat.find('>', i + 3);
                    if (end == std::string::npos)
                        throw std::runtime_error("Invalid regular expression: unterminated capture group name");
                    std::string name = decode_group_name(pat.substr(i + 3, end - (i + 3)));
                    if (name.empty())
                        throw std::runtime_error("Invalid regular expression: empty capture group name");
                    if (in_any_frame(name))
                        throw std::runtime_error("Invalid regular expression: duplicate capture group name");
                    frames.back().insert(name);
                    ncap++;
                    int idx = find_group(name);
                    if (idx < 0) {
                        named_groups_.push_back({name, {ncap}});
                    } else {
                        named_groups_[idx].second.push_back(ncap);
                    }
                    i = end;
                }
                frames.push_back({});
                continue;
            }
            ncap++;
            frames.push_back({});
        }
    }

    // Pass 2: rewrite group definitions and \k references.
    std::string out;
    out.reserve(pat.size());
    int cc_depth = 0;
    for (size_t i = 0; i < pat.size(); i++) {
        if (pat[i] == '\\' && i + 1 < pat.size()) {
            if (pat[i+1] == 'k' && cc_depth == 0 && i + 2 < pat.size() && pat[i+2] == '<') {
                size_t end = pat.find('>', i + 3);
                if (end != std::string::npos) {
                    std::string name;
                    bool decoded = true;
                    try { name = decode_group_name(pat.substr(i + 3, end - (i + 3))); }
                    catch (...) { decoded = false; }
                    int idx = decoded ? find_group(name) : -1;
                    if (idx >= 0) {
                        out += "\\k<q" + std::to_string(idx) + ">";
                        i = end;
                        continue;
                    }
                    if (!named_groups_.empty())
                        throw std::runtime_error("Invalid regular expression: invalid named capture group reference");
                }
            }
            out += pat[i++];
            out += pat[i];
            continue;
        }
        if (pat[i] == '[') { cc_depth++; out += pat[i]; continue; }
        if (pat[i] == ']') { if (cc_depth > 0) cc_depth--; out += pat[i]; continue; }
        if (cc_depth == 0 && pat[i] == '(' && i + 2 < pat.size() && pat[i+1] == '?' && pat[i+2] == '<' &&
            i + 3 < pat.size() && pat[i+3] != '=' && pat[i+3] != '!') {
            size_t end = pat.find('>', i + 3);
            std::string name = decode_group_name(pat.substr(i + 3, end - (i + 3)));
            out += "(?<q" + std::to_string(find_group(name)) + ">";
            i = end;
            continue;
        }
        out += pat[i];
    }
    return out;
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
            static const char* js_valid_escapes = "cdDwWsSnrtfvbBxu0123456789().[]{}\\^$|*+?/";
            bool is_valid = false;
            // In non-Unicode mode, \k<name> with no named group → literal 'k' (identity escape).
            // Scan ahead to check if the referenced group exists in the pattern.
            if (next == 'k' && i + 2 < pat.size() && pat[i+2] == '<') {
                size_t close = pat.find('>', i + 3);
                if (close != std::string::npos) {
                    std::string ref = pat.substr(i + 3, close - (i + 3));
                    bool group_exists = pat.find("(?<" + ref + ">") != std::string::npos;
                    if (group_exists) {
                        is_valid = true;
                    } else {
                        // Identity escape: output 'k' literally and continue
                        result += 'k';
                        i += 2;
                        continue;
                    }
                } else {
                    result += 'k'; i += 2; continue;
                }
            } else if (next == 'k' || next == 'p' || next == 'P') {
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

        // [^] is ES "match any character" (empty negated class); replace for PCRE2
        if (ch == '[' && !in_char_class && i + 2 < pat.size() && pat[i+1] == '^' && pat[i+2] == ']') {
            result += "[\\s\\S]";
            i += 3;
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

// JS has no POSIX class syntax, but PCRE2 parses [.x.], [:x:], [=x=] inside
// character classes (e.g. /[.-.]/ fails as a collating element). Escape the
// trigger character so these stay ordinary literals. Runs after the v-mode
// transform, so every remaining class is flat.
static std::string escape_posix_lookalikes(const std::string& pat) {
    std::string result;
    result.reserve(pat.size() + 8);
    int cc_depth = 0;
    for (size_t i = 0; i < pat.size(); i++) {
        if (pat[i] == '\\' && i + 1 < pat.size()) {
            result += pat[i++];
            result += pat[i];
            continue;
        }
        if (pat[i] == '[') {
            if (cc_depth == 0) {
                cc_depth = 1;
                result += '[';
                if (i + 1 < pat.size() && pat[i+1] == '^') { result += '^'; i++; }
                if (i + 1 < pat.size() && (pat[i+1] == '.' || pat[i+1] == ':' || pat[i+1] == '=')) {
                    result += '\\';
                    result += pat[i+1];
                    i++;
                }
            } else {
                // Literal '[' inside a class: escape when a POSIX form would follow.
                if (i + 1 < pat.size() && (pat[i+1] == '.' || pat[i+1] == ':' || pat[i+1] == '=')) result += '\\';
                result += '[';
            }
            continue;
        }
        if (pat[i] == ']' && cc_depth > 0) cc_depth = 0;
        result += pat[i];
    }
    return result;
}

static std::string adjust_dot(const std::string& pat, bool dotall) {
    // JS non-dotAll '.' excludes exactly the four LineTerminators (PCRE2's dot
    // depends on its newline convention instead); s-state is tracked through
    // (?s:)/(?-s:) modifier groups so the substitution follows scope.
    std::string result;
    result.reserve(pat.size() + 32);
    std::vector<bool> stk;
    bool cur_s = dotall;
    bool in_cc = false;
    for (size_t i = 0; i < pat.size(); i++) {
        if (pat[i] == '\\' && i + 1 < pat.size()) {
            result += pat[i++]; result += pat[i]; continue;
        }
        if (pat[i] == '[' && !in_cc) { in_cc = true; result += pat[i]; continue; }
        if (pat[i] == ']' && in_cc) { in_cc = false; result += pat[i]; continue; }
        if (in_cc) { result += pat[i]; continue; }
        if (pat[i] == '(') {
            bool next_s = cur_s;
            if (i + 1 < pat.size() && pat[i+1] == '?') {
                size_t k = i + 2;
                bool add_s = false, rem_s = false, dash = false, is_mod = false;
                while (k < pat.size()) {
                    char c = pat[k];
                    if (c == ':') { is_mod = (k > i + 2); break; }
                    if (c == '-' && !dash) { dash = true; k++; continue; }
                    if (c == 'i' || c == 'm' || c == 's') {
                        if (c == 's') (dash ? rem_s : add_s) = true;
                        k++;
                        continue;
                    }
                    break;
                }
                if (is_mod) {
                    if (add_s) next_s = true;
                    if (rem_s) next_s = false;
                }
            }
            stk.push_back(cur_s);
            cur_s = next_s;
            result += pat[i];
            continue;
        }
        if (pat[i] == ')' && !stk.empty()) {
            cur_s = stk.back();
            stk.pop_back();
            result += pat[i];
            continue;
        }
        if (pat[i] == '.' && !cur_s) {
            result += "[^\\n\\r\\x{2028}\\x{2029}]";
            continue;
        }
        result += pat[i];
    }
    return result;
}

// PCRE2 backtracks into (?=...) / (?!...) content; ES spec requires atomicity.
// Wrap lookahead content in (?>...) to prevent backtracking.
// Inside a lookbehind/lookahead, replace unbounded quantifiers with bounded ones so
// PCRE2 can compile variable-length lookbehinds (error 125 otherwise).
static std::string bound_lookbehind_quantifiers(const std::string& pat) {
    std::string result;
    result.reserve(pat.size() + 64);
    size_t i = 0;
    bool in_cc = false;
    std::vector<bool> stk; // true = this group is a lookbehind
    int lb_depth = 0;

    while (i < pat.size()) {
        if (pat[i] == '\\' && i + 1 < pat.size()) {
            result += pat[i++]; result += pat[i++]; continue;
        }
        if (pat[i] == '[' && !in_cc) { in_cc = true; result += pat[i++]; continue; }
        if (pat[i] == ']' && in_cc) { in_cc = false; result += pat[i++]; continue; }
        if (in_cc) { result += pat[i++]; continue; }

        if (pat[i] == '(') {
            bool is_lb = i + 3 < pat.size() && pat[i+1] == '?' && pat[i+2] == '<' &&
                         (pat[i+3] == '=' || pat[i+3] == '!');
            stk.push_back(is_lb);
            if (is_lb) lb_depth++;
            result += pat[i++];
            continue;
        }
        if (pat[i] == ')' && !stk.empty()) {
            if (stk.back()) lb_depth--;
            stk.pop_back();
            result += pat[i++];
            continue;
        }

        if (lb_depth > 0) {
            if (pat[i] == '*' || pat[i] == '+') {
                result += (pat[i] == '*') ? "{0,255}" : "{1,255}";
                i++;
                if (i < pat.size() && pat[i] == '?') { result += '?'; i++; }
                continue;
            }
            if (pat[i] == '{') {
                size_t j = i + 1;
                while (j < pat.size() && pat[j] >= '0' && pat[j] <= '9') j++;
                if (j > i + 1 && j < pat.size() && pat[j] == ',') {
                    size_t k = j + 1;
                    if (k < pat.size() && pat[k] == '}') {
                        result += pat.substr(i, j - i + 1);
                        result += "255}";
                        i = k + 1;
                        if (i < pat.size() && pat[i] == '?') { result += '?'; i++; }
                        continue;
                    }
                }
            }
        }

        result += pat[i++];
    }
    return result;
}

// True when pat[start..end) consists only of assertions (^ $ \b \B, lookarounds,
// alternation of those): such content can never consume input.
static bool is_assertion_only_content(const std::string& p, size_t start, size_t end) {
    size_t i = start;
    while (i < end) {
        char c = p[i];
        if (c == '^' || c == '$' || c == '|') { i++; continue; }
        if (c == '\\' && i + 1 < end && (p[i+1] == 'b' || p[i+1] == 'B')) { i += 2; continue; }
        if (c == '(') {
            bool la = i + 2 < end && p[i+1] == '?' && (p[i+2] == '=' || p[i+2] == '!');
            bool lb = i + 3 < end && p[i+1] == '?' && p[i+2] == '<' && (p[i+3] == '=' || p[i+3] == '!');
            if (!la && !lb) return false;
            int depth = 1;
            bool in_cc = false;
            i++;
            while (i < end && depth > 0) {
                if (p[i] == '\\' && i + 1 < end) { i += 2; continue; }
                if (p[i] == '[' && !in_cc) { in_cc = true; i++; continue; }
                if (p[i] == ']' && in_cc) { in_cc = false; i++; continue; }
                if (!in_cc && p[i] == '(') depth++;
                if (!in_cc && p[i] == ')') depth--;
                i++;
            }
            continue;
        }
        return false;
    }
    return true;
}

// ES RepeatMatcher discards a min=0 iteration that matched empty, leaving its
// captures undefined; PCRE2 keeps them. Wrapping the group as (?:G(?!)) forces
// the zero-iteration path while preserving capture numbering.
static std::string discard_empty_optional_groups(const std::string& pat) {
    std::string p = pat;
    bool changed = true;
    while (changed) {
        changed = false;
        bool in_cc = false;
        for (size_t i = 0; i < p.size(); i++) {
            if (p[i] == '\\' && i + 1 < p.size()) { i++; continue; }
            if (p[i] == '[' && !in_cc) { in_cc = true; continue; }
            if (p[i] == ']' && in_cc) { in_cc = false; continue; }
            if (in_cc || p[i] != '(') continue;
            // Skip groups we already wrapped: (?:G(?!)) scans as normal groups whose
            // content is not assertion-only, so no infinite loop.
            bool is_lookaround = (i + 2 < p.size() && p[i+1] == '?' && (p[i+2] == '=' || p[i+2] == '!')) ||
                                 (i + 3 < p.size() && p[i+1] == '?' && p[i+2] == '<' &&
                                  (p[i+3] == '=' || p[i+3] == '!'));
            size_t content_start = i + 1;
            if (i + 1 < p.size() && p[i+1] == '?') {
                size_t k = i + 2;
                if (k < p.size() && p[k] == '<') {
                    size_t gt = p.find('>', k);
                    content_start = (gt == std::string::npos) ? p.size() : gt + 1;
                } else {
                    while (k < p.size() && p[k] != ':' && p[k] != ')') k++;
                    content_start = (k < p.size() && p[k] == ':') ? k + 1 : i + 1;
                }
                if (is_lookaround) content_start = i + (p[i+2] == '<' ? 4 : 3);
            }
            size_t j = i + 1;
            int depth = 1;
            bool cc2 = false;
            while (j < p.size() && depth > 0) {
                if (p[j] == '\\' && j + 1 < p.size()) { j += 2; continue; }
                if (p[j] == '[' && !cc2) { cc2 = true; j++; continue; }
                if (p[j] == ']' && cc2) { cc2 = false; j++; continue; }
                if (!cc2 && p[j] == '(') depth++;
                if (!cc2 && p[j] == ')') depth--;
                j++;
            }
            if (depth != 0) break;
            size_t close = j - 1;
            bool min_zero = false;
            if (j < p.size()) {
                if (p[j] == '?' || p[j] == '*') min_zero = true;
                else if (p[j] == '{' && j + 1 < p.size() && p[j+1] == '0') min_zero = true;
            }
            if (!min_zero) continue;
            // Lookarounds are zero-width by definition; other groups qualify only
            // when their content cannot consume input.
            if (!is_lookaround && !is_assertion_only_content(p, content_start, close)) continue;
            p = p.substr(0, i) + "(?:" + p.substr(i, j - i) + "(?!))" + p.substr(j);
            changed = true;
            break;
        }
    }
    return p;
}

// Returns true if pat[start..end) contains a | alternation outside char classes.
static bool has_alternation(const std::string& pat, size_t start, size_t end) {
    bool in_cc = false;
    for (size_t i = start; i < end; i++) {
        if (pat[i] == '\\' && i + 1 < end) { i++; continue; }
        if (pat[i] == '[' && !in_cc) { in_cc = true; continue; }
        if (pat[i] == ']' && in_cc) { in_cc = false; continue; }
        if (in_cc) continue;
        if (pat[i] == '|') return true;
    }
    return false;
}

// Wrap lookahead (?=/!) content and positive lookbehind (?<=) content in (?>...) to
// enforce ES spec atomicity. For lookbehinds, only wrap when there are alternatives
// (|) -- quantifier-only lookbehinds need normal backtracking to work correctly.
static std::string wrap_lookaheads_atomic(const std::string& pat) {
    std::string result;
    result.reserve(pat.size() + 32);
    size_t i = 0;
    bool in_cc = false;

    while (i < pat.size()) {
        if (pat[i] == '\\' && i + 1 < pat.size()) {
            result += pat[i++]; result += pat[i++]; continue;
        }
        if (pat[i] == '[' && !in_cc) { in_cc = true; result += pat[i++]; continue; }
        if (pat[i] == ']' && in_cc) { in_cc = false; result += pat[i++]; continue; }
        if (in_cc) { result += pat[i++]; continue; }

        bool is_la = pat[i] == '(' && i + 2 < pat.size() && pat[i+1] == '?' &&
                     (pat[i+2] == '=' || pat[i+2] == '!');
        // Only positive lookbehind (?<= needs atomicity; (?<! passes when nothing matches.
        bool is_lb = pat[i] == '(' && i + 3 < pat.size() && pat[i+1] == '?' &&
                     pat[i+2] == '<' && pat[i+3] == '=';

        if (is_la || is_lb) {
            // For positive lookbehinds, pre-scan content to check for alternation.
            // Only wrap with (?>...) if alternations are present.
            bool do_wrap = is_la;
            if (is_lb) {
                size_t j = i + 4, d = 1;
                bool jcc = false;
                while (j < pat.size() && d > 0) {
                    if (pat[j] == '\\' && j+1 < pat.size()) { j += 2; continue; }
                    if (pat[j] == '[' && !jcc) { jcc = true; j++; continue; }
                    if (pat[j] == ']' && jcc) { jcc = false; j++; continue; }
                    if (jcc) { j++; continue; }
                    if (pat[j] == '(') { d++; j++; }
                    else if (pat[j] == ')') { d--; j++; }
                    else j++;
                }
                // Content is pat[i+4 .. j-1), closing ')' at j-1.
                do_wrap = has_alternation(pat, i + 4, j - 1);
            }

            result += pat[i++]; // (
            result += pat[i++]; // ?
            if (is_lb) result += pat[i++]; // <
            result += pat[i++]; // = or !
            if (do_wrap) result += "(?>";

            int depth = 1;
            bool la_cc = false;
            while (i < pat.size() && depth > 0) {
                if (pat[i] == '\\' && i + 1 < pat.size()) {
                    result += pat[i++]; result += pat[i++]; continue;
                }
                if (pat[i] == '[' && !la_cc) { la_cc = true; result += pat[i++]; continue; }
                if (pat[i] == ']' && la_cc) { la_cc = false; result += pat[i++]; continue; }
                if (la_cc) { result += pat[i++]; continue; }
                if (pat[i] == '(') { depth++; result += pat[i++]; continue; }
                if (pat[i] == ')') {
                    depth--;
                    if (depth == 0) {
                        if (do_wrap) result += ')';
                        result += pat[i++];
                        break;
                    }
                }
                result += pat[i++];
            }
            continue;
        }

        result += pat[i++];
    }
    return result;
}

// PCRE2 retains inner captures from previous iterations of * / +; ES spec requires
// them undefined if they didn't participate in the last iteration. Reset captures
// whose span falls outside their nearest enclosing repeated capturing group's span.
static void reset_stale_captures(const std::string& pat,
                                  std::vector<PCRE2_SIZE>& ov, uint32_t cap_count) {
    if (cap_count == 0) return;

    struct GInfo { int num; bool is_cap; bool repeated; };
    std::vector<GInfo> groups;
    std::vector<int> stk;
    bool esc = false, in_cc = false;
    int n_cap = 0;

    for (size_t i = 0; i < pat.size(); ++i) {
        if (esc) { esc = false; continue; }
        if (pat[i] == '\\') { esc = true; continue; }
        if (pat[i] == '[' && !in_cc) { in_cc = true; continue; }
        if (pat[i] == ']' && in_cc) { in_cc = false; continue; }
        if (in_cc) continue;
        if (pat[i] == '(') {
            bool is_cap = true;
            if (i + 1 < pat.size() && pat[i+1] == '?') {
                is_cap = false;
                if (i + 3 < pat.size() && pat[i+2] == '<' &&
                    pat[i+3] != '=' && pat[i+3] != '!')
                    is_cap = true;
            }
            GInfo g; g.num = is_cap ? ++n_cap : 0; g.is_cap = is_cap; g.repeated = false;
            groups.push_back(g);
            stk.push_back((int)groups.size() - 1);
        } else if (pat[i] == ')' && !stk.empty()) {
            int idx = stk.back(); stk.pop_back();
            if (i + 1 < pat.size() && (pat[i+1] == '*' || pat[i+1] == '+'))
                groups[idx].repeated = true;
        }
    }

    std::vector<int> rep_parent(cap_count, 0);
    stk.clear(); esc = false; in_cc = false;
    int gi = 0;
    for (size_t i = 0; i < pat.size(); ++i) {
        if (esc) { esc = false; continue; }
        if (pat[i] == '\\') { esc = true; continue; }
        if (pat[i] == '[' && !in_cc) { in_cc = true; continue; }
        if (pat[i] == ']' && in_cc) { in_cc = false; continue; }
        if (in_cc) continue;
        if (pat[i] == '(') {
            int rp = 0;
            for (int k = (int)stk.size() - 1; k >= 0; --k) {
                const GInfo& g = groups[stk[k]];
                if (g.is_cap && g.repeated) { rp = g.num; break; }
            }
            const GInfo& cur = groups[gi];
            if (cur.is_cap && cur.num >= 1 && cur.num <= (int)cap_count)
                rep_parent[cur.num - 1] = rp;
            stk.push_back(gi++);
        } else if (pat[i] == ')' && !stk.empty()) {
            stk.pop_back();
        }
    }

    for (uint32_t i = 1; i <= cap_count; ++i) {
        int rp = rep_parent[i - 1];
        if (rp <= 0 || rp > (int)cap_count) continue;
        if (ov[2*i] == PCRE2_UNSET || ov[2*rp] == PCRE2_UNSET) continue;
        if (ov[2*i] < ov[2*rp] || ov[2*i+1] > ov[2*rp+1])
            ov[2*i] = ov[2*i+1] = PCRE2_UNSET;
    }
}

// JS spec: \d = [0-9], \w = [A-Za-z0-9_], \s = specific Unicode whitespace list.
// PCRE2_UCP extends these to full Unicode categories and non-UCP \s is ASCII-only;
// both are wrong per spec, so \s and \S are expanded to explicit code point sets.
static std::string expand_js_charclass_shortcuts(const std::string& p, bool unicode) {
    // JS \s whitespace — exactly the set from ECMAScript spec (WhiteSpace + LineTerminator)
    static const char* s_inner  = "\\t\\n\\x0B\\f\\r\\x20\\x{00A0}\\x{1680}\\x{2000}-\\x{200A}\\x{2028}\\x{2029}\\x{202F}\\x{205F}\\x{3000}\\x{FEFF}";
    static const char* s_outer  = "[\\t\\n\\x0B\\f\\r\\x20\\x{00A0}\\x{1680}\\x{2000}-\\x{200A}\\x{2028}\\x{2029}\\x{202F}\\x{205F}\\x{3000}\\x{FEFF}]";
    // Complement of the JS whitespace set as explicit ranges so \S also works
    // inside character classes (set subtraction is not available in PCRE2 /u).
    static const char* S_inner_u  = "\\x00-\\x08\\x0E-\\x1F\\x21-\\x9F\\x{A1}-\\x{167F}\\x{1681}-\\x{1FFF}\\x{200B}-\\x{2027}\\x{202A}-\\x{202E}\\x{2030}-\\x{205E}\\x{2060}-\\x{2FFF}\\x{3001}-\\x{FEFE}\\x{FF00}-\\x{10FFFF}";
    static const char* S_inner_16 = "\\x00-\\x08\\x0E-\\x1F\\x21-\\x9F\\x{A1}-\\x{167F}\\x{1681}-\\x{1FFF}\\x{200B}-\\x{2027}\\x{202A}-\\x{202E}\\x{2030}-\\x{205E}\\x{2060}-\\x{2FFF}\\x{3001}-\\x{FEFE}\\x{FF00}-\\x{FFFF}";
    const char* S_inner = unicode ? S_inner_u : S_inner_16;

    std::string result;
    result.reserve(p.size() * 2);
    int cc_depth = 0;
    for (size_t i = 0; i < p.size(); i++) {
        if (p[i] == '\\' && i + 1 < p.size()) {
            char next = p[i + 1];
            if (next == 's') {
                result += (cc_depth > 0) ? s_inner : s_outer;
                i++;
                continue;
            }
            if (next == 'S') {
                if (cc_depth > 0) result += S_inner;
                else { result += '['; result += S_inner; result += ']'; }
                i++;
                continue;
            }
            result += p[i++];
            result += p[i];
        } else if (p[i] == '[') {
            if (cc_depth == 0 && i + 2 < p.size() && p[i+1] == '^' && p[i+2] == ']') {
                result += "[\\s\\S]";
                i += 2;
                continue;
            }
            cc_depth++;
            result += p[i];
        } else if (p[i] == ']' && cc_depth > 0) {
            cc_depth--;
            result += p[i];
        } else {
            result += p[i];
        }
    }
    return result;
}

// Non-unicode mode compiles with PCRE2_UCP (for full-range caseless folding),
// which would wrongly extend \d/\w/\b to Unicode categories -- rewrite them to
// their ASCII sets first. Inside classes, \D/\W become explicit complement ranges.
static std::string expand_ascii_word_classes(const std::string& p) {
    static const char* W = "[A-Za-z0-9_]";
    static const char* D_ranges = "\\x00-\\x2F\\x3A-\\x{FFFF}";
    static const char* W_ranges = "\\x00-\\x2F\\x3A-\\x40\\x5B-\\x5E\\x60\\x7B-\\x{FFFF}";
    std::string result;
    result.reserve(p.size() + 32);
    int cc_depth = 0;
    for (size_t i = 0; i < p.size(); i++) {
        if (p[i] == '\\' && i + 1 < p.size()) {
            char n = p[i+1];
            bool in_cc = cc_depth > 0;
            switch (n) {
                case 'd': result += in_cc ? "0-9" : "[0-9]"; i++; continue;
                case 'D':
                    if (in_cc) result += D_ranges;
                    else result += "[^0-9]";
                    i++;
                    continue;
                case 'w': result += in_cc ? "A-Za-z0-9_" : W; i++; continue;
                case 'W':
                    if (in_cc) result += W_ranges;
                    else result += "[^A-Za-z0-9_]";
                    i++;
                    continue;
                case 'b':
                    if (!in_cc) { // inside a class \b is backspace
                        result += std::string("(?:(?<=") + W + ")(?!" + W + ")|(?<!" + W + ")(?=" + W + "))";
                        i++;
                        continue;
                    }
                    break;
                case 'B':
                    if (!in_cc) {
                        result += std::string("(?:(?<=") + W + ")(?=" + W + ")|(?<!" + W + ")(?!" + W + "))";
                        i++;
                        continue;
                    }
                    break;
                default: break;
            }
            result += p[i++];
            result += p[i];
            continue;
        }
        if (p[i] == '[') cc_depth++;
        else if (p[i] == ']' && cc_depth > 0) cc_depth--;
        result += p[i];
    }
    return result;
}

// Under unicode+ignoreCase, \w includes chars whose case folding lands in
// [A-Za-z0-9_] (ſ, K). PCRE2's \w is a fixed ASCII table exempt from folding, so
// \w/\W/\b/\B are rewritten as classes within every i-effective scope.
static std::string expand_word_classes_unicode_i(const std::string& p, bool base_i) {
    static const char* W = "[A-Za-z0-9_]";
    std::string result;
    result.reserve(p.size() + 32);
    std::vector<bool> stk;
    bool cur_i = base_i;
    bool in_cc = false;
    for (size_t i = 0; i < p.size(); i++) {
        if (p[i] == '\\' && i + 1 < p.size()) {
            char n = p[i+1];
            if (cur_i && !in_cc && (n == 'w' || n == 'W' || n == 'b' || n == 'B')) {
                switch (n) {
                    case 'w': result += W; break;
                    case 'W': result += "[^A-Za-z0-9_]"; break;
                    case 'b':
                        result += std::string("(?:(?<=") + W + ")(?!" + W + ")|(?<!" + W + ")(?=" + W + "))";
                        break;
                    case 'B':
                        result += std::string("(?:(?<=") + W + ")(?=" + W + ")|(?<!" + W + ")(?!" + W + "))";
                        break;
                }
                i++;
                continue;
            }
            result += p[i++];
            result += p[i];
            continue;
        }
        if (p[i] == '[' && !in_cc) { in_cc = true; result += p[i]; continue; }
        if (p[i] == ']' && in_cc) { in_cc = false; result += p[i]; continue; }
        if (in_cc) { result += p[i]; continue; }
        if (p[i] == '(') {
            bool next_i = cur_i;
            if (i + 1 < p.size() && p[i+1] == '?') {
                // Modifier groups (?ims-ims: adjust the effective i state.
                size_t k = i + 2;
                bool add_i = false, rem_i = false, dash = false, is_mod = false;
                while (k < p.size()) {
                    char c = p[k];
                    if (c == ':') { is_mod = (k > i + 2); break; }
                    if (c == '-' && !dash) { dash = true; k++; continue; }
                    if (c == 'i' || c == 'm' || c == 's') {
                        if (c == 'i') (dash ? rem_i : add_i) = true;
                        k++;
                        continue;
                    }
                    break;
                }
                if (is_mod) {
                    if (add_i) next_i = true;
                    if (rem_i) next_i = false;
                }
            }
            stk.push_back(cur_i);
            cur_i = next_i;
            result += p[i];
            continue;
        }
        if (p[i] == ')' && !stk.empty()) {
            cur_i = stk.back();
            stk.pop_back();
            result += p[i];
            continue;
        }
        result += p[i];
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

// v-mode ClassSetExpression support (--, &&, \q{...}, properties of strings).
// PCRE2 has no native equivalent, so operands are modeled as literal code point
// strings plus a character class fragment, and set operations computed statically.
namespace {
struct VModeOperand {
    std::vector<std::vector<uint32_t>> strings; // multi-code-point sequences only
    std::vector<std::string> raw_alts;          // prebuilt one-character fragments
    std::string cc_text;                        // character class body (no brackets)
    bool has_chars = false;
};
}

static std::string vmode_escape_cp(uint32_t cp) {
    char buf[16];
    snprintf(buf, sizeof(buf), "\\x{%X}", cp);
    return buf;
}

static std::string vmode_string_pattern(const std::vector<uint32_t>& s) {
    std::string r;
    for (uint32_t cp : s) r += vmode_escape_cp(cp);
    return r;
}

// Character-only alternation of an operand (raw fragments and class, no strings).
// Every branch consumes exactly one code point. Empty when the operand has none.
static std::string vmode_char_alternation(const VModeOperand& op) {
    std::string r;
    for (auto& f : op.raw_alts) {
        if (!r.empty()) r += '|';
        r += f;
    }
    if (op.has_chars) {
        if (!r.empty()) r += '|';
        r += "[" + op.cc_text + "]";
    }
    return r;
}

// Joined alternation for an operand: longer strings first so alternation matching
// follows the spec's longest-string-first semantics, then the character branches.
static std::string vmode_alternation(const VModeOperand& op) {
    std::vector<std::vector<uint32_t>> sorted = op.strings;
    std::sort(sorted.begin(), sorted.end(),
              [](const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
                  return a.size() > b.size();
              });
    std::string r;
    for (auto& s : sorted) {
        if (!r.empty()) r += '|';
        r += vmode_string_pattern(s);
    }
    std::string chars = vmode_char_alternation(op);
    if (!chars.empty()) {
        if (!r.empty()) r += '|';
        r += chars;
    }
    if (r.empty()) r = "(?!)"; // empty set matches nothing
    return r;
}

static std::string vmode_consume(const VModeOperand& op) {
    return "(?:" + vmode_alternation(op) + ")";
}

// Decodes the text of one \q alternative (literal UTF-8 plus escapes emitted by
// convert_unicode_escapes) into a code point sequence.
static std::vector<uint32_t> vmode_decode_string(const std::string& s) {
    std::vector<uint32_t> cps;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\' && i + 1 < s.size()) {
            char n = s[i+1];
            if (n == 'x' && i + 2 < s.size() && s[i+2] == '{') {
                size_t end = s.find('}', i + 3);
                if (end != std::string::npos) {
                    cps.push_back(static_cast<uint32_t>(strtoul(s.substr(i+3, end-(i+3)).c_str(), nullptr, 16)));
                    i = end + 1;
                    continue;
                }
            }
            if (n == 'x' && i + 3 < s.size() && is_hex_ch(s[i+2]) && is_hex_ch(s[i+3])) {
                cps.push_back(static_cast<uint32_t>(strtoul(s.substr(i+2, 2).c_str(), nullptr, 16)));
                i += 4;
                continue;
            }
            switch (n) {
                case 'n': cps.push_back('\n'); break;
                case 'r': cps.push_back('\r'); break;
                case 't': cps.push_back('\t'); break;
                case 'f': cps.push_back('\f'); break;
                case 'v': cps.push_back('\v'); break;
                case '0': cps.push_back(0); break;
                default:  cps.push_back((unsigned char)n); break;
            }
            i += 2;
            continue;
        }
        if (c < 0x80) { cps.push_back(c); i++; continue; }
        size_t len = (c & 0xF8) == 0xF0 ? 4 : (c & 0xF0) == 0xE0 ? 3 : 2;
        uint32_t cp = len == 4 ? (c & 0x07) : len == 3 ? (c & 0x0F) : (c & 0x1F);
        for (size_t k = 1; k < len && i + k < s.size(); k++)
            cp = (cp << 6) | ((unsigned char)s[i+k] & 0x3F);
        cps.push_back(cp);
        i += len;
    }
    return cps;
}

static void vmode_add_string(VModeOperand& op, const std::vector<uint32_t>& cps) {
    if (cps.size() == 1) {
        // Single-code-point strings belong to the character set.
        op.cc_text += vmode_escape_cp(cps[0]);
        op.has_chars = true;
        return;
    }
    for (auto& s : op.strings) if (s == cps) return;
    op.strings.push_back(cps);
}

// Properties of strings with a bounded, hardcodable sequence list.
static const std::vector<std::vector<uint32_t>>* vmode_string_property_sequences(const std::string& name) {
    if (name == "Emoji_Keycap_Sequence") {
        static const std::vector<std::vector<uint32_t>> seqs = [] {
            std::vector<std::vector<uint32_t>> v;
            for (char c : std::string("#*0123456789"))
                v.push_back({static_cast<uint32_t>(c), 0xFE0F, 0x20E3});
            return v;
        }();
        return &seqs;
    }
    return nullptr;
}

static VModeOperand transform_vmode_class_body(const std::string& content);

// Parses one ClassSetOperand starting at content[i] and advances i past it.
static VModeOperand parse_vmode_operand(const std::string& content, size_t& i) {
    VModeOperand op;
    if (content[i] == '[') {
        size_t j = i + 1;
        int depth = 1;
        while (j < content.size() && depth > 0) {
            if (content[j] == '\\' && j + 1 < content.size()) { j += 2; continue; }
            if (content[j] == '[') { depth++; j++; continue; }
            if (content[j] == ']') { depth--; j++; continue; }
            j++;
        }
        std::string inner = content.substr(i + 1, (j - 1) - (i + 1));
        op = transform_vmode_class_body(inner);
        i = j;
        return op;
    }
    if (content[i] == '\\' && i + 1 < content.size() && content[i+1] == 'q' &&
        i + 2 < content.size() && content[i+2] == '{') {
        // Skip } inside \x{}/\p{}/\u{} so convert_unicode_escapes output doesn't fool us
        size_t end = content.size();
        for (size_t k = i + 3; k < content.size(); k++) {
            if (content[k] == '\\' && k + 1 < content.size()) {
                char nx = content[k+1];
                k += 2;
                if ((nx == 'x' || nx == 'p' || nx == 'P' || nx == 'u') && k < content.size() && content[k] == '{') {
                    size_t inner = content.find('}', k + 1);
                    k = (inner == std::string::npos) ? content.size() - 1 : inner;
                }
                continue;
            }
            if (content[k] == '}') { end = k; break; }
        }
        std::string body = content.substr(i + 3, end - (i + 3));
        size_t start = 0;
        for (size_t k = 0; k <= body.size(); k++) {
            if (k == body.size() || body[k] == '|') {
                vmode_add_string(op, vmode_decode_string(body.substr(start, k - start)));
                start = k + 1;
            }
        }
        i = end + 1;
        return op;
    }
    if (content[i] == '\\' && i + 1 < content.size()) {
        char nxt = content[i+1];
        size_t start = i;
        i += 2;
        if ((nxt == 'p' || nxt == 'P' || nxt == 'u') && i < content.size() && content[i] == '{') {
            size_t end = content.find('}', i + 1);
            i = (end == std::string::npos) ? content.size() : end + 1;
            if (nxt == 'p') {
                std::string prop = content.substr(start + 3, (i - 1) - (start + 3));
                const auto* seqs = vmode_string_property_sequences(prop);
                if (seqs) {
                    for (auto& s : *seqs) vmode_add_string(op, s);
                    return op;
                }
            }
        }
        op.cc_text = content.substr(start, i - start);
        op.has_chars = true;
        return op;
    }
    size_t start = i;
    i++;
    if (i + 1 < content.size() && content[i] == '-' && content[i+1] != '-') {
        i++;
        if (content[i] == '\\' && i + 1 < content.size()) {
            char nxt = content[i+1];
            i += 2;
            if ((nxt == 'p' || nxt == 'P' || nxt == 'u') && i < content.size() && content[i] == '{') {
                size_t end = content.find('}', i + 1);
                i = (end == std::string::npos) ? content.size() : end + 1;
            }
        } else {
            i++;
        }
    }
    op.cc_text = content.substr(start, i - start);
    op.has_chars = true;
    return op;
}

static bool vmode_contains_string(const VModeOperand& op, const std::vector<uint32_t>& s) {
    for (auto& t : op.strings) if (t == s) return true;
    return false;
}

// Transforms the body of a v-mode character class (the text between [ and ]).
static VModeOperand transform_vmode_class_body(const std::string& content) {
    bool has_diff = false, has_intersect = false;
    {
        int depth = 0;
        for (size_t i = 0; i < content.size(); i++) {
            if (content[i] == '\\' && i + 1 < content.size()) {
                if (content[i+1] == 'q' && i + 2 < content.size() && content[i+2] == '{') {
                    size_t end = content.find('}', i + 3);
                    i = (end == std::string::npos) ? content.size() : end;
                    continue;
                }
                i++;
                continue;
            }
            if (content[i] == '[') { depth++; continue; }
            if (content[i] == ']') { depth--; continue; }
            if (depth == 0 && i + 1 < content.size()) {
                if (content[i] == '-' && content[i+1] == '-') has_diff = true;
                if (content[i] == '&' && content[i+1] == '&') has_intersect = true;
            }
        }
    }

    if (has_diff || has_intersect) {
        char op_char = has_diff ? '-' : '&';
        std::vector<VModeOperand> operands;
        size_t i = 0;
        while (i < content.size()) {
            if (i + 1 < content.size() && content[i] == op_char && content[i+1] == op_char) {
                i += 2;
                continue;
            }
            operands.push_back(parse_vmode_operand(content, i));
        }
        VModeOperand result;
        if (operands.empty()) return result;

        // Multi-code-point strings: computed statically; a string is in the result
        // iff it is in the first operand and (for &&) in all others / (for --) in
        // none of the others.
        for (auto& s : operands[0].strings) {
            bool keep = true;
            for (size_t k = 1; k < operands.size(); k++) {
                bool in_k = vmode_contains_string(operands[k], s);
                if (has_diff ? in_k : !in_k) { keep = false; break; }
            }
            if (keep) result.strings.push_back(s);
        }

        // Character part: lookahead chain over one-code-point branches is exact
        // because every branch on both sides consumes exactly one code point.
        std::string base = vmode_char_alternation(operands[0]);
        if (!base.empty()) {
            std::string body;
            bool dead = false;
            for (size_t k = 1; k < operands.size(); k++) {
                std::string other = vmode_char_alternation(operands[k]);
                if (other.empty()) {
                    if (has_diff) continue;
                    dead = true; // intersection with a charless set has no chars
                    break;
                }
                body += (has_diff ? "(?!(?:" : "(?=(?:") + other + "))";
            }
            if (!dead) result.raw_alts.push_back(body + "(?:" + base + ")");
        }
        return result;
    }

    std::vector<VModeOperand> operands;
    size_t i = 0;
    while (i < content.size()) {
        operands.push_back(parse_vmode_operand(content, i));
    }
    VModeOperand result;
    for (auto& o : operands) {
        for (auto& s : o.strings) {
            if (!vmode_contains_string(result, s)) result.strings.push_back(s);
        }
        for (auto& f : o.raw_alts) result.raw_alts.push_back(f);
        if (o.has_chars) {
            result.cc_text += o.cc_text;
            result.has_chars = true;
        }
    }
    return result;
}

static std::string transform_v_mode_classes(const std::string& pattern) {
    std::string result;
    result.reserve(pattern.size());
    size_t i = 0;
    while (i < pattern.size()) {
        if (pattern[i] == '\\' && i + 1 < pattern.size()) {
            // Bare \p{StringProperty} outside a class expands to its sequences.
            if (pattern[i+1] == 'p' && i + 2 < pattern.size() && pattern[i+2] == '{') {
                size_t end = pattern.find('}', i + 3);
                if (end != std::string::npos) {
                    const auto* seqs = vmode_string_property_sequences(pattern.substr(i + 3, end - (i + 3)));
                    if (seqs) {
                        VModeOperand op;
                        for (auto& s : *seqs) vmode_add_string(op, s);
                        result += vmode_consume(op);
                        i = end + 1;
                        continue;
                    }
                }
            }
            result += pattern[i]; result += pattern[i+1];
            i += 2;
            continue;
        }
        if (pattern[i] == '[') {
            size_t j = i + 1;
            int depth = 1;
            bool negated = (j < pattern.size() && pattern[j] == '^');
            while (j < pattern.size() && depth > 0) {
                if (pattern[j] == '\\' && j + 1 < pattern.size()) { j += 2; continue; }
                if (pattern[j] == '[') { depth++; j++; continue; }
                if (pattern[j] == ']') { depth--; j++; continue; }
                j++;
            }
            size_t content_start = i + 1 + (negated ? 1 : 0);
            std::string inner = pattern.substr(content_start, (j - 1) - content_start);
            VModeOperand transformed = transform_vmode_class_body(inner);
            bool plain_class = transformed.strings.empty() && transformed.raw_alts.empty();
            if (plain_class) {
                result += negated ? "[^" : "[";
                result += transformed.cc_text;
                result += "]";
            } else {
                std::string alt = vmode_alternation(transformed);
                result += negated ? ("(?:(?!" + alt + ")[\\s\\S])") : ("(?:" + alt + ")");
            }
            i = j;
            continue;
        }
        result += pattern[i++];
    }
    return result;
}

// ES table-binary-unicode-properties (names and aliases) plus "Any"/"Assigned".
static bool is_binary_unicode_property(const std::string& n) {
    static const std::unordered_set<std::string> props = {
        "ASCII", "ASCII_Hex_Digit", "AHex", "Alphabetic", "Alpha", "Any", "Assigned",
        "Bidi_Control", "Bidi_C", "Bidi_Mirrored", "Bidi_M", "Case_Ignorable", "CI",
        "Cased", "Changes_When_Casefolded", "CWCF", "Changes_When_Casemapped", "CWCM",
        "Changes_When_Lowercased", "CWL", "Changes_When_NFKC_Casefolded", "CWKCF",
        "Changes_When_Titlecased", "CWT", "Changes_When_Uppercased", "CWU", "Dash",
        "Default_Ignorable_Code_Point", "DI", "Deprecated", "Dep", "Diacritic", "Dia",
        "Emoji", "Emoji_Component", "EComp", "Emoji_Modifier", "EMod",
        "Emoji_Modifier_Base", "EBase", "Emoji_Presentation", "EPres",
        "Extended_Pictographic", "ExtPict", "Extender", "Ext", "Grapheme_Base",
        "Gr_Base", "Grapheme_Extend", "Gr_Ext", "Hex_Digit", "Hex",
        "IDS_Binary_Operator", "IDSB", "IDS_Trinary_Operator", "IDST", "ID_Continue",
        "IDC", "ID_Start", "IDS", "Ideographic", "Ideo", "Join_Control", "Join_C",
        "Logical_Order_Exception", "LOE", "Lowercase", "Lower", "Math",
        "Noncharacter_Code_Point", "NChar", "Pattern_Syntax", "Pat_Syn",
        "Pattern_White_Space", "Pat_WS", "Quotation_Mark", "QMark", "Radical",
        "Regional_Indicator", "RI", "Sentence_Terminal", "STerm", "Soft_Dotted", "SD",
        "Terminal_Punctuation", "Term", "Unified_Ideograph", "UIdeo", "Uppercase",
        "Upper", "Variation_Selector", "VS", "White_Space", "space", "XID_Continue",
        "XIDC", "XID_Start", "XIDS",
    };
    return props.count(n) > 0;
}

// PropertyValueAliases for gc: short forms, long forms, and extra aliases.
static bool is_general_category_value(const std::string& n) {
    static const std::unordered_set<std::string> values = {
        "C", "Other", "Cc", "Control", "cntrl", "Cf", "Format", "Cn", "Unassigned",
        "Co", "Private_Use", "Cs", "Surrogate", "L", "Letter", "LC", "Cased_Letter",
        "Ll", "Lowercase_Letter", "Lm", "Modifier_Letter", "Lo", "Other_Letter", "Lt",
        "Titlecase_Letter", "Lu", "Uppercase_Letter", "M", "Mark", "Combining_Mark",
        "Mc", "Spacing_Mark", "Me", "Enclosing_Mark", "Mn", "Nonspacing_Mark", "N",
        "Number", "Nd", "Decimal_Number", "digit", "Nl", "Letter_Number", "No",
        "Other_Number", "P", "Punctuation", "punct", "Pc", "Connector_Punctuation",
        "Pd", "Dash_Punctuation", "Pe", "Close_Punctuation", "Pf", "Final_Punctuation",
        "Pi", "Initial_Punctuation", "Po", "Other_Punctuation", "Ps",
        "Open_Punctuation", "S", "Symbol", "Sc", "Currency_Symbol", "Sk",
        "Modifier_Symbol", "Sm", "Math_Symbol", "So", "Other_Symbol", "Z", "Separator",
        "Zl", "Line_Separator", "Zp", "Paragraph_Separator", "Zs", "Space_Separator",
    };
    return values.count(n) > 0;
}

static bool is_string_unicode_property(const std::string& n) {
    static const std::unordered_set<std::string> props = {
        "Basic_Emoji", "Emoji_Keycap_Sequence", "RGI_Emoji",
        "RGI_Emoji_Flag_Sequence", "RGI_Emoji_Modifier_Sequence",
        "RGI_Emoji_Tag_Sequence", "RGI_Emoji_ZWJ_Sequence",
    };
    return props.count(n) > 0;
}

// Strict UnicodePropertyValueExpression validation: PCRE2 does UAX44-LM3 loose
// matching (\p{lu}, \p{Any } etc.) which ES forbids.
static void validate_property_expression(const std::string& content, bool negated,
                                         bool v_mode, std::string& err) {
    size_t eq = content.find('=');
    if (eq != std::string::npos) {
        std::string name = content.substr(0, eq);
        std::string value = content.substr(eq + 1);
        if (name != "General_Category" && name != "gc" && name != "Script" &&
            name != "sc" && name != "Script_Extensions" && name != "scx") {
            err = "unknown property name '" + name + "'";
            return;
        }
        if (name == "General_Category" || name == "gc") {
            if (!is_general_category_value(value)) err = "unknown property value '" + value + "'";
            return;
        }
        // Script values are validated by PCRE2; only reject obviously malformed text.
        for (char c : value) {
            if (!std::isalnum((unsigned char)c) && c != '_') { err = "malformed script value"; return; }
        }
        if (value.empty()) err = "empty script value";
        return;
    }
    if (is_binary_unicode_property(content) || is_general_category_value(content)) return;
    if (is_string_unicode_property(content)) {
        if (!v_mode) err = "property of strings requires the v flag";
        else if (negated) err = "property of strings cannot be negated";
        return;
    }
    err = "unknown property '" + content + "'";
}

// Strict ES pattern grammar validation for /u and /v mode. PCRE2 accepts many
// constructs (\A, a{, [\1], (?=x)*, \p{lu}, ...) that are SyntaxErrors in ES
// unicode mode; this validator catches them before compilation.
namespace {
class UnicodePatternValidator {
public:
    UnicodePatternValidator(const std::string& p, bool v_mode, size_t n_groups, size_t n_named)
        : p_(p), v_(v_mode), n_groups_(n_groups), n_named_(n_named) {}

    void run() {
        parse_disjunction();
        if (i_ < p_.size()) fail("unmatched ')'");
    }

private:
    const std::string& p_;
    bool v_;
    size_t n_groups_;
    size_t n_named_;
    size_t i_ = 0;

    [[noreturn]] void fail(const std::string& msg) {
        throw std::runtime_error("Invalid regular expression: " + msg);
    }

    bool eof() const { return i_ >= p_.size(); }
    char cur() const { return p_[i_]; }
    char peek(size_t k = 1) const { return i_ + k < p_.size() ? p_[i_ + k] : '\0'; }

    bool parse_hex(size_t count, uint32_t& out) {
        uint32_t v = 0;
        for (size_t k = 0; k < count; k++) {
            if (eof() || !is_hex_ch(cur())) return false;
            char c = cur();
            v = v * 16 + (c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
            i_++;
        }
        out = v;
        return true;
    }

    // i_ points after 'u'
    void parse_unicode_escape() {
        uint32_t cp;
        if (!eof() && cur() == '{') {
            i_++;
            uint32_t v = 0;
            bool any = false;
            while (!eof() && cur() != '}') {
                if (!is_hex_ch(cur())) fail("invalid \\u{} escape");
                char c = cur();
                v = v * 16 + (c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
                if (v > 0x10FFFF) fail("\\u{} escape out of range");
                any = true;
                i_++;
            }
            if (eof() || !any) fail("invalid \\u{} escape");
            i_++;
            return;
        }
        if (!parse_hex(4, cp)) fail("invalid \\u escape");
    }

    // i_ points at the escape character after the backslash.
    void parse_shared_escape(bool in_class) {
        if (eof()) fail("pattern ends with \\");
        char c = cur();
        switch (c) {
            case 'f': case 'n': case 'r': case 't': case 'v':
            case 'd': case 'D': case 's': case 'S': case 'w': case 'W':
                i_++;
                return;
            case 'c':
                i_++;
                if (eof() || !std::isalpha((unsigned char)cur())) fail("invalid \\c escape");
                i_++;
                return;
            case 'x': {
                i_++;
                uint32_t v;
                if (!parse_hex(2, v)) fail("invalid \\x escape");
                return;
            }
            case 'u':
                i_++;
                parse_unicode_escape();
                return;
            case 'p': case 'P': {
                bool negated = (c == 'P');
                i_++;
                if (eof() || cur() != '{') fail("\\p must be followed by {Property}");
                size_t close = p_.find('}', i_ + 1);
                if (close == std::string::npos) fail("unterminated \\p{...}");
                std::string content = p_.substr(i_ + 1, close - (i_ + 1));
                std::string err;
                validate_property_expression(content, negated, v_, err);
                if (!err.empty()) fail(err);
                i_ = close + 1;
                return;
            }
            case '0':
                i_++;
                if (!eof() && std::isdigit((unsigned char)cur())) fail("invalid decimal escape");
                return;
            default:
                break;
        }
        if (!in_class && c >= '1' && c <= '9') {
            size_t n = 0;
            while (!eof() && std::isdigit((unsigned char)cur())) {
                n = n * 10 + (cur() - '0');
                i_++;
            }
            if (n > n_groups_) fail("invalid backreference");
            return;
        }
        if (!in_class && c == 'k') {
            i_++;
            if (eof() || cur() != '<') fail("invalid \\k escape");
            size_t close = p_.find('>', i_ + 1);
            if (close == std::string::npos) fail("unterminated \\k<");
            if (n_named_ == 0) fail("\\k reference with no named groups");
            i_ = close + 1;
            return;
        }
        // Identity escapes: SyntaxCharacter and '/'; in class also '-'.
        // c != '\0' guards: strchr(s, '\0') always "finds" s's own terminator.
        static const char* syntax_chars = "^$\\.*+?()[]{}|/";
        if ((c != '\0' && strchr(syntax_chars, c)) || (in_class && c == '-')) {
            i_++;
            return;
        }
        if (in_class && c == 'b') { i_++; return; }
        if (v_ && in_class && c != '\0' && strchr("&-!#%,:;<=>@`~", c)) { i_++; return; }
        fail(std::string("invalid escape \\") + c);
    }

    // Returns the quantifier presence; fails on malformed brace quantifiers.
    bool try_parse_quantifier() {
        if (eof()) return false;
        char c = cur();
        if (c == '*' || c == '+' || c == '?') {
            i_++;
            if (!eof() && cur() == '?') i_++;
            return true;
        }
        if (c == '{') {
            size_t j = i_ + 1;
            uint64_t lo = 0, hi = 0;
            bool has_lo = false, has_hi = false, comma = false;
            while (j < p_.size() && std::isdigit((unsigned char)p_[j])) {
                lo = lo * 10 + (p_[j] - '0');
                if (lo > 0xFFFFFFFFull) lo = 0xFFFFFFFFull;
                has_lo = true;
                j++;
            }
            if (j < p_.size() && p_[j] == ',') {
                comma = true;
                j++;
                while (j < p_.size() && std::isdigit((unsigned char)p_[j])) {
                    hi = hi * 10 + (p_[j] - '0');
                    if (hi > 0xFFFFFFFFull) hi = 0xFFFFFFFFull;
                    has_hi = true;
                    j++;
                }
            }
            if (!has_lo || j >= p_.size() || p_[j] != '}')
                fail("incomplete quantifier");
            if (comma && has_hi && lo > hi) fail("quantifier range out of order");
            i_ = j + 1;
            if (!eof() && cur() == '?') i_++;
            return true;
        }
        return false;
    }

    void parse_class_atom(bool& is_class_escape, uint32_t& cp) {
        is_class_escape = false;
        cp = 0;
        if (cur() == '\\') {
            char c = peek();
            i_++;
            if (c == 'd' || c == 'D' || c == 's' || c == 'S' || c == 'w' || c == 'W' ||
                c == 'p' || c == 'P') {
                is_class_escape = true;
            }
            parse_shared_escape(true);
            return;
        }
        unsigned char c = (unsigned char)cur();
        if (c < 0x80) { cp = c; i_++; return; }
        // Decode UTF-8 code point for range-order checks.
        size_t len = (c & 0xF8) == 0xF0 ? 4 : (c & 0xF0) == 0xE0 ? 3 : 2;
        cp = 0;
        switch (len) {
            case 2: cp = c & 0x1F; break;
            case 3: cp = c & 0x0F; break;
            default: cp = c & 0x07; break;
        }
        for (size_t k = 1; k < len && i_ + k < p_.size(); k++)
            cp = (cp << 6) | ((unsigned char)p_[i_ + k] & 0x3F);
        i_ += len;
    }

    void parse_character_class() {
        i_++;
        if (!eof() && cur() == '^') i_++;
        if (v_) {
            // v-mode class-set syntax: allow nesting, \q{...}, -- and && operators.
            // Structural checks live in the parser; only balance and escapes here.
            int depth = 1;
            while (!eof() && depth > 0) {
                if (cur() == '\\') {
                    char c = peek();
                    if (c == 'q') {
                        i_ += 2;
                        if (!eof() && cur() == '{') {
                            size_t close = p_.find('}', i_ + 1);
                            if (close == std::string::npos) fail("unterminated \\q{...}");
                            i_ = close + 1;
                        } else {
                            fail("\\q must be followed by {...}");
                        }
                        continue;
                    }
                    i_++;
                    parse_shared_escape(true);
                    continue;
                }
                if (cur() == '[') { depth++; i_++; continue; }
                if (cur() == ']') { depth--; i_++; continue; }
                i_++;
            }
            if (depth > 0) fail("unterminated character class");
            return;
        }
        while (!eof() && cur() != ']') {
            bool lhs_is_escape = false;
            uint32_t lo = 0;
            parse_class_atom(lhs_is_escape, lo);
            if (!eof() && cur() == '-' && peek() != ']' && i_ + 1 < p_.size()) {
                i_++;
                bool rhs_is_escape = false;
                uint32_t hi = 0;
                parse_class_atom(rhs_is_escape, hi);
                if (lhs_is_escape || rhs_is_escape) fail("invalid character class range");
                if (lo != 0 && hi != 0 && lo > hi) fail("character class range out of order");
            }
        }
        if (eof()) fail("unterminated character class");
        i_++;
    }

    void parse_group() {
        bool quantifiable = true;
        if (peek() == '?') {
            char c2 = peek(2);
            if (c2 == '=' || c2 == '!') {
                i_ += 3;
                quantifiable = false;
            } else if (c2 == '<' && (peek(3) == '=' || peek(3) == '!')) {
                i_ += 4;
                quantifiable = false;
            } else if (c2 == '<') {
                size_t close = p_.find('>', i_ + 3);
                if (close == std::string::npos) fail("unterminated capture group name");
                i_ = close + 1;
            } else if (c2 == ':') {
                i_ += 3;
            } else {
                // (?ims-ims: modifier group, already validated by validate_js_modifiers
                size_t colon = i_ + 2;
                while (colon < p_.size() && p_[colon] != ':' && p_[colon] != ')') colon++;
                if (colon >= p_.size() || p_[colon] != ':') fail("invalid group");
                i_ = colon + 1;
            }
        } else {
            i_++;
        }
        parse_disjunction();
        if (eof() || cur() != ')') fail("unterminated group");
        i_++;
        bool has_quant = try_parse_quantifier();
        if (has_quant && !quantifiable) fail("quantifier after assertion");
    }

    void parse_term() {
        char c = cur();
        if (c == '^' || c == '$') {
            i_++;
            if (try_parse_quantifier()) fail("quantifier after assertion");
            return;
        }
        if (c == '\\' && (peek() == 'b' || peek() == 'B')) {
            i_ += 2;
            if (try_parse_quantifier()) fail("quantifier after assertion");
            return;
        }
        if (c == '(') {
            parse_group();
            return;
        }
        if (c == '[') {
            parse_character_class();
            try_parse_quantifier();
            return;
        }
        if (c == '*' || c == '+' || c == '?') fail("nothing to repeat");
        if (c == '{') {
            size_t save = i_;
            bool valid = false;
            try { valid = try_parse_quantifier(); } catch (...) { valid = false; }
            i_ = save;
            fail(valid ? "nothing to repeat" : "lone quantifier bracket");
        }
        if (c == '}' || c == ']') fail("lone quantifier bracket");
        if (c == '\\') {
            i_++;
            parse_shared_escape(false);
            try_parse_quantifier();
            return;
        }
        // Literal character (any code point, including '.')
        unsigned char uc = (unsigned char)c;
        i_ += uc < 0x80 ? 1 : (uc & 0xF8) == 0xF0 ? 4 : (uc & 0xF0) == 0xE0 ? 3 : 2;
        try_parse_quantifier();
    }

    void parse_disjunction() {
        while (true) {
            while (!eof() && cur() != '|' && cur() != ')') parse_term();
            if (!eof() && cur() == '|') { i_++; continue; }
            return;
        }
    }
};
} // namespace

// Counts capture groups outside character classes.
static size_t count_capture_groups(const std::string& p) {
    size_t n = 0;
    int cc_depth = 0;
    for (size_t i = 0; i < p.size(); i++) {
        if (p[i] == '\\') { i++; continue; }
        if (p[i] == '[') { cc_depth++; continue; }
        if (p[i] == ']') { if (cc_depth > 0) cc_depth--; continue; }
        if (cc_depth > 0 || p[i] != '(') continue;
        if (i + 1 < p.size() && p[i+1] == '?') {
            if (i + 2 < p.size() && p[i+2] == '<' &&
                i + 3 < p.size() && p[i+3] != '=' && p[i+3] != '!') n++;
        } else {
            n++;
        }
    }
    return n;
}

static void validate_js_modifiers(const std::string& pat) {
    bool in_class = false;
    bool esc = false;
    for (size_t i = 0; i < pat.size(); i++) {
        unsigned char c = (unsigned char)pat[i];
        if (esc) { esc = false; continue; }
        if (c == '\\') { esc = true; continue; }
        if (c == '[' && !in_class) { in_class = true; continue; }
        if (!in_class && (c == '*' || c == '+' || c == '?') &&
            i + 1 < pat.size() && (unsigned char)pat[i+1] == '+')
            throw std::runtime_error("Invalid regular expression: nothing to repeat");
        if (c == ']' && in_class) { in_class = false; continue; }
        if (in_class) continue;
        if (c != '(' || i + 1 >= pat.size() || (unsigned char)pat[i+1] != '?') continue;

        size_t j = i + 2;
        bool has_dash = false;
        std::string add_flags, rem_flags;
        bool is_modifier = false;

        while (j < pat.size()) {
            unsigned char b = (unsigned char)pat[j];
            if (b == ':') { is_modifier = true; break; }
            if (b == '-' && !has_dash) { has_dash = true; j++; continue; }
            if ((b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') ||
                (b >= '0' && b <= '9') || b == '_' || b == '$') {
                (has_dash ? rem_flags : add_flags) += (char)b;
                j++;
                continue;
            }
            if (b >= 0x80) {
                (has_dash ? rem_flags : add_flags) += '\x01';
                j++;
                while (j < pat.size() && ((unsigned char)pat[j] & 0xC0) == 0x80) j++;
                continue;
            }
            break;
        }

        if (!is_modifier) continue;

        if (has_dash && add_flags.empty() && rem_flags.empty())
            throw std::runtime_error("Regexp modifier: both add and remove sets are empty");

        auto validate_flags = [](const std::string& flags) {
            bool seen[3] = {};
            for (unsigned char fc : flags) {
                int idx = (fc == 'i') ? 0 : (fc == 'm') ? 1 : (fc == 's') ? 2 : -1;
                if (idx < 0) throw std::runtime_error("Invalid character in regexp modifier flags");
                if (seen[idx]) throw std::runtime_error("Duplicate flag in regexp modifier");
                seen[idx] = true;
            }
        };
        validate_flags(add_flags);
        validate_flags(rem_flags);

        if (has_dash) {
            for (char ac : add_flags)
                for (char rc : rem_flags)
                    if (ac == rc) throw std::runtime_error("Same flag in both add and remove of regexp modifier");
        }
    }
}

void RegExp::do_compile() {
    std::string cache_key;
    if (!g_regexp_validation_only) {
        cache_key = pattern_;
        cache_key.push_back('\0');
        cache_key += flags_;
        auto it = g_regex_cache.find(cache_key);
        if (it != g_regex_cache.end()) {
            code_owner_ = it->second.code;
            code_ = code_owner_.get();
            named_groups_ = it->second.named_groups;
            backtrack_engine_ = it->second.backtrack_engine;
            return;
        }
    }
    code_owner_.reset();
    code_ = nullptr;

    validate_js_modifiers(pattern_);

    backtrack_engine_.reset();
    if (RegexBacktrackEngine::pattern_needs_backtrack_engine(pattern_)) {
        try {
            backtrack_engine_ = std::make_shared<RegexBacktrackEngine>(pattern_, ignore_case_, multiline_, dotall_, unicode_);
        } catch (...) {
            backtrack_engine_.reset(); // unsupported syntax; keep the PCRE2 (possibly imperfect) result
        }
    }

    // Trailing backslash is a SyntaxError in every mode; PCRE2's
    // BAD_ESCAPE_IS_LITERAL would otherwise swallow it.
    {
        size_t bs = 0;
        while (bs < pattern_.size() && pattern_[pattern_.size() - 1 - bs] == '\\') bs++;
        if (bs % 2 == 1) throw std::runtime_error("Invalid regular expression: pattern ends with \\");
    }

    std::string base = rename_named_groups(pattern_);
    if (unicode_) {
        UnicodePatternValidator(base, unicode_sets_, count_capture_groups(base), named_groups_.size()).run();
    }

    std::string pat = unicode_
        ? convert_unicode_escapes(expand_gc_aliases(expand_word_classes_unicode_i(expand_js_charclass_shortcuts(base, true), ignore_case_)), true)
        : convert_unicode_escapes(expand_ascii_word_classes(expand_js_charclass_shortcuts(preprocess_pattern(base), false)), false);
    // Only /u-mode PCRE2_UTF rejects unpaired surrogate units in the pattern; in
    // 16-bit code-unit mode they are ordinary units.
    if (unicode_) pat = normalize_u_surrogate_escapes(escape_wtf8_surrogates(pat));
    // v-mode class-set expressions are flattened before the passes below so they
    // only ever see PCRE2-style flat character classes.
    if (unicode_sets_) pat = transform_v_mode_classes(pat);
    pat = escape_posix_lookalikes(pat);
    pat = discard_empty_optional_groups(pat);
    pat = fix_optional_backrefs(adjust_dot(wrap_lookaheads_atomic(bound_lookbehind_quantifiers(pat)), dotall_));

    // PCRE2_MATCH_UNSET_BACKREF: ES spec 15.10.2.9 says a backreference to an unset
    // capture group matches empty string ("return c(x)"). Baked in at compile time so
    // JIT is aware of it (JIT rejects this option at match time).
    // PCRE2_DOLLAR_ENDONLY: JS non-multiline $ never matches before a final newline.
    uint32_t options = PCRE2_DUPNAMES | PCRE2_MATCH_UNSET_BACKREF | PCRE2_DOLLAR_ENDONLY;
    if (unicode_) {
        // UTF-16 code point matching. PCRE2_UCP intentionally absent: it would extend
        // \d/\w to Unicode categories, but JS requires ASCII sets.
        options |= PCRE2_UTF;
    } else {
        // Code-unit matching (JS non-unicode semantics: '.' = one UTF-16 unit, astral
        // chars are surrogate pairs). PCRE2_UCP provides full-range caseless folding
        // and \p{} support; \d/\w/\b were already rewritten to ASCII sets above.
        options |= PCRE2_UCP;
    }
    if (ignore_case_) options |= PCRE2_CASELESS;
    if (multiline_)  options |= PCRE2_MULTILINE;
    if (dotall_)     options |= PCRE2_DOTALL;

    int errcode = 0;
    PCRE2_SIZE erroffset = 0;

    pcre2_compile_context* cctx = pcre2_compile_context_create(nullptr);
    if (cctx) {
        // Non-unicode Canonicalize has an ASCII barrier (ToUpperCase must not map a
        // non-ASCII char into ASCII): CASELESS_RESTRICT implements exactly that.
        uint32_t extra = unicode_ ? 0 : (PCRE2_EXTRA_BAD_ESCAPE_IS_LITERAL | PCRE2_EXTRA_CASELESS_RESTRICT);
        pcre2_set_compile_extra_options(cctx, extra);
        pcre2_set_max_varlookbehind(cctx, UINT32_MAX);
    }

    std::u16string pat16 = wtf8_to_utf16(pat);
    pcre2_code* re = pcre2_compile(
        reinterpret_cast<PCRE2_SPTR>(pat16.c_str()),
        pat16.size(),
        options,
        &errcode,
        &erroffset,
        cctx
    );

    // MATCH_UNSET_BACKREF makes PCRE2 reject any lookbehind with a backreference (error 125); retry without it.
    if (!re && errcode == 125 && (options & PCRE2_MATCH_UNSET_BACKREF)) {
        re = pcre2_compile(
            reinterpret_cast<PCRE2_SPTR>(pat16.c_str()),
            pat16.size(),
            options & ~PCRE2_MATCH_UNSET_BACKREF,
            &errcode,
            &erroffset,
            cctx
        );
    }

    if (cctx) pcre2_compile_context_free(cctx);

    if (!re) {
        // JS-valid but PCRE2-unsupported: huge quantifier (105), empty [] (106),
        // variable lookbehind (125), \u in PCRE2 ctx (137), string properties (147).
        if (errcode == 105 || errcode == 106 || errcode == 125 || errcode == 137 || errcode == 147 || errcode == 187) {
            static const char16_t always_fail[] = u"(?!)";
            re = pcre2_compile(
                reinterpret_cast<PCRE2_SPTR>(always_fail),
                4, 0, &errcode, &erroffset, nullptr
            );
        } else {
            PCRE2_UCHAR errbuf[256] = {};
            pcre2_get_error_message(errcode, errbuf, 256);
            std::string msg;
            for (size_t k = 0; k < 256 && errbuf[k]; k++) msg += static_cast<char>(errbuf[k]);
            throw std::runtime_error("Invalid regular expression: " + msg);
        }
    } else if (!g_regexp_validation_only) {
        pcre2_jit_compile(re, PCRE2_JIT_COMPLETE | PCRE2_JIT_PARTIAL_SOFT);
    }

    code_owner_ = std::shared_ptr<void>(re, [](void* p) { pcre2_code_free(static_cast<pcre2_code*>(p)); });
    code_ = code_owner_.get();

    if (!g_regexp_validation_only) {
        if (g_regex_cache.size() >= kRegexCacheCap) g_regex_cache.clear();
        g_regex_cache[cache_key] = {code_owner_, named_groups_, backtrack_engine_};
    }
}

// In /u mode PCRE2_UTF validates the subject, so lone surrogate units are
// replaced with U+FFFD (unit count preserved).
static void sanitize_utf16_surrogates(std::u16string& s) {
    for (size_t i = 0; i < s.size(); i++) {
        char16_t c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size() &&
            s[i+1] >= 0xDC00 && s[i+1] <= 0xDFFF) {
            i++;
            continue;
        }
        if (c >= 0xD800 && c <= 0xDFFF) s[i] = u'�';
    }
}

bool RegExp::test(const std::string& str) {
    std::u16string subject = wtf8_to_utf16(str);
    if (unicode_) sanitize_utf16_surrogates(subject);

    size_t start = 0;
    if ((global_ || sticky_) && last_index_ > 0) {
        if (static_cast<size_t>(last_index_) > subject.size()) {
            last_index_ = 0;
            return false;
        }
        start = static_cast<size_t>(last_index_);
    }

    bool found = false;
    size_t match_end = 0;

    if (backtrack_engine_) {
        BacktrackMatch bm;
        found = backtrack_engine_->exec(subject, start, sticky_, bm);
        if (found) match_end = bm.end;
    } else {
        if (!code_) return false;
        pcre2_code* re = static_cast<pcre2_code*>(code_);
        pcre2_match_data* md = pcre2_match_data_create_from_pattern(re, nullptr);
        if (!md) return false;

        int rc = pcre2_match(re,
            reinterpret_cast<PCRE2_SPTR>(subject.c_str()), subject.size(),
            static_cast<PCRE2_SIZE>(start), 0, md, nullptr);

        found = (rc >= 0);
        if (found) {
            PCRE2_SIZE* ov = pcre2_get_ovector_pointer(md);
            if (sticky_ && ov[0] != start) found = false;
            else match_end = ov[1];
        }
        pcre2_match_data_free(md);
    }

    if (found && (global_ || sticky_)) {
        last_index_ = static_cast<int>(match_end);
    } else if (!found && (global_ || sticky_)) {
        last_index_ = 0;
    }
    return found;
}

Value RegExp::exec(const std::string& str) {
    // orig holds the unsanitized units so capture text preserves lone surrogates.
    std::u16string orig = wtf8_to_utf16(str);
    std::u16string subject = orig;
    if (unicode_) sanitize_utf16_surrogates(subject);

    bool advances = global_ || sticky_;
    size_t start = 0;
    if (advances && last_index_ > 0) {
        if (static_cast<size_t>(last_index_) > subject.size()) {
            last_index_ = 0;
            return Value::null();
        }
        start = static_cast<size_t>(last_index_);
    }

    uint32_t capture_count = 0;
    // PCRE2_UNSET convention shared by both backends so result-building below is unchanged.
    std::vector<PCRE2_SIZE> saved;
    size_t match_start = 0, match_end = 0;
    bool found = false;

    if (backtrack_engine_) {
        BacktrackMatch bm;
        found = backtrack_engine_->exec(subject, start, sticky_, bm);
        if (found) {
            capture_count = backtrack_engine_->capture_count();
            match_start = bm.start;
            match_end = bm.end;
            saved.assign((capture_count + 1) * 2, PCRE2_UNSET);
            saved[0] = match_start;
            saved[1] = match_end;
            for (uint32_t g = 1; g <= capture_count; g++) {
                if (bm.captures[g].first < 0) continue;
                saved[2 * g] = static_cast<PCRE2_SIZE>(bm.captures[g].first);
                saved[2 * g + 1] = static_cast<PCRE2_SIZE>(bm.captures[g].second);
            }
        }
    } else {
        if (!code_) return Value::null();
        pcre2_code* re = static_cast<pcre2_code*>(code_);
        pcre2_match_data* md = pcre2_match_data_create_from_pattern(re, nullptr);
        if (!md) return Value::null();

        int rc = pcre2_match(re,
            reinterpret_cast<PCRE2_SPTR>(subject.c_str()), subject.size(),
            static_cast<PCRE2_SIZE>(start), 0, md, nullptr);

        if (rc >= 0) {
            PCRE2_SIZE* ov = pcre2_get_ovector_pointer(md);
            if (!(sticky_ && ov[0] != start)) {
                found = true;
                pcre2_pattern_info(re, PCRE2_INFO_CAPTURECOUNT, &capture_count);
                match_start = ov[0];
                match_end = ov[1];
                saved.assign(ov, ov + (capture_count + 1) * 2);
                reset_stale_captures(pattern_, saved, capture_count);
            }
        }
        pcre2_match_data_free(md);
    }

    if (!found) {
        if (advances) last_index_ = 0;
        return Value::null();
    }
    if (advances) last_index_ = static_cast<int>(match_end);

    auto slice = [&](size_t from, size_t to) -> std::string {
        return utf16_to_wtf8(orig.data() + from, to - from);
    };

    // ArrayCreate(n): the match result is a genuine Array (RegExpBuiltinExec step 24), not a plain object.
    auto result_owner = ObjectFactory::create_array(capture_count + 1);
    Object* result = result_owner.get();
    result->set_element(0, Value(slice(match_start, match_end)));
    result->set_property("index", Value(static_cast<double>(match_start)));
    result->set_property("input", Value(str));

    for (uint32_t i = 1; i <= capture_count; ++i) {
        if (saved[2 * i] == PCRE2_UNSET)
            result->set_element(i, Value());
        else
            result->set_element(i, Value(slice(saved[2*i], saved[2*i+1])));
    }

    // Resolve the matched capture number for a named group (duplicates share a name).
    auto matched_capture = [&](const std::vector<uint32_t>& nums) -> int {
        for (uint32_t gn : nums) {
            if (gn <= capture_count && saved[2*gn] != PCRE2_UNSET) return static_cast<int>(gn);
        }
        return -1;
    };

    if (!named_groups_.empty()) {
        // ObjectCreate(null): groups dict has null prototype per spec; property order
        // follows source order of first group occurrence.
        auto groups_owner = ObjectFactory::create_object();
        groups_owner->set_prototype(nullptr);
        for (const auto& ng : named_groups_) {
            int gn = matched_capture(ng.second);
            Value gval = gn >= 0 ? Value(slice(saved[2*gn], saved[2*gn+1])) : Value();
            groups_owner->set_property_descriptor(ng.first, PropertyDescriptor(gval, PropertyAttributes::Default));
        }
        // CreateDataProperty: define directly on A, bypassing any inherited setter on Array.prototype["groups"].
        result->set_property_descriptor("groups", PropertyDescriptor(Value(groups_owner.release()), PropertyAttributes::Default));
    } else {
        result->set_property_descriptor("groups", PropertyDescriptor(Value(), PropertyAttributes::Default));
    }

    if (has_indices_) {
        // MakeMatchIndicesIndexPairArray: per-capture [start, end] in JS indices.
        auto indices_owner = ObjectFactory::create_array(capture_count + 1);
        auto make_pair = [&](size_t from, size_t to) -> Value {
            auto pair = ObjectFactory::create_array(2);
            pair->set_element(0, Value(static_cast<double>(from)));
            pair->set_element(1, Value(static_cast<double>(to)));
            return Value(pair.release());
        };
        for (uint32_t i = 0; i <= capture_count; ++i) {
            if (saved[2*i] == PCRE2_UNSET) indices_owner->set_element(i, Value());
            else indices_owner->set_element(i, make_pair(saved[2*i], saved[2*i+1]));
        }
        if (!named_groups_.empty()) {
            auto igroups = ObjectFactory::create_object();
            igroups->set_prototype(nullptr);
            for (const auto& ng : named_groups_) {
                int gn = matched_capture(ng.second);
                Value pv = gn >= 0 ? make_pair(saved[2*gn], saved[2*gn+1]) : Value();
                igroups->set_property_descriptor(ng.first, PropertyDescriptor(pv, PropertyAttributes::Default));
            }
            indices_owner->set_property_descriptor("groups", PropertyDescriptor(Value(igroups.release()), PropertyAttributes::Default));
        } else {
            indices_owner->set_property_descriptor("groups", PropertyDescriptor(Value(), PropertyAttributes::Default));
        }
        result->set_property_descriptor("indices", PropertyDescriptor(Value(indices_owner.release()), PropertyAttributes::Default));
    }

    return Value(result_owner.release());
}

void RegExp::compile(const std::string& pattern, const std::string& flags) {
    pattern_ = pattern;
    flags_ = flags;
    global_ = ignore_case_ = multiline_ = unicode_ = sticky_ = dotall_ = unicode_sets_ = has_indices_ = false;
    last_index_ = 0;
    parse_flags(flags_);
    do_compile();
}

std::string RegExp::to_string() const {
    return "/" + pattern_ + "/" + flags_;
}

bool RegExp::is_valid_unicode_pattern(const std::string& pattern, const std::string& flags) {
    // Delegate to the real compilation pipeline so literal validation and
    // RegExp-constructor validation can never disagree. JIT is skipped: the probe
    // never matches anything.
    g_regexp_validation_only = true;
    bool ok = true;
    try {
        RegExp probe(pattern, flags);
    } catch (...) {
        ok = false;
    }
    g_regexp_validation_only = false;
    return ok;
}

}
