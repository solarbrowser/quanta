/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/parser/ScriptCache.h"
#include "quanta/parser/NamePool.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>

namespace Quanta {

namespace {

// Changes whenever what a parse records about a body, or how a recorded body is used, changes.
constexpr uint32_t kFormat = 1;
constexpr char kMagic[4] = {'Q', 'S', 'C', '1'};

uint64_t fnv1a(const void* data, size_t size, uint64_t hash = 1469598103934665603ull) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; i++) {
        hash ^= p[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

void put_varint(std::vector<uint8_t>& out, uint64_t v) {
    while (v >= 0x80) { out.push_back(static_cast<uint8_t>(v) | 0x80); v >>= 7; }
    out.push_back(static_cast<uint8_t>(v));
}

void put_u64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; i++) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    bool ok = true;
    uint64_t varint() {
        uint64_t v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            if (p == end) { ok = false; return 0; }
            uint8_t b = *p++;
            v |= static_cast<uint64_t>(b & 0x7f) << shift;
            if (!(b & 0x80)) return v;
        }
        ok = false;
        return 0;
    }
    uint64_t u64() {
        if (end - p < 8) { ok = false; return 0; }
        uint64_t v = 0;
        for (int i = 0; i < 8; i++) v |= static_cast<uint64_t>(*p++) << (8 * i);
        return v;
    }
};

enum Flag : uint32_t {
    kFreeValid = 1u << 0, kFreeUnknown = 1u << 1, kFreeSawEval = 1u << 2, kFreeSawClass = 1u << 3,
    kMethodSuperValid = 1u << 4, kMethodReferencesSuper = 1u << 5, kEvalAnywhere = 1u << 6,
    kSuperAnywhere = 1u << 7, kEvalInNested = 1u << 8, kClassExpression = 1u << 9,
    kCapturesOuter = 1u << 10, kBodyStrict = 1u << 11,
};

uint32_t flags_of(const BodyScopeInfo& i) {
    uint32_t f = 0;
    if (i.free_valid) f |= kFreeValid;
    if (i.free_unknown) f |= kFreeUnknown;
    if (i.free_saw_eval) f |= kFreeSawEval;
    if (i.free_saw_class) f |= kFreeSawClass;
    if (i.method_super_valid) f |= kMethodSuperValid;
    if (i.method_references_super) f |= kMethodReferencesSuper;
    if (i.eval_anywhere) f |= kEvalAnywhere;
    if (i.super_anywhere) f |= kSuperAnywhere;
    if (i.eval_in_nested) f |= kEvalInNested;
    if (i.class_expression) f |= kClassExpression;
    if (i.captures_outer) f |= kCapturesOuter;
    if (i.body_strict) f |= kBodyStrict;
    return f;
}

}

std::vector<uint8_t> ScriptCache::serialize(const ScriptUnit& unit, std::string_view source) {
    // The bodies in source order, and the names they mention once each.
    std::vector<std::pair<uint32_t, const BodyScopeInfo*>> bodies;
    bodies.reserve(unit.body_scopes().size());
    for (const auto& [open, info] : unit.body_scopes()) {
        if (info.body_end > open) bodies.emplace_back(open, &info);
    }
    std::sort(bodies.begin(), bodies.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    std::unordered_map<uint32_t, uint32_t> index_of;   // NamePool id -> position in the table
    std::vector<uint32_t> table;
    auto note = [&](const FrozenIds& ids) {
        for (uint32_t id : ids) {
            if (index_of.emplace(id, static_cast<uint32_t>(table.size())).second) table.push_back(id);
        }
    };
    for (const auto& [open, info] : bodies) {
        note(info->captured);
        note(info->all_names);
        note(info->free_names);
    }

    std::vector<uint8_t> out;
    out.insert(out.end(), kMagic, kMagic + 4);
    put_varint(out, kFormat);
    put_u64(out, fnv1a(source.data(), source.size()));
    put_varint(out, source.size());
    put_varint(out, table.size());
    for (uint32_t id : table) {
        const std::string& name = NamePool::text(id);
        put_varint(out, name.size());
        out.insert(out.end(), name.begin(), name.end());
    }
    put_varint(out, bodies.size());
    uint32_t previous = 0;
    auto put_ids = [&](const FrozenIds& ids) {
        put_varint(out, ids.size());
        std::vector<uint32_t> indices;
        indices.reserve(ids.size());
        for (uint32_t id : ids) indices.push_back(index_of[id]);
        std::sort(indices.begin(), indices.end());
        uint32_t last = 0;
        for (uint32_t i : indices) { put_varint(out, i - last); last = i; }
    };
    for (const auto& [open, info] : bodies) {
        put_varint(out, open - previous);
        previous = open;
        put_varint(out, info->body_end - open);
        put_varint(out, flags_of(*info));
        put_ids(info->captured);
        put_ids(info->all_names);
        put_ids(info->free_names);
    }
    put_u64(out, fnv1a(out.data(), out.size()));
    return out;
}

bool ScriptCache::deserialize(const uint8_t* bytes, size_t size, std::string_view source, ScriptCache& out) {
    out.entries.clear();
    if (!bytes || size < 4 + 8 + 8) return false;
    // The checksum of everything before it: a damaged cache is refused rather than trusted.
    {
        Reader tail{bytes + size - 8, bytes + size};
        if (tail.u64() != fnv1a(bytes, size - 8)) return false;
    }
    if (std::memcmp(bytes, kMagic, 4) != 0) return false;
    Reader r{bytes + 4, bytes + size - 8};
    if (r.varint() != kFormat || !r.ok) return false;
    const uint64_t hash = r.u64();
    const uint64_t length = r.varint();
    if (!r.ok || length != source.size() || hash != fnv1a(source.data(), source.size())) return false;

    const uint64_t names = r.varint();
    if (!r.ok || names > size) return false;
    std::vector<uint32_t> ids;
    ids.reserve(names);
    for (uint64_t i = 0; i < names; i++) {
        const uint64_t len = r.varint();
        if (!r.ok || static_cast<uint64_t>(r.end - r.p) < len) return false;
        ids.push_back(NamePool::intern(std::string_view(reinterpret_cast<const char*>(r.p), len)));
        r.p += len;
    }
    const uint64_t count = r.varint();
    if (!r.ok || count > size) return false;
    out.entries.reserve(count);
    uint32_t previous = 0;
    auto read_ids = [&](FrozenIds& into) -> bool {
        const uint64_t n = r.varint();
        if (!r.ok || n > size) return false;
        IdSet set;
        uint64_t index = 0;
        for (uint64_t i = 0; i < n; i++) {
            index += r.varint();
            if (!r.ok || index >= ids.size()) return false;
            set.insert(ids[index]);
        }
        into = FrozenIds(set);
        return true;
    };
    for (uint64_t i = 0; i < count; i++) {
        Entry e;
        previous += static_cast<uint32_t>(r.varint());
        e.open = previous;
        const uint64_t span = r.varint();
        const uint64_t f = r.varint();
        if (!r.ok || span == 0 || e.open + span > source.size()) return false;
        e.info.body_end = static_cast<uint32_t>(e.open + span);
        e.info.free_valid = f & kFreeValid;
        e.info.free_unknown = f & kFreeUnknown;
        e.info.free_saw_eval = f & kFreeSawEval;
        e.info.free_saw_class = f & kFreeSawClass;
        e.info.method_super_valid = f & kMethodSuperValid;
        e.info.method_references_super = f & kMethodReferencesSuper;
        e.info.eval_anywhere = f & kEvalAnywhere;
        e.info.super_anywhere = f & kSuperAnywhere;
        e.info.eval_in_nested = f & kEvalInNested;
        e.info.class_expression = f & kClassExpression;
        e.info.captures_outer = f & kCapturesOuter;
        e.info.body_strict = f & kBodyStrict;
        if (!read_ids(e.info.captured) || !read_ids(e.info.all_names) || !read_ids(e.info.free_names)) return false;
        out.entries.push_back(std::move(e));
    }
    if (!r.ok || r.p != r.end) {
        out.entries.clear();
        return false;
    }
    return true;
}

}
