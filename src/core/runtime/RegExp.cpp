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

// Replace WTF-8 lone surrogate sequences (ED [A0-BF] [80-BF]) with U+FFFD (EF BF BD).
// Both are 3 bytes so byte offsets are preserved; PCRE2 rejects raw surrogate bytes.
// Fast-path: if the string contains no 0xED byte, no surrogates -> return original.
static const std::string& sanitize_wtf8(const std::string& s, std::string& buf) {
    if (s.find('\xED') == std::string::npos) return s;
    buf = s;
    for (size_t i = 0; i + 2 < buf.size(); ) {
        unsigned char c = static_cast<unsigned char>(buf[i]);
        if (c == 0xED) {
            unsigned char c1 = static_cast<unsigned char>(buf[i+1]);
            if (c1 >= 0xA0 && c1 <= 0xBF) {
                buf[i]   = static_cast<char>(0xEF);
                buf[i+1] = static_cast<char>(0xBF);
                buf[i+2] = static_cast<char>(0xBD);
                i += 3;
                continue;
            }
        }
        if (c < 0x80) i += 1;
        else if ((c & 0xE0) == 0xC0) i += 2;
        else if ((c & 0xF0) == 0xE0) i += 3;
        else i += 4;
    }
    return buf;
}

// Convert UTF-8 byte offset to JS string index (UTF-16 code units).
// 4-byte UTF-8 sequences (U+10000..U+10FFFF) become surrogate pairs = 2 JS chars.
static size_t byte_to_js_index(const std::string& s, size_t byte_off) {
    size_t b = 0, js = 0;
    while (b < byte_off && b < s.size()) {
        unsigned char c = (unsigned char)s[b];
        if (c < 0x80)       { b += 1; js += 1; }
        else if (c < 0xE0)  { b += 2; js += 1; }
        else if (c < 0xF0)  { b += 3; js += 1; }
        else                 { b += 4; js += 2; }
    }
    return js;
}

// Convert JS string index (UTF-16 code units) to UTF-8 byte offset.
static size_t js_index_to_byte(const std::string& s, size_t js_idx) {
    size_t b = 0, js = 0;
    while (b < s.size() && js < js_idx) {
        unsigned char c = (unsigned char)s[b];
        if (c < 0x80)       { b += 1; js += 1; }
        else if (c < 0xE0)  { b += 2; js += 1; }
        else if (c < 0xF0)  { b += 3; js += 1; }
        else                 { b += 4; js += 2; }
    }
    return b;
}

RegExp::RegExp(const std::string& pattern, const std::string& flags)
    : pattern_(pattern), flags_(flags), code_(nullptr),
      global_(false), ignore_case_(false), multiline_(false),
      unicode_(false), sticky_(false), dotall_(false), unicode_sets_(false), last_index_(0) {
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
    bool seen[8] = {};
    for (char flag : flags) {
        int idx = -1;
        switch (flag) {
            case 'd':                        idx = 0; break;
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

// JS spec: \d = [0-9], \w = [A-Za-z0-9_], \s = specific Unicode whitespace list.
// PCRE2_UCP extends these to full Unicode categories which is wrong per spec.
// We remove UCP and expand \s manually; \d/\w revert to ASCII-only automatically.
static std::string expand_js_charclass_shortcuts(const std::string& p) {
    // JS \s whitespace — exactly the set from ECMAScript spec (WhiteSpace + LineTerminator)
    static const char* s_inner  = "\\t\\n\\x0B\\f\\r\\x20\\x{00A0}\\x{1680}\\x{2000}-\\x{200A}\\x{2028}\\x{2029}\\x{202F}\\x{205F}\\x{3000}\\x{FEFF}";
    static const char* s_outer  = "[\\t\\n\\x0B\\f\\r\\x20\\x{00A0}\\x{1680}\\x{2000}-\\x{200A}\\x{2028}\\x{2029}\\x{202F}\\x{205F}\\x{3000}\\x{FEFF}]";
    static const char* S_outer  = "[^\\t\\n\\x0B\\f\\r\\x20\\x{00A0}\\x{1680}\\x{2000}-\\x{200A}\\x{2028}\\x{2029}\\x{202F}\\x{205F}\\x{3000}\\x{FEFF}]";
    // For \S inside a character class we'd need set subtraction (not standard in /u mode);
    // in practice \S inside [...] is rare, leave it as-is and let PCRE2 handle it.

    std::string result;
    result.reserve(p.size() * 2);
    bool in_cc = false;
    for (size_t i = 0; i < p.size(); i++) {
        if (p[i] == '\\' && i + 1 < p.size()) {
            char next = p[i + 1];
            if (next == 's') {
                result += in_cc ? s_inner : s_outer;
                i++;
                continue;
            }
            if (next == 'S' && !in_cc) {
                result += S_outer;
                i++;
                continue;
            }
            result += p[i++];
            result += p[i];
        } else if (p[i] == '[' && !in_cc) {
            if (i + 2 < p.size() && p[i+1] == '^' && p[i+2] == ']') {
                result += "[\\s\\S]";
                i += 2;
                continue;
            }
            in_cc = true;
            result += p[i];
        } else if (p[i] == ']' && in_cc) {
            in_cc = false;
            result += p[i];
        } else {
            result += p[i];
        }
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

// v-mode ClassSetExpression support (--, &&, nested-class union, \q{...}). PCRE2 has no
// native equivalent, so each operand becomes a lookahead/alternation fragment instead.
namespace {
struct VModeOperand {
    bool is_simple = true;
    std::string cc_text;
    std::vector<std::string> alts;
};
}

static std::string vmode_alt_join(const std::vector<std::string>& alts) {
    std::string r;
    for (size_t i = 0; i < alts.size(); i++) {
        if (i) r += '|';
        r += alts[i];
    }
    return r;
}

static std::string vmode_consume(const VModeOperand& op) {
    return op.is_simple ? ("[" + op.cc_text + "]") : ("(?:" + vmode_alt_join(op.alts) + ")");
}
static std::string vmode_pos_lookahead(const VModeOperand& op) {
    return op.is_simple ? ("(?=[" + op.cc_text + "])") : ("(?=" + vmode_alt_join(op.alts) + ")");
}
static std::string vmode_neg_lookahead(const VModeOperand& op) {
    return op.is_simple ? ("(?![" + op.cc_text + "])") : ("(?!" + vmode_alt_join(op.alts) + ")");
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
        op.is_simple = false;
        size_t start = 0;
        for (size_t k = 0; k <= body.size(); k++) {
            if (k == body.size() || body[k] == '|') {
                op.alts.push_back(body.substr(start, k - start));
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
        }
        op.cc_text = content.substr(start, i - start);
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
    return op;
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
        result.is_simple = false;
        if (!operands.empty()) {
            std::string body = vmode_pos_lookahead(operands[0]);
            for (size_t k = 1; k < operands.size(); k++) {
                body += has_diff ? vmode_neg_lookahead(operands[k]) : vmode_pos_lookahead(operands[k]);
            }
            body += vmode_consume(operands[0]);
            result.alts.push_back("(?:" + body + ")");
        }
        return result;
    }

    std::vector<VModeOperand> operands;
    size_t i = 0;
    while (i < content.size()) {
        operands.push_back(parse_vmode_operand(content, i));
    }
    bool all_simple = true;
    for (auto& o : operands) if (!o.is_simple) { all_simple = false; break; }
    VModeOperand result;
    if (all_simple) {
        for (auto& o : operands) result.cc_text += o.cc_text;
        return result;
    }
    result.is_simple = false;
    for (auto& o : operands) {
        if (o.is_simple) result.alts.push_back("[" + o.cc_text + "]");
        else for (auto& a : o.alts) result.alts.push_back(a);
    }
    return result;
}

static std::string transform_v_mode_classes(const std::string& pattern) {
    std::string result;
    result.reserve(pattern.size());
    size_t i = 0;
    while (i < pattern.size()) {
        if (pattern[i] == '\\' && i + 1 < pattern.size()) {
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
            if (transformed.is_simple) {
                result += negated ? "[^" : "[";
                result += transformed.cc_text;
                result += "]";
            } else {
                std::string alt = vmode_alt_join(transformed.alts);
                result += negated ? ("(?:(?!" + alt + ")[\\s\\S])") : ("(?:" + alt + ")");
            }
            i = j;
            continue;
        }
        result += pattern[i++];
    }
    return result;
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
    if (code_) {
        pcre2_code_free(static_cast<pcre2_code*>(code_));
        code_ = nullptr;
    }

    validate_js_modifiers(pattern_);

    std::string pat = unicode_
        ? fix_optional_backrefs(convert_unicode_escapes(expand_gc_aliases(expand_js_charclass_shortcuts(pattern_))))
        : fix_optional_backrefs(convert_unicode_escapes(preprocess_pattern(pattern_)));

    if (unicode_sets_) pat = transform_v_mode_classes(pat);

    // PCRE2_MATCH_UNSET_BACKREF: ES spec 15.10.2.9 says a backreference to an unset
    // capture group matches empty string ("return c(x)"). Baked in at compile time so
    // JIT is aware of it (JIT rejects this option at match time).
    uint32_t options = PCRE2_UTF | PCRE2_DUPNAMES | PCRE2_MATCH_UNSET_BACKREF;
    if (unicode_) {
        // PCRE2_UCP intentionally absent: it would extend \d/\w to Unicode categories,
        // but JS spec requires \d=[0-9] and \w=[A-Za-z0-9_] (ASCII only). \s is handled
        // by expand_js_charclass_shortcuts above. \p{} still works via PCRE2_UTF.
    }
    if (ignore_case_) options |= PCRE2_CASELESS;
    if (multiline_)  options |= PCRE2_MULTILINE;
    if (dotall_)     options |= PCRE2_DOTALL;

    int errcode = 0;
    PCRE2_SIZE erroffset = 0;

    // Lone surrogates (e.g. \uDC00) are valid in JS regex source but PCRE2_UTF rejects
    // them by default, silently falling through to the always-fail "(?!)" below.
    pcre2_compile_context* cctx = pcre2_compile_context_create(nullptr);
    if (cctx) {
        uint32_t extra = PCRE2_EXTRA_ALLOW_SURROGATE_ESCAPES;
        if (!unicode_) extra |= PCRE2_EXTRA_BAD_ESCAPE_IS_LITERAL;
        pcre2_set_compile_extra_options(cctx, extra);
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
        // JS-valid but PCRE2-unsupported: huge quantifier (105), empty [] (106),
        // variable lookbehind (125), \u in PCRE2 ctx (137), string properties (147).
        if (errcode == 105 || errcode == 106 || errcode == 125 || errcode == 137 || errcode == 147) {
            re = pcre2_compile(
                reinterpret_cast<PCRE2_SPTR>("(?!)"),
                PCRE2_ZERO_TERMINATED, 0, &errcode, &erroffset, nullptr
            );
        } else {
            PCRE2_UCHAR8 errbuf[256] = {};
            pcre2_get_error_message(errcode, errbuf, sizeof(errbuf));
            throw std::runtime_error(std::string("Invalid regular expression: ") + reinterpret_cast<const char*>(errbuf));
        }
    } else {
        pcre2_jit_compile(re, PCRE2_JIT_COMPLETE | PCRE2_JIT_PARTIAL_SOFT);
    }

    code_ = re;
}

bool RegExp::test(const std::string& str) {
    if (!code_) return false;

    std::string _wtf8_buf; const std::string& subject = unicode_ ? sanitize_wtf8(str, _wtf8_buf) : str;
    pcre2_code* re = static_cast<pcre2_code*>(code_);
    pcre2_match_data* md = pcre2_match_data_create_from_pattern(re, nullptr);
    if (!md) return false;

    PCRE2_SIZE start = 0;
    if ((global_ || sticky_) && last_index_ > 0) {
        size_t js_len = byte_to_js_index(subject, subject.length());
        if (static_cast<size_t>(last_index_) > js_len) {
            last_index_ = 0;
            pcre2_match_data_free(md);
            return false;
        }
        start = static_cast<PCRE2_SIZE>(js_index_to_byte(subject, static_cast<size_t>(last_index_)));
    }

    int rc = pcre2_match(re,
        reinterpret_cast<PCRE2_SPTR>(subject.c_str()), subject.length(),
        start, 0, md, nullptr);

    bool found = (rc >= 0);
    if (found && sticky_) {
        PCRE2_SIZE match_start = pcre2_get_ovector_pointer(md)[0];
        if (match_start != start) found = false;
    }
    if (found && (global_ || sticky_)) {
        last_index_ = static_cast<int>(byte_to_js_index(subject, pcre2_get_ovector_pointer(md)[1]));
    } else if (!found && (global_ || sticky_)) {
        last_index_ = 0;
    }

    pcre2_match_data_free(md);
    return found;
}

Value RegExp::exec(const std::string& str) {
    if (!code_) return Value::null();

    std::string _wtf8_buf; const std::string& subject = unicode_ ? sanitize_wtf8(str, _wtf8_buf) : str;
    pcre2_code* re = static_cast<pcre2_code*>(code_);
    pcre2_match_data* md = pcre2_match_data_create_from_pattern(re, nullptr);
    if (!md) return Value::null();

    bool advances = global_ || sticky_;
    PCRE2_SIZE start = 0;
    if (advances && last_index_ > 0) {
        size_t js_len = byte_to_js_index(subject, subject.length());
        if (static_cast<size_t>(last_index_) > js_len) {
            last_index_ = 0;
            pcre2_match_data_free(md);
            return Value::null();
        }
        start = static_cast<PCRE2_SIZE>(js_index_to_byte(subject, static_cast<size_t>(last_index_)));
    }

    int rc = pcre2_match(re,
        reinterpret_cast<PCRE2_SPTR>(subject.c_str()), subject.length(),
        start, 0, md, nullptr);

    if (rc < 0) {
        if (advances) last_index_ = 0;
        pcre2_match_data_free(md);
        return Value::null();
    }

    PCRE2_SIZE* ov = pcre2_get_ovector_pointer(md);

    if (sticky_ && ov[0] != start) {
        if (advances) last_index_ = 0;
        pcre2_match_data_free(md);
        return Value::null();
    }

    uint32_t capture_count = 0;
    pcre2_pattern_info(re, PCRE2_INFO_CAPTURECOUNT, &capture_count);

    size_t match_start = ov[0];
    size_t match_end   = ov[1];
    std::vector<PCRE2_SIZE> saved(ov, ov + (capture_count + 1) * 2);

    size_t match_start_js = byte_to_js_index(subject, match_start);
    size_t match_end_js   = byte_to_js_index(subject, match_end);

    if (advances) last_index_ = static_cast<int>(match_end_js);
    pcre2_match_data_free(md);

    // ArrayCreate(n): the match result is a genuine Array (RegExpBuiltinExec step 24), not a plain object.
    auto result_owner = ObjectFactory::create_array(capture_count + 1);
    Object* result = result_owner.get();
    result->set_element(0, Value(str.substr(match_start, match_end - match_start)));
    result->set_property("index", Value(static_cast<double>(match_start_js)));
    result->set_property("input", Value(str));

    for (uint32_t i = 1; i <= capture_count; ++i) {
        if (saved[2 * i] == PCRE2_UNSET)
            result->set_element(i, Value());
        else
            result->set_element(i,
                Value(str.substr(saved[2*i], saved[2*i+1] - saved[2*i])));
    }

    uint32_t name_count = 0;
    pcre2_pattern_info(re, PCRE2_INFO_NAMECOUNT, &name_count);
    if (name_count > 0) {
        uint32_t entry_size = 0;
        PCRE2_SPTR name_table = nullptr;
        pcre2_pattern_info(re, PCRE2_INFO_NAMEENTRYSIZE, &entry_size);
        pcre2_pattern_info(re, PCRE2_INFO_NAMETABLE, &name_table);

        // ObjectCreate(null): groups dict has null prototype per spec.
        auto groups_owner = ObjectFactory::create_object();
        groups_owner->set_prototype(nullptr);
        const unsigned char* tbl = reinterpret_cast<const unsigned char*>(name_table);
        for (uint32_t i = 0; i < name_count; ++i) {
            const unsigned char* e = tbl + i * entry_size;
            uint32_t gn = (e[0] << 8) | e[1];
            const char* name = reinterpret_cast<const char*>(e + 2);
            bool matched = (gn <= capture_count && saved[2*gn] != PCRE2_UNSET);
            Value gval = matched ? Value(str.substr(saved[2*gn], saved[2*gn+1] - saved[2*gn])) : Value();
            // With PCRE2_DUPNAMES, prefer the matched entry over an already-set undefined.
            Value existing = groups_owner->get_own_property(name);
            if (matched || existing.is_undefined())
                groups_owner->set_property_descriptor(name, PropertyDescriptor(gval, PropertyAttributes::Default));
        }
        // CreateDataProperty: define directly on A, bypassing any inherited setter on Array.prototype["groups"].
        result->set_property_descriptor("groups", PropertyDescriptor(Value(groups_owner.release()), PropertyAttributes::Default));
    } else {
        result->set_property_descriptor("groups", PropertyDescriptor(Value(), PropertyAttributes::Default));
    }

    return Value(result_owner.release());
}

void RegExp::compile(const std::string& pattern, const std::string& flags) {
    pattern_ = pattern;
    flags_ = flags;
    global_ = ignore_case_ = multiline_ = unicode_ = sticky_ = dotall_ = unicode_sets_ = false;
    last_index_ = 0;
    parse_flags(flags_);
    do_compile();
}

std::string RegExp::to_string() const {
    return "/" + pattern_ + "/" + flags_;
}

bool RegExp::is_valid_unicode_pattern(const std::string& pattern, const std::string& flags) {
    bool has_u = flags.find('u') != std::string::npos;
    if (!has_u) return true;

    std::string pat = fix_optional_backrefs(convert_unicode_escapes(expand_gc_aliases(pattern)));
    uint32_t options = PCRE2_UTF | PCRE2_UCP;
    int errcode = 0;
    PCRE2_SIZE erroffset = 0;
    pcre2_compile_context* cctx = pcre2_compile_context_create(nullptr);
    if (cctx) pcre2_set_compile_extra_options(cctx, PCRE2_EXTRA_ALLOW_SURROGATE_ESCAPES);
    pcre2_code* re = pcre2_compile(
        reinterpret_cast<PCRE2_SPTR>(pat.c_str()),
        PCRE2_ZERO_TERMINATED,
        options, &errcode, &erroffset, cctx
    );
    if (cctx) pcre2_compile_context_free(cctx);
    if (!re) return false;
    pcre2_code_free(re);
    return true;
}

}
