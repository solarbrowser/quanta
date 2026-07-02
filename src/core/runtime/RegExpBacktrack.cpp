/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/runtime/RegExpBacktrack.h"
#include "quanta/core/runtime/RegExp.h"
#include "utf8proc.h"
#include <cctype>
#include <functional>
#include <unordered_map>

namespace Quanta {

namespace {


bool is_line_terminator(uint32_t cp) {
    return cp == '\n' || cp == '\r' || cp == 0x2028 || cp == 0x2029;
}

bool is_word_char(uint32_t cp) {
    return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9') || cp == '_';
}

// Approximate case fold (not full Unicode Canonicalize); fine for this rare fallback path.
uint32_t fold_cp(uint32_t cp) {
    if (cp < 128) return (cp >= 'A' && cp <= 'Z') ? cp + 32 : cp;
    return static_cast<uint32_t>(utf8proc_tolower(static_cast<utf8proc_int32_t>(cp)));
}

bool codepoint_equals(uint32_t a, uint32_t b, bool ignore_case) {
    if (a == b) return true;
    if (!ignore_case) return false;
    return fold_cp(a) == fold_cp(b);
}

bool is_hex_digit(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}

// Decodes one WTF-8 code point at byte offset i, advancing i past it.
uint32_t decode_wtf8_cp(const std::string& s, size_t& i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) { i += 1; return c; }
    size_t len = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
    if (i + len > s.size()) { i = s.size(); return 0xFFFD; }
    uint32_t cp = len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
    for (size_t k = 1; k < len; k++) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    i += len;
    return cp;
}

using Range = std::pair<uint32_t, uint32_t>;

const std::vector<Range>& digit_ranges() {
    static const std::vector<Range> r = {{'0', '9'}};
    return r;
}
const std::vector<Range>& word_ranges() {
    static const std::vector<Range> r = {{'0', '9'}, {'A', 'Z'}, {'_', '_'}, {'a', 'z'}};
    return r;
}
// ECMAScript WhiteSpace + LineTerminator set.
const std::vector<Range>& space_ranges() {
    static const std::vector<Range> r = {
        {0x09, 0x0D}, {0x20, 0x20}, {0xA0, 0xA0}, {0x1680, 0x1680}, {0x2000, 0x200A},
        {0x2028, 0x2029}, {0x202F, 0x202F}, {0x205F, 0x205F}, {0x3000, 0x3000}, {0xFEFF, 0xFEFF},
    };
    return r;
}


struct MatcherContext {
    const std::vector<uint32_t>& atoms;
    bool ignore_case;
    bool multiline;
    bool dot_all;
    std::vector<std::pair<int64_t, int64_t>> captures; // index 0 unused
};

using Cont = std::function<bool(int64_t)>;

struct Node {
    virtual ~Node() = default;
    virtual bool match(MatcherContext& ctx, int64_t pos, int dir, const Cont& k) const = 0;
};

struct SequenceNode : Node {
    std::vector<std::unique_ptr<Node>> children;
    bool match(MatcherContext& ctx, int64_t pos, int dir, const Cont& k) const override {
        return match_at(dir == 1 ? 0 : static_cast<int64_t>(children.size()) - 1, ctx, pos, dir, k);
    }
    bool match_at(int64_t idx, MatcherContext& ctx, int64_t pos, int dir, const Cont& k) const {
        if (idx < 0 || idx >= static_cast<int64_t>(children.size())) return k(pos);
        int64_t next = idx + dir;
        const Node* child = children[static_cast<size_t>(idx)].get();
        return child->match(ctx, pos, dir, [this, next, dir, &ctx, &k](int64_t p2) {
            return match_at(next, ctx, p2, dir, k);
        });
    }
};

struct AlternationNode : Node {
    std::vector<std::unique_ptr<Node>> branches;
    bool match(MatcherContext& ctx, int64_t pos, int dir, const Cont& k) const override {
        for (auto& b : branches) {
            auto snapshot = ctx.captures;
            if (b->match(ctx, pos, dir, k)) return true;
            ctx.captures = snapshot;
        }
        return false;
    }
};

struct LiteralNode : Node {
    uint32_t cp;
    bool ignore_case;
    bool match(MatcherContext& ctx, int64_t pos, int dir, const Cont& k) const override {
        if (dir == 1) { if (pos >= static_cast<int64_t>(ctx.atoms.size())) return false; }
        else { if (pos <= 0) return false; }
        uint32_t atom = ctx.atoms[static_cast<size_t>(dir == 1 ? pos : pos - 1)];
        if (!codepoint_equals(atom, cp, ignore_case)) return false;
        return k(pos + dir);
    }
};

struct AnyCharNode : Node {
    bool dot_all;
    bool match(MatcherContext& ctx, int64_t pos, int dir, const Cont& k) const override {
        if (dir == 1) { if (pos >= static_cast<int64_t>(ctx.atoms.size())) return false; }
        else { if (pos <= 0) return false; }
        uint32_t atom = ctx.atoms[static_cast<size_t>(dir == 1 ? pos : pos - 1)];
        if (!dot_all && is_line_terminator(atom)) return false;
        return k(pos + dir);
    }
};

struct CharClassNode : Node {
    bool negated = false;
    bool ignore_case = false;
    std::vector<Range> ranges;
    bool match(MatcherContext& ctx, int64_t pos, int dir, const Cont& k) const override {
        if (dir == 1) { if (pos >= static_cast<int64_t>(ctx.atoms.size())) return false; }
        else { if (pos <= 0) return false; }
        uint32_t atom = ctx.atoms[static_cast<size_t>(dir == 1 ? pos : pos - 1)];
        bool in = contains(atom);
        if (ignore_case && in == negated) {
            // Retry with folded forms so e.g. [a-z] matches 'A' under /i.
            uint32_t f = fold_cp(atom);
            if (f != atom) in = contains(f);
        }
        if (in == negated) return false;
        return k(pos + dir);
    }
    bool contains(uint32_t atom) const {
        for (auto& r : ranges) if (atom >= r.first && atom <= r.second) return true;
        return false;
    }
};

struct AssertionNode : Node {
    enum Kind { Start, End, WordB, NotWordB } kind;
    bool multiline;
    bool match(MatcherContext& ctx, int64_t pos, int dir, const Cont& k) const override {
        (void)dir;
        bool ok = false;
        int64_t len = static_cast<int64_t>(ctx.atoms.size());
        switch (kind) {
            case Start:
                ok = (pos == 0) || (multiline && pos > 0 && is_line_terminator(ctx.atoms[static_cast<size_t>(pos - 1)]));
                break;
            case End:
                ok = (pos == len) || (multiline && pos < len && is_line_terminator(ctx.atoms[static_cast<size_t>(pos)]));
                break;
            case WordB:
            case NotWordB: {
                bool before = pos > 0 && is_word_char(ctx.atoms[static_cast<size_t>(pos - 1)]);
                bool after = pos < len && is_word_char(ctx.atoms[static_cast<size_t>(pos)]);
                bool boundary = before != after;
                ok = (kind == WordB) ? boundary : !boundary;
                break;
            }
        }
        if (!ok) return false;
        return k(pos);
    }
};

struct GroupNode : Node {
    int index = 0; // 0 = non-capturing
    std::unique_ptr<Node> child;
    bool match(MatcherContext& ctx, int64_t pos, int dir, const Cont& k) const override {
        if (index == 0) return child->match(ctx, pos, dir, k);
        int idx = index;
        return child->match(ctx, pos, dir, [idx, pos, &ctx, &k](int64_t p2) -> bool {
            auto saved = ctx.captures[static_cast<size_t>(idx)];
            ctx.captures[static_cast<size_t>(idx)] = {std::min(pos, p2), std::max(pos, p2)};
            if (k(p2)) return true;
            ctx.captures[static_cast<size_t>(idx)] = saved;
            return false;
        });
    }
};

struct BackrefNode : Node {
    std::vector<int> indices; // usually one; multiple for duplicate-named groups
    bool ignore_case = false;
    bool match(MatcherContext& ctx, int64_t pos, int dir, const Cont& k) const override {
        std::pair<int64_t, int64_t> cap = {-1, -1};
        for (int idx : indices) {
            auto c = ctx.captures[static_cast<size_t>(idx)];
            if (c.first >= 0) { cap = c; break; }
        }
        if (cap.first < 0) return k(pos); // unset backreference matches empty
        int64_t len = cap.second - cap.first;
        if (dir == 1) {
            if (pos + len > static_cast<int64_t>(ctx.atoms.size())) return false;
            for (int64_t i = 0; i < len; i++)
                if (!codepoint_equals(ctx.atoms[static_cast<size_t>(pos + i)], ctx.atoms[static_cast<size_t>(cap.first + i)], ignore_case))
                    return false;
            return k(pos + len);
        }
        if (pos - len < 0) return false;
        for (int64_t i = 0; i < len; i++)
            if (!codepoint_equals(ctx.atoms[static_cast<size_t>(pos - len + i)], ctx.atoms[static_cast<size_t>(cap.first + i)], ignore_case))
                return false;
        return k(pos - len);
    }
};

// Direction-aware RepeatMatcher (ECMA-262 22.2.2.5).
bool repeat_matcher(const Node* m, int min, int max, bool greedy, MatcherContext& ctx, int64_t x, int dir,
                     const Cont& c, int paren_index, int paren_count) {
    if (max == 0) return c(x);
    Cont d = [m, min, max, greedy, dir, &ctx, &c, x, paren_index, paren_count](int64_t y) -> bool {
        if (min == 0 && y == x) return false; // guard against an infinite empty-match loop
        int min2 = min > 0 ? min - 1 : 0;
        int max2 = (max < 0) ? -1 : max - 1;
        return repeat_matcher(m, min2, max2, greedy, ctx, y, dir, c, paren_index, paren_count);
    };
    // Clears this iteration's captures before attempting it; restores on failure.
    auto try_one_more = [&]() -> bool {
        auto entry = ctx.captures;
        for (int i = 0; i < paren_count; i++) ctx.captures[static_cast<size_t>(paren_index + i)] = {-1, -1};
        bool r = m->match(ctx, x, dir, d);
        if (!r) ctx.captures = entry;
        return r;
    };
    if (min != 0) return try_one_more();
    if (!greedy) {
        if (c(x)) return true;
        return try_one_more();
    }
    if (try_one_more()) return true;
    return c(x);
}

struct QuantifierNode : Node {
    std::unique_ptr<Node> child;
    int min = 0, max = -1;
    bool greedy = true;
    int paren_index = 0, paren_count = 0;
    bool match(MatcherContext& ctx, int64_t pos, int dir, const Cont& k) const override {
        return repeat_matcher(child.get(), min, max, greedy, ctx, pos, dir, k, paren_index, paren_count);
    }
};

// Body continuation is an unconditional accept (not `k`), making this atomic.
struct LookaroundNode : Node {
    std::unique_ptr<Node> child;
    bool negate = false;
    bool behind = false;
    bool match(MatcherContext& ctx, int64_t pos, int dir, const Cont& k) const override {
        (void)dir;
        int inner_dir = behind ? -1 : 1;
        auto snapshot = ctx.captures;
        Cont accept = [](int64_t) { return true; };
        bool matched = child->match(ctx, pos, inner_dir, accept);
        if (negate) {
            ctx.captures = snapshot;
            if (matched) return false;
            return k(pos);
        }
        if (!matched) { ctx.captures = snapshot; return false; }
        if (k(pos)) return true;
        ctx.captures = snapshot;
        return false;
    }
};


struct UnsupportedSyntax : std::runtime_error {
    UnsupportedSyntax(const std::string& m) : std::runtime_error(m) {}
};

class Parser {
public:
    Parser(const std::string& pat, bool unicode) : pat_(pat), unicode_(unicode) {
        group_count_ = count_capture_groups(pat);
    }

    std::unique_ptr<Node> parse(std::unordered_map<std::string, std::vector<int>>& names_out) {
        auto root = parse_disjunction();
        if (i_ != pat_.size()) throw UnsupportedSyntax("trailing content");
        names_out = names_;
        return root;
    }

    uint32_t group_count() const { return group_count_; }

private:
    const std::string& pat_;
    bool unicode_;
    size_t i_ = 0;
    uint32_t group_count_ = 0;
    uint32_t next_group_ = 1;
    std::unordered_map<std::string, std::vector<int>> names_;

    static uint32_t count_capture_groups(const std::string& p) {
        uint32_t n = 0;
        int cc = 0;
        for (size_t i = 0; i < p.size(); i++) {
            if (p[i] == '\\') { i++; continue; }
            if (p[i] == '[') { cc++; continue; }
            if (p[i] == ']') { if (cc > 0) cc--; continue; }
            if (cc > 0 || p[i] != '(') continue;
            if (i + 1 < p.size() && p[i + 1] == '?') {
                if (i + 2 < p.size() && p[i + 2] == '<' && i + 3 < p.size() && p[i + 3] != '=' && p[i + 3] != '!') n++;
            } else {
                n++;
            }
        }
        return n;
    }

    bool eof() const { return i_ >= pat_.size(); }
    char cur() const { return pat_[i_]; }
    char peek(size_t k = 1) const { return i_ + k < pat_.size() ? pat_[i_ + k] : '\0'; }

    std::unique_ptr<Node> parse_disjunction() {
        auto alt = std::make_unique<AlternationNode>();
        alt->branches.push_back(parse_alternative());
        while (!eof() && cur() == '|') {
            i_++;
            alt->branches.push_back(parse_alternative());
        }
        if (alt->branches.size() == 1) return std::move(alt->branches[0]);
        return alt;
    }

    std::unique_ptr<Node> parse_alternative() {
        auto seq = std::make_unique<SequenceNode>();
        while (!eof() && cur() != '|' && cur() != ')') seq->children.push_back(parse_term());
        return seq;
    }

    std::unique_ptr<Node> parse_term() {
        auto atom = parse_assertion_or_atom();
        if (dynamic_cast<AssertionNode*>(atom.get()) || dynamic_cast<LookaroundNode*>(atom.get())) {
            // Only lookaround is quantifiable per grammar.
            if (dynamic_cast<LookaroundNode*>(atom.get())) {
                int pi = 0, pc = 0;
                if (try_parse_quantifier(atom, pi, pc)) throw UnsupportedSyntax("quantified lookaround");
            }
            return atom;
        }
        int paren_index = 0, paren_count = 0;
        find_paren_range(atom.get(), paren_index, paren_count);
        auto q = std::make_unique<QuantifierNode>();
        if (!try_parse_quantifier_into(q.get())) return atom;
        q->child = std::move(atom);
        q->paren_index = paren_index;
        q->paren_count = paren_count;
        return q;
    }

    static void find_paren_range(const Node* n, int& first, int& count) {
        // Capture-index span the atom owns, for RepeatMatcher's per-cycle reset.
        std::vector<int> idxs;
        collect_indices(n, idxs);
        if (idxs.empty()) { first = 0; count = 0; return; }
        int lo = idxs[0], hi = idxs[0];
        for (int v : idxs) { lo = std::min(lo, v); hi = std::max(hi, v); }
        first = lo;
        count = hi - lo + 1;
    }
    static void collect_indices(const Node* n, std::vector<int>& out) {
        if (auto* g = dynamic_cast<const GroupNode*>(n)) {
            if (g->index != 0) out.push_back(g->index);
            collect_indices(g->child.get(), out);
        } else if (auto* s = dynamic_cast<const SequenceNode*>(n)) {
            for (auto& c : s->children) collect_indices(c.get(), out);
        } else if (auto* a = dynamic_cast<const AlternationNode*>(n)) {
            for (auto& b : a->branches) collect_indices(b.get(), out);
        } else if (auto* q = dynamic_cast<const QuantifierNode*>(n)) {
            collect_indices(q->child.get(), out);
        } else if (auto* l = dynamic_cast<const LookaroundNode*>(n)) {
            collect_indices(l->child.get(), out);
        }
    }

    bool try_parse_quantifier(std::unique_ptr<Node>&, int&, int&) {
        size_t save = i_;
        auto dummy = std::make_unique<QuantifierNode>();
        bool has = try_parse_quantifier_into(dummy.get());
        if (!has) i_ = save;
        return has;
    }

    bool try_parse_quantifier_into(QuantifierNode* q) {
        if (eof()) return false;
        char c = cur();
        if (c == '*') { q->min = 0; q->max = -1; i_++; }
        else if (c == '+') { q->min = 1; q->max = -1; i_++; }
        else if (c == '?') { q->min = 0; q->max = 1; i_++; }
        else if (c == '{') {
            size_t save = i_;
            size_t j = i_ + 1;
            long lo = 0, hi = -1;
            bool has_lo = false, has_comma = false, has_hi = false;
            while (j < pat_.size() && isdigit(static_cast<unsigned char>(pat_[j]))) { lo = lo * 10 + (pat_[j] - '0'); has_lo = true; j++; }
            if (j < pat_.size() && pat_[j] == ',') {
                has_comma = true;
                j++;
                long h = 0;
                while (j < pat_.size() && isdigit(static_cast<unsigned char>(pat_[j]))) { h = h * 10 + (pat_[j] - '0'); has_hi = true; j++; }
                hi = has_hi ? h : -1;
            }
            if (!has_lo || j >= pat_.size() || pat_[j] != '}') { i_ = save; return false; }
            q->min = static_cast<int>(lo);
            q->max = has_comma ? static_cast<int>(hi) : static_cast<int>(lo);
            i_ = j + 1;
        } else {
            return false;
        }
        if (!eof() && cur() == '?') { q->greedy = false; i_++; } else { q->greedy = true; }
        return true;
    }

    std::unique_ptr<Node> parse_assertion_or_atom() {
        char c = cur();
        if (c == '^') { i_++; auto a = std::make_unique<AssertionNode>(); a->kind = AssertionNode::Start; a->multiline = multiline_; return a; }
        if (c == '$') { i_++; auto a = std::make_unique<AssertionNode>(); a->kind = AssertionNode::End; a->multiline = multiline_; return a; }
        if (c == '\\' && (peek() == 'b' || peek() == 'B')) {
            bool neg = peek() == 'B';
            i_ += 2;
            auto a = std::make_unique<AssertionNode>();
            a->kind = neg ? AssertionNode::NotWordB : AssertionNode::WordB;
            a->multiline = multiline_;
            return a;
        }
        if (c == '(') return parse_group();
        if (c == '[') return parse_char_class();
        if (c == '.') { i_++; auto n = std::make_unique<AnyCharNode>(); n->dot_all = dot_all_; return n; }
        if (c == '\\') return parse_atom_escape();
        if (c == '*' || c == '+' || c == '?') throw UnsupportedSyntax("nothing to repeat");
        auto n = std::make_unique<LiteralNode>();
        n->cp = next_cp();
        n->ignore_case = ignore_case_;
        return n;
    }

    uint32_t next_cp() {
        return decode_wtf8_cp(pat_, i_);
    }

    std::unique_ptr<Node> parse_group() {
        i_++; // (
        if (!eof() && cur() == '?') {
            char c2 = peek();
            if (c2 == '=' || c2 == '!') {
                i_ += 2;
                auto la = std::make_unique<LookaroundNode>();
                la->behind = false;
                la->negate = (c2 == '!');
                la->child = parse_disjunction();
                expect(')');
                return la;
            }
            if (c2 == '<' && (peek(2) == '=' || peek(2) == '!')) {
                bool neg = peek(2) == '!';
                i_ += 3;
                auto lb = std::make_unique<LookaroundNode>();
                lb->behind = true;
                lb->negate = neg;
                lb->child = parse_disjunction();
                expect(')');
                return lb;
            }
            if (c2 == ':') {
                i_ += 2;
                auto g = std::make_unique<GroupNode>();
                g->index = 0;
                g->child = parse_disjunction();
                expect(')');
                return g;
            }
            if (c2 == '<') {
                i_ += 2;
                size_t close = pat_.find('>', i_);
                if (close == std::string::npos) throw UnsupportedSyntax("unterminated group name");
                std::string name = pat_.substr(i_, close - i_);
                i_ = close + 1;
                auto g = std::make_unique<GroupNode>();
                g->index = static_cast<int>(next_group_++);
                names_[name].push_back(g->index);
                g->child = parse_disjunction();
                expect(')');
                return g;
            }
            throw UnsupportedSyntax("inline modifier group");
        }
        auto g = std::make_unique<GroupNode>();
        g->index = static_cast<int>(next_group_++);
        g->child = parse_disjunction();
        expect(')');
        return g;
    }

    void expect(char c) {
        if (eof() || cur() != c) throw UnsupportedSyntax(std::string("expected '") + c + "'");
        i_++;
    }

    std::unique_ptr<Node> parse_atom_escape() {
        i_++; // backslash
        if (eof()) throw UnsupportedSyntax("trailing backslash");
        char c = cur();
        if (c >= '1' && c <= '9') {
            size_t j = i_;
            int n = 0;
            while (j < pat_.size() && isdigit(static_cast<unsigned char>(pat_[j]))) { n = n * 10 + (pat_[j] - '0'); j++; }
            i_ = j;
            if (n > static_cast<int>(group_count_)) throw UnsupportedSyntax("invalid backreference");
            auto b = std::make_unique<BackrefNode>();
            b->indices = {n};
            b->ignore_case = ignore_case_;
            return b;
        }
        if (c == 'k' && peek() == '<') {
            i_ += 2;
            size_t close = pat_.find('>', i_);
            if (close == std::string::npos) throw UnsupportedSyntax("unterminated named backreference");
            std::string name = pat_.substr(i_, close - i_);
            i_ = close + 1;
            auto it = names_.find(name);
            auto b = std::make_unique<BackrefNode>();
            b->indices = it != names_.end() ? it->second : std::vector<int>{};
            if (b->indices.empty()) throw UnsupportedSyntax("unknown named backreference");
            b->ignore_case = ignore_case_;
            return b;
        }
        if (c == 'd' || c == 'D' || c == 'w' || c == 'W' || c == 's' || c == 'S') {
            i_++;
            auto n = std::make_unique<CharClassNode>();
            n->ignore_case = ignore_case_;
            n->negated = isupper(static_cast<unsigned char>(c));
            n->ranges = (c == 'd' || c == 'D') ? digit_ranges() : (c == 'w' || c == 'W') ? word_ranges() : space_ranges();
            return n;
        }
        if (c == 'p' || c == 'P') throw UnsupportedSyntax("unicode property escape");
        auto n = std::make_unique<LiteralNode>();
        n->cp = parse_character_escape();
        n->ignore_case = ignore_case_;
        return n;
    }

    // CharacterEscape; caller has ruled out backreference/class-escape forms.
    uint32_t parse_character_escape() {
        char c = cur();
        switch (c) {
            case 'n': i_++; return '\n';
            case 'r': i_++; return '\r';
            case 't': i_++; return '\t';
            case 'f': i_++; return '\f';
            case 'v': i_++; return '\v';
            case '0':
                if (!(peek() >= '0' && peek() <= '9')) { i_++; return 0; }
                break;
            case 'c':
                if (isalpha(static_cast<unsigned char>(peek()))) {
                    char letter = peek();
                    i_ += 2;
                    return static_cast<uint32_t>(toupper(static_cast<unsigned char>(letter)) % 32);
                }
                break;
            case 'x':
                if (is_hex_digit(peek()) && is_hex_digit(peek(2))) {
                    uint32_t v = static_cast<uint32_t>(hex_val(peek()) * 16 + hex_val(peek(2)));
                    i_ += 3;
                    return v;
                }
                break;
            case 'u': {
                if (peek() == '{') {
                    size_t j = i_ + 2;
                    uint32_t v = 0;
                    bool any = false;
                    while (j < pat_.size() && pat_[j] != '}') {
                        if (!is_hex_digit(pat_[j])) break;
                        v = v * 16 + static_cast<uint32_t>(hex_val(pat_[j]));
                        any = true;
                        j++;
                    }
                    if (any && j < pat_.size() && pat_[j] == '}') { i_ = j + 1; return v; }
                } else if (is_hex_digit(peek()) && is_hex_digit(peek(2)) && is_hex_digit(peek(3)) && is_hex_digit(peek(4))) {
                    uint32_t hi = static_cast<uint32_t>(hex_val(peek()) << 12 | hex_val(peek(2)) << 8 | hex_val(peek(3)) << 4 | hex_val(peek(4)));
                    i_ += 5;
                    if (unicode_ && hi >= 0xD800 && hi <= 0xDBFF && cur() == '\\' && peek() == 'u' &&
                        is_hex_digit(peek(2)) && is_hex_digit(peek(3)) && is_hex_digit(peek(4)) && is_hex_digit(peek(5))) {
                        uint32_t lo = static_cast<uint32_t>(hex_val(peek(2)) << 12 | hex_val(peek(3)) << 8 | hex_val(peek(4)) << 4 | hex_val(peek(5)));
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            i_ += 6;
                            return 0x10000 + ((hi - 0xD800) << 10) + (lo - 0xDC00);
                        }
                    }
                    return hi;
                }
                break;
            }
            default: break;
        }
        // IdentityEscape: any other character stands for itself.
        return next_cp();
    }

    std::unique_ptr<Node> parse_char_class() {
        i_++; // [
        auto n = std::make_unique<CharClassNode>();
        n->ignore_case = ignore_case_;
        if (!eof() && cur() == '^') { n->negated = true; i_++; }
        while (!eof() && cur() != ']') {
            uint32_t lo_cp; bool lo_is_escape;
            auto lo_ranges = parse_class_atom(lo_cp, lo_is_escape);
            if (!lo_ranges.empty()) { for (auto& r : lo_ranges) n->ranges.push_back(r); continue; }
            if (!eof() && cur() == '-' && peek() != ']' && i_ + 1 < pat_.size()) {
                i_++;
                uint32_t hi_cp; bool hi_is_escape;
                auto hi_ranges = parse_class_atom(hi_cp, hi_is_escape);
                if (!hi_ranges.empty() || lo_is_escape || hi_is_escape) throw UnsupportedSyntax("invalid class range");
                n->ranges.push_back({lo_cp, hi_cp});
            } else {
                n->ranges.push_back({lo_cp, lo_cp});
            }
        }
        if (eof()) throw UnsupportedSyntax("unterminated character class");
        i_++; // ]
        return n;
    }

    // Non-empty return = atom was a class-escape (\d\w\s...); else sets cp_out.
    std::vector<Range> parse_class_atom(uint32_t& cp_out, bool& is_escape) {
        is_escape = false;
        if (cur() == '\\') {
            char n = peek();
            if (n == 'd' || n == 'D' || n == 'w' || n == 'W' || n == 's' || n == 'S') {
                i_ += 2;
                is_escape = true;
                bool negated = isupper(static_cast<unsigned char>(n));
                std::vector<Range> base = (n == 'd' || n == 'D') ? digit_ranges() : (n == 'w' || n == 'W') ? word_ranges() : space_ranges();
                if (!negated) return base;
                throw UnsupportedSyntax("negated class-escape inside class range union");
            }
            if (n == 'b') { i_ += 2; cp_out = 0x08; return {}; }
            if (n == 'p' || n == 'P') throw UnsupportedSyntax("unicode property escape");
            i_++;
            cp_out = parse_character_escape();
            return {};
        }
        cp_out = next_cp();
        return {};
    }

public:
    bool ignore_case_ = false;
    bool multiline_ = false;
    bool dot_all_ = false;
};

} // namespace

struct RegexBacktrackEngine::Impl {
    std::unique_ptr<Node> root;
    uint32_t group_count = 0;
    bool unicode = false;
};

RegexBacktrackEngine::RegexBacktrackEngine(const std::string& pattern_wtf8, bool ignore_case, bool multiline,
                                            bool dot_all, bool unicode)
    : impl_(std::make_unique<Impl>()) {
    Parser parser(pattern_wtf8, unicode);
    parser.ignore_case_ = ignore_case;
    parser.multiline_ = multiline;
    parser.dot_all_ = dot_all;
    std::unordered_map<std::string, std::vector<int>> names;
    impl_->root = parser.parse(names);
    impl_->group_count = parser.group_count();
    impl_->unicode = unicode;
}

RegexBacktrackEngine::~RegexBacktrackEngine() = default;

uint32_t RegexBacktrackEngine::capture_count() const { return impl_->group_count; }

bool RegexBacktrackEngine::exec(const std::u16string& subject, size_t start_at, bool sticky, BacktrackMatch& out) const {
    // Atoms are code points in /u mode, raw UTF-16 units otherwise.
    std::vector<uint32_t> atoms;
    std::vector<size_t> atom_unit_offset;
    atoms.reserve(subject.size());
    atom_unit_offset.reserve(subject.size() + 1);
    if (impl_->unicode) {
        for (size_t i = 0; i < subject.size();) {
            atom_unit_offset.push_back(i);
            char16_t c = subject[i];
            if (c >= 0xD800 && c <= 0xDBFF && i + 1 < subject.size() && subject[i + 1] >= 0xDC00 && subject[i + 1] <= 0xDFFF) {
                atoms.push_back(0x10000 + ((static_cast<uint32_t>(c) - 0xD800) << 10) + (static_cast<uint32_t>(subject[i + 1]) - 0xDC00));
                i += 2;
            } else {
                atoms.push_back(c);
                i += 1;
            }
        }
        atom_unit_offset.push_back(subject.size());
    } else {
        for (size_t i = 0; i < subject.size(); i++) { atoms.push_back(subject[i]); atom_unit_offset.push_back(i); }
        atom_unit_offset.push_back(subject.size());
    }
    // Map the caller's UTF-16 start_at to an atom index.
    size_t start_atom = 0;
    while (start_atom < atom_unit_offset.size() && atom_unit_offset[start_atom] < start_at) start_atom++;

    for (size_t s = start_atom; s <= atoms.size(); s++) {
        MatcherContext ctx{atoms, false, false, false, {}};
        ctx.captures.assign(impl_->group_count + 1, {-1, -1});
        int64_t end = -1;
        Cont accept = [&end](int64_t p) { end = p; return true; };
        if (impl_->root->match(ctx, static_cast<int64_t>(s), 1, accept)) {
            out.matched = true;
            out.start = atom_unit_offset[s];
            out.end = atom_unit_offset[static_cast<size_t>(end)];
            out.captures.resize(impl_->group_count + 1);
            for (uint32_t g = 1; g <= impl_->group_count; g++) {
                auto c = ctx.captures[g];
                if (c.first < 0) out.captures[g] = {-1, -1};
                else out.captures[g] = {static_cast<int64_t>(atom_unit_offset[static_cast<size_t>(c.first)]),
                                         static_cast<int64_t>(atom_unit_offset[static_cast<size_t>(c.second)])};
            }
            return true;
        }
        if (sticky) break;
    }
    out.matched = false;
    return false;
}

bool RegexBacktrackEngine::pattern_needs_backtrack_engine(const std::string& pattern) {
    // Risky: a lookbehind with a repeated capturing group, a backreference, or a nested lookaround.
    int cc = 0;
    for (size_t i = 0; i + 3 < pattern.size(); i++) {
        if (pattern[i] == '\\') { i++; continue; }
        if (pattern[i] == '[') { cc++; continue; }
        if (pattern[i] == ']') { if (cc > 0) cc--; continue; }
        if (cc > 0) continue;
        bool is_lb = pattern[i] == '(' && pattern[i + 1] == '?' && pattern[i + 2] == '<' &&
                     (pattern[i + 3] == '=' || pattern[i + 3] == '!');
        if (!is_lb) continue;
        size_t j = i + 4;
        int depth = 1;
        bool jcc = false;
        int group_open = 0; // capturing groups still open within this lookbehind
        bool risky = false;
        while (j < pattern.size() && depth > 0) {
            if (pattern[j] == '\\' && j + 1 < pattern.size()) {
                char n = pattern[j + 1];
                if (!jcc && (n >= '1' && n <= '9')) risky = true; // any backref
                if (!jcc && n == 'k') risky = true; // named backref
                j += 2;
                continue;
            }
            if (pattern[j] == '[' && !jcc) { jcc = true; j++; continue; }
            if (pattern[j] == ']' && jcc) { jcc = false; j++; continue; }
            if (jcc) { j++; continue; }
            if (pattern[j] == '(') {
                depth++;
                bool is_capturing = true;
                if (j + 1 < pattern.size() && pattern[j + 1] == '?') {
                    is_capturing = (j + 3 < pattern.size() && pattern[j + 2] == '<' && pattern[j + 3] != '=' && pattern[j + 3] != '!');
                    bool is_lookaround = (j + 2 < pattern.size() && (pattern[j + 2] == '=' || pattern[j + 2] == '!')) ||
                                         (j + 3 < pattern.size() && pattern[j + 2] == '<' && (pattern[j + 3] == '=' || pattern[j + 3] == '!'));
                    if (is_lookaround) risky = true;
                }
                if (is_capturing) group_open++;
                j++;
                continue;
            }
            if (pattern[j] == ')') {
                depth--;
                j++;
                if (depth > 0 && group_open > 0) {
                    // Closed group followed by a >1 quantifier?
                    if (j < pattern.size()) {
                        char q = pattern[j];
                        bool repeats = false;
                        if (q == '*' || q == '+') repeats = true;
                        else if (q == '{') {
                            size_t k = j + 1;
                            long lo = 0;
                            bool has_lo = false;
                            while (k < pattern.size() && isdigit(static_cast<unsigned char>(pattern[k]))) { lo = lo * 10 + (pattern[k] - '0'); has_lo = true; k++; }
                            bool comma = k < pattern.size() && pattern[k] == ',';
                            if (has_lo && (comma || lo > 1)) repeats = true;
                        }
                        if (repeats) risky = true;
                    }
                    group_open--;
                }
                continue;
            }
            j++;
        }
        if (risky) return true;
        i = j - 1;
    }
    return false;
}

} // namespace Quanta
