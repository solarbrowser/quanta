/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

// The checks of the meaning of a decoded module (the types, the index spaces, the segments, the constant expressions), and, through the compiler,
// of the bodies of its functions.

#include <unordered_set>

#include "quanta/wasm/Decoder.h"
#include "quanta/wasm/Reader.h"
#include "src/wasm/Internal.h"

namespace Quanta::Wasm {

namespace {

constexpr uint64_t kMaxPages32 = 65536;
constexpr uint64_t kMaxPages64 = uint64_t(1) << 48;

bool CheckLimits(const Limits& l, uint64_t bound, const char* what, std::string& error) {
    if (l.min > bound) { error = std::string("initial ") + what + " size is too large"; return false; }
    if (l.hasMax) {
        if (l.max > bound) { error = std::string("maximum ") + what + " size is too large"; return false; }
        if (l.min > l.max) { error = "size minimum must not be greater than maximum"; return false; }
    }
    return true;
}

bool CheckMemory(const MemType& m, std::string& error) { return CheckLimits(m.limits, m.limits.is64 ? kMaxPages64 : kMaxPages32, "memory", error); }

bool CheckTable(const TableType& t, std::string& error) {
    if (!t.elem.IsRef()) { error = "table element type must be a reference type"; return false; }
    return CheckLimits(t.limits, t.limits.is64 ? ~uint64_t(0) : 0xFFFFFFFFull, "table", error);
}

bool CheckRef(const Module& m, const ValType& t, std::string& error) {
    if (t.IsRef() && t.heap >= 0 && static_cast<size_t>(t.heap) >= m.types.size()) {
        error = "unknown type " + std::to_string(t.heap);
        return false;
    }
    return true;
}

// The functions a constant expression names with ref.func.
void ScanRefFuncs(const Module& m, const ByteRange& range, std::vector<bool>& declared) {
    Reader reader(m.bytes.data() + range.offset, range.size, range.offset);
    while (reader.ok() && !reader.atEnd()) {
        const uint8_t op = reader.u8();
        switch (op) {
            case 0x41: reader.s32(); break;
            case 0x42: reader.s64(); break;
            case 0x43: reader.skip(4); break;
            case 0x44: reader.skip(8); break;
            case 0xD0: reader.heapType(); break;
            case 0x23: reader.u32(); break;
            case 0xD2: {
                const uint32_t f = reader.u32();
                if (reader.ok() && f < declared.size()) declared[f] = true;
                break;
            }
            case 0xFB: {
                const uint32_t sub = reader.u32();
                if (sub == 0x00 || sub == 0x01 || sub == 0x06 || sub == 0x07) reader.u32();
                else if (sub == 0x08) { reader.u32(); reader.u32(); }
                break;
            }
            case 0xFD: {
                const uint32_t sub = reader.u32();
                if (sub == 0x0C) reader.skip(16);
                break;
            }
            default: break;
        }
    }
}

}  // namespace

bool Validate(Module& m, std::string& error) {
    ModuleContext cx(m);
    if (!TypeRegistry::Canonicalize(m, cx.typeIds, error)) return false;
    m.typeIds = cx.typeIds;

    // (Every type is about types that are there.)
    for (const TypeDef& t : m.types) {
        for (const ValType& v : t.func.params) if (!CheckRef(m, v, error)) return false;
        for (const ValType& v : t.func.results) if (!CheckRef(m, v, error)) return false;
        for (const FieldType& f : t.str.fields) if (!CheckRef(m, f.type.val, error)) return false;
        if (t.kind == TypeDef::Kind::Array && !CheckRef(m, t.arr.element.type.val, error)) return false;
    }

    for (uint32_t type : m.funcTypes) {
        if (type >= m.types.size()) { error = "unknown type"; return false; }
        if (m.types[type].kind != TypeDef::Kind::Func) { error = "function type is not a function type"; return false; }
        cx.funcSigs.push_back(cx.typeIds[type]);
    }
    for (const TableType& t : m.tableTypes) {
        if (!CheckRef(m, t.elem, error) || !CheckTable(t, error)) return false;
        cx.tables.push_back(TableType{cx.Canon(t.elem), t.limits});
    }
    for (const MemType& t : m.memTypes) {
        if (!CheckMemory(t, error)) return false;
        cx.memories.push_back(t);
    }
    for (const GlobalType& g : m.globalTypes) {
        if (!CheckRef(m, g.type, error)) return false;
        cx.globals.push_back(GlobalType{cx.Canon(g.type), g.mut});
    }
    for (const TagType& t : m.tagTypes) {
        if (t.type >= m.types.size()) { error = "unknown type"; return false; }
        const CanonType& c = TypeRegistry::Get(cx.typeIds[t.type]);
        if (c.kind != TypeDef::Kind::Func) { error = "tag type is not a function type"; return false; }
        if (!c.func.results.empty()) { error = "tag type has results"; return false; }
        cx.tagSigs.push_back(cx.typeIds[t.type]);
    }
    for (const ElemSegment& e : m.elems) {
        if (!CheckRef(m, e.type, error)) return false;
        cx.elemTypes.push_back(cx.Canon(e.type));
    }
    m.funcSigs = cx.funcSigs;
    m.tagSigs = cx.tagSigs;

    // The functions a ref.func may name: the ones that appear outside of function bodies.
    cx.declared.assign(m.funcTypes.size(), false);
    for (const Export& e : m.exports) {
        if (e.kind != ExternKind::Func) continue;
        if (e.index >= cx.declared.size()) { error = "unknown function"; return false; }
        cx.declared[e.index] = true;
    }
    for (const ElemSegment& e : m.elems) {
        for (uint32_t f : e.funcs) {
            if (f >= cx.declared.size()) { error = "unknown function"; return false; }
            cx.declared[f] = true;
        }
        for (const ByteRange& x : e.exprs) ScanRefFuncs(m, x, cx.declared);
        if (e.mode == ElemSegment::Mode::Active) ScanRefFuncs(m, e.offset, cx.declared);
    }
    for (const Global& g : m.globals) ScanRefFuncs(m, g.init, cx.declared);
    for (const Table& t : m.tables) if (t.hasInit) ScanRefFuncs(m, t.init, cx.declared);
    for (const DataSegment& d : m.datas) if (d.active) ScanRefFuncs(m, d.offset, cx.declared);
    m.declaredFuncs = cx.declared;

    // The exports have names of their own, and what they name is there.
    std::unordered_set<std::string> names;
    for (const Export& e : m.exports) {
        if (!names.insert(e.name).second) { error = "duplicate export name"; return false; }
        size_t limit = 0;
        switch (e.kind) {
            case ExternKind::Func: limit = cx.funcSigs.size(); break;
            case ExternKind::Table: limit = cx.tables.size(); break;
            case ExternKind::Memory: limit = cx.memories.size(); break;
            case ExternKind::Global: limit = cx.globals.size(); break;
            case ExternKind::Tag: limit = cx.tagSigs.size(); break;
        }
        if (e.index >= limit) { error = "unknown export index"; return false; }
    }

    if (m.hasStart) {
        if (m.start >= cx.funcSigs.size()) { error = "unknown function"; return false; }
        const FuncType& f = TypeRegistry::Get(cx.funcSigs[m.start]).func;
        if (!f.params.empty() || !f.results.empty()) { error = "start function must have no parameters or results"; return false; }
    }

    // The constant expressions.
    m.constExprs.clear();
    const auto constExpr = [&](ByteRange& range, const ValType& expected, uint32_t globalLimit) {
        CompiledFunction code;
        if (!CompileConstExpr(cx, range, expected, globalLimit, code, error)) return false;
        range.compiled = static_cast<int32_t>(m.constExprs.size());
        m.constExprs.push_back(std::move(code));
        return true;
    };
    for (size_t i = 0; i < m.globals.size(); ++i) {
        const uint32_t index = m.importedGlobals + static_cast<uint32_t>(i);
        if (!constExpr(m.globals[i].init, cx.globals[index].type, index)) return false;
    }
    const uint32_t allGlobals = static_cast<uint32_t>(cx.globals.size());
    for (size_t i = 0; i < m.tables.size(); ++i) {
        Table& t = m.tables[i];
        const uint32_t index = m.importedTables + static_cast<uint32_t>(i);
        if (t.hasInit) {
            if (!constExpr(t.init, cx.tables[index].elem, m.importedGlobals)) return false;
        } else if (!cx.tables[index].elem.nullable) {
            error = "a table of a type that does not allow null needs an initial value";
            return false;
        }
    }
    for (size_t i = 0; i < m.elems.size(); ++i) {
        ElemSegment& e = m.elems[i];
        const ValType type = cx.elemTypes[i];
        if (e.isFuncIndex) {
            for (uint32_t f : e.funcs) {
                if (f >= cx.funcSigs.size()) { error = "unknown function"; return false; }
            }
        } else {
            for (ByteRange& x : e.exprs) {
                if (!constExpr(x, type, allGlobals)) return false;
            }
        }
        if (e.mode == ElemSegment::Mode::Active) {
            if (e.table >= cx.tables.size()) { error = "unknown table"; return false; }
            if (!TypeRegistry::Matches(type, cx.tables[e.table].elem)) { error = "type mismatch: element type does not match the table's"; return false; }
            if (!constExpr(e.offset, cx.tables[e.table].limits.is64 ? ValType::I64() : ValType::I32(), allGlobals)) return false;
        }
    }
    if (m.hasDataCount && m.dataCount != m.datas.size()) { error = "data count and data section have inconsistent lengths"; return false; }
    for (DataSegment& d : m.datas) {
        if (!d.active) continue;
        if (d.memory >= cx.memories.size()) { error = "unknown memory"; return false; }
        if (!constExpr(d.offset, cx.memories[d.memory].limits.is64 ? ValType::I64() : ValType::I32(), allGlobals)) return false;
    }

    // The functions.
    m.compiled.clear();
    m.compiled.resize(m.bodies.size());
    for (size_t i = 0; i < m.bodies.size(); ++i) {
        std::string why;
        if (!CompileFunction(cx, m.importedFuncs + static_cast<uint32_t>(i), m.compiled[i], why)) {
            error = "function #" + std::to_string(m.importedFuncs + i) + ": " + why;
            return false;
        }
    }
    return true;
}

}  // namespace Quanta::Wasm
