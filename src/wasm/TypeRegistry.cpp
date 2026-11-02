/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/wasm/TypeRegistry.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace Quanta::Wasm {

namespace {

// The types are in chunks that never move, so that Get needs no lock: a chunk is published before any id in it is handed out.
constexpr uint32_t kChunkBits = 12;
constexpr uint32_t kChunkSize = 1u << kChunkBits;
constexpr uint32_t kMaxChunks = 4096;

struct Registry {
    std::mutex mutex;
    std::atomic<CanonType*> chunks[kMaxChunks] = {};
    std::atomic<uint32_t> count{0};
    std::unordered_map<std::string, uint32_t> groups;
};

Registry& R() {
    static Registry* registry = new Registry();  // (never destroyed: types are used by whatever runs last)
    return *registry;
}

CanonType& Slot(Registry& r, uint32_t id) {
    CanonType* chunk = r.chunks[id >> kChunkBits].load(std::memory_order_acquire);
    return chunk[id & (kChunkSize - 1)];
}

// The id of a new type (the lock is held).
bool Append(Registry& r, CanonType&& type, uint32_t& id) {
    id = r.count.load(std::memory_order_relaxed);
    const uint32_t chunk = id >> kChunkBits;
    if (chunk >= kMaxChunks) return false;
    if (!r.chunks[chunk].load(std::memory_order_relaxed)) r.chunks[chunk].store(new CanonType[kChunkSize], std::memory_order_release);
    Slot(r, id) = std::move(type);
    r.count.store(id + 1, std::memory_order_release);
    return true;
}

void Put(std::string& key, uint32_t v) { key.append(reinterpret_cast<const char*>(&v), 4); }

// A type that is in the group being made is told by its place in it: that is what makes two groups the same whatever their ids are.
struct GroupContext {
    const std::vector<uint32_t>& ids;  // (of the types before the group)
    uint32_t first, size;
};

void PutValType(std::string& key, const ValType& v, const GroupContext& g) {
    key.push_back(static_cast<char>(v.kind));
    if (!v.IsRef()) return;
    key.push_back(v.nullable ? 1 : 0);
    if (v.heap < 0) {
        key.push_back('A');
        Put(key, static_cast<uint32_t>(v.heap));
    } else if (static_cast<uint32_t>(v.heap) >= g.first) {
        key.push_back('R');
        Put(key, static_cast<uint32_t>(v.heap) - g.first);
    } else {
        key.push_back('C');
        Put(key, g.ids[static_cast<size_t>(v.heap)]);
    }
}

void PutField(std::string& key, const FieldType& f, const GroupContext& g) {
    key.push_back(f.mut ? 1 : 0);
    key.push_back(static_cast<char>(f.type.pack));
    PutValType(key, f.type.val, g);
}

ValType MapIn(const ValType& v, const std::vector<uint32_t>& ids, uint32_t first, uint32_t base) {
    if (!v.IsRef() || v.heap < 0) return v;
    const uint32_t h = static_cast<uint32_t>(v.heap);
    return ValType::Ref(v.nullable, static_cast<HeapType>(h >= first ? base + (h - first) : ids[h]));
}

}  // namespace

namespace TypeRegistry {

const CanonType& Get(uint32_t id) { return Slot(R(), id); }

ValType Map(const ValType& v, const std::vector<uint32_t>& ids) {
    if (!v.IsRef() || v.heap < 0) return v;
    return ValType::Ref(v.nullable, static_cast<HeapType>(ids[static_cast<size_t>(v.heap)]));
}

bool HeapSubtype(HeapType a, HeapType b) {
    if (a == b) return true;
    if (a < 0) {
        switch (a) {
            case Heap::None: return b == Heap::Any || b == Heap::Eq || b == Heap::I31 || b == Heap::Struct || b == Heap::Array || (b >= 0 && Get(static_cast<uint32_t>(b)).kind != TypeDef::Kind::Func);
            case Heap::NoFunc: return b == Heap::Func || (b >= 0 && Get(static_cast<uint32_t>(b)).kind == TypeDef::Kind::Func);
            case Heap::NoExtern: return b == Heap::Extern;
            case Heap::NoExn: return b == Heap::Exn;
            case Heap::I31: return b == Heap::Eq || b == Heap::Any;
            case Heap::Struct: return b == Heap::Eq || b == Heap::Any;
            case Heap::Array: return b == Heap::Eq || b == Heap::Any;
            case Heap::Eq: return b == Heap::Any;
            default: return false;
        }
    }
    const CanonType& t = Get(static_cast<uint32_t>(a));
    if (b < 0) {
        switch (t.kind) {
            case TypeDef::Kind::Func: return b == Heap::Func;
            case TypeDef::Kind::Struct: return b == Heap::Struct || b == Heap::Eq || b == Heap::Any;
            case TypeDef::Kind::Array: return b == Heap::Array || b == Heap::Eq || b == Heap::Any;
        }
        return false;
    }
    for (int64_t x = t.super; x >= 0; x = Get(static_cast<uint32_t>(x)).super) {
        if (x == b) return true;
    }
    return false;
}

bool Matches(const ValType& a, const ValType& b) {
    if (a.IsBottom()) return true;
    if (a.kind != b.kind) return false;
    if (!a.IsRef()) return true;
    if (a.nullable && !b.nullable) return false;
    return HeapSubtype(a.heap, b.heap);
}

bool Matches(const StorageType& a, const StorageType& b) {
    if (a.pack != b.pack) return false;
    return a.pack != StorageType::Pack::None || Matches(a.val, b.val);
}

bool SameSignature(uint32_t a, uint32_t b) { return a == b; }

namespace {

bool SameStorage(const StorageType& a, const StorageType& b) {
    return a.pack == b.pack && (a.pack != StorageType::Pack::None || a.val == b.val);
}

// Whether the type `sub` is what its supertype allows.
bool SubtypeOk(const CanonType& sub, const CanonType& super, std::string& why) {
    if (sub.kind != super.kind) { why = "a type and its supertype are not of the same kind"; return false; }
    switch (sub.kind) {
        case TypeDef::Kind::Func: {
            if (sub.func.params.size() != super.func.params.size() || sub.func.results.size() != super.func.results.size()) { why = "a function type and its supertype take and give different numbers of values"; return false; }
            for (size_t i = 0; i < sub.func.params.size(); ++i) {
                if (!Matches(super.func.params[i], sub.func.params[i])) { why = "a parameter of a function type is not what its supertype takes"; return false; }
            }
            for (size_t i = 0; i < sub.func.results.size(); ++i) {
                if (!Matches(sub.func.results[i], super.func.results[i])) { why = "a result of a function type is not what its supertype gives"; return false; }
            }
            return true;
        }
        case TypeDef::Kind::Struct: {
            if (sub.str.fields.size() < super.str.fields.size()) { why = "a struct type has fewer fields than its supertype"; return false; }
            for (size_t i = 0; i < super.str.fields.size(); ++i) {
                const FieldType& a = sub.str.fields[i];
                const FieldType& b = super.str.fields[i];
                if (a.mut != b.mut) { why = "a field of a struct type is not as mutable as in its supertype"; return false; }
                if (a.mut ? !SameStorage(a.type, b.type) : !Matches(a.type, b.type)) { why = "a field of a struct type does not match its supertype"; return false; }
            }
            return true;
        }
        case TypeDef::Kind::Array: {
            const FieldType& a = sub.arr.element;
            const FieldType& b = super.arr.element;
            if (a.mut != b.mut) { why = "the element of an array type is not as mutable as in its supertype"; return false; }
            if (a.mut ? !SameStorage(a.type, b.type) : !Matches(a.type, b.type)) { why = "the element of an array type does not match its supertype"; return false; }
            return true;
        }
    }
    return false;
}

}  // namespace

bool Canonicalize(const Module& module, std::vector<uint32_t>& ids, std::string& error) {
    Registry& r = R();
    ids.assign(module.types.size(), 0);
    std::lock_guard<std::mutex> lock(r.mutex);
    for (uint32_t first = 0; first < module.types.size();) {
        const uint32_t size = module.types[first].groupSize;
        GroupContext g{ids, first, size};
        std::string key;
        Put(key, size);
        for (uint32_t i = 0; i < size; ++i) {
            const TypeDef& t = module.types[first + i];
            key.push_back(t.final ? 1 : 0);
            if (t.supers.empty()) {
                key.push_back('N');
            } else if (t.supers[0] >= first) {
                key.push_back('R');
                Put(key, t.supers[0] - first);
            } else {
                key.push_back('C');
                Put(key, ids[t.supers[0]]);
            }
            key.push_back(static_cast<char>(t.kind));
            switch (t.kind) {
                case TypeDef::Kind::Func:
                    Put(key, static_cast<uint32_t>(t.func.params.size()));
                    for (const ValType& v : t.func.params) PutValType(key, v, g);
                    Put(key, static_cast<uint32_t>(t.func.results.size()));
                    for (const ValType& v : t.func.results) PutValType(key, v, g);
                    break;
                case TypeDef::Kind::Struct:
                    Put(key, static_cast<uint32_t>(t.str.fields.size()));
                    for (const FieldType& f : t.str.fields) PutField(key, f, g);
                    break;
                case TypeDef::Kind::Array: PutField(key, t.arr.element, g); break;
            }
        }
        const auto found = r.groups.find(key);
        if (found != r.groups.end()) {
            for (uint32_t i = 0; i < size; ++i) ids[first + i] = found->second + i;
            first += size;
            continue;
        }
        const uint32_t base = r.count.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < size; ++i) {
            const TypeDef& t = module.types[first + i];
            CanonType c;
            c.kind = t.kind;
            c.final = t.final;
            for (const ValType& v : t.func.params) c.func.params.push_back(MapIn(v, ids, first, base));
            for (const ValType& v : t.func.results) c.func.results.push_back(MapIn(v, ids, first, base));
            for (const FieldType& f : t.str.fields) {
                FieldType m = f;
                m.type.val = MapIn(f.type.val, ids, first, base);
                c.str.fields.push_back(m);
            }
            c.arr = t.arr;
            c.arr.element.type.val = MapIn(t.arr.element.type.val, ids, first, base);
            if (!t.supers.empty()) c.super = t.supers[0] >= first ? base + (t.supers[0] - first) : ids[t.supers[0]];
            uint32_t id;
            if (!Append(r, std::move(c), id)) { error = "too many types"; return false; }
            ids[first + i] = id;
        }
        // (The supertypes are checked with the group in place, and the group is forgotten again if it is no good.)
        std::string why;
        bool ok = true;
        for (uint32_t i = 0; i < size && ok; ++i) {
            CanonType& c = Slot(r, base + i);
            if (c.super < 0) continue;
            const CanonType& super = Slot(r, static_cast<uint32_t>(c.super));
            if (super.final) { why = "a type has a final supertype"; ok = false; break; }
            if (!SubtypeOk(c, super, why)) { ok = false; break; }
            c.depth = super.depth + 1;
            if (c.depth > 63) { why = "a type has too many supertypes"; ok = false; }
        }
        if (!ok) {
            error = why;
            return false;
        }
        r.groups.emplace(std::move(key), base);
        first += size;
    }
    return true;
}

}  // namespace TypeRegistry

}  // namespace Quanta::Wasm
