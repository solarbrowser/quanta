/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/wasm/Decoder.h"

#include <cstring>

#include "quanta/wasm/Reader.h"

namespace Quanta::Wasm {

namespace {

// The limits an implementation may set (the same ones the engines that run the spec's own tests have).
constexpr uint32_t kMaxTypes = 1000000;
constexpr uint32_t kMaxFunctions = 1000000;
constexpr uint32_t kMaxImports = 100000;
constexpr uint32_t kMaxExports = 100000;
constexpr uint32_t kMaxGlobals = 1000000;
constexpr uint32_t kMaxTables = 100000;
constexpr uint32_t kMaxMemories = 100;
constexpr uint32_t kMaxTags = 1000000;
constexpr uint32_t kMaxSegments = 100000;
constexpr uint32_t kMaxLocals = 50000;

enum Section : uint8_t { kCustom = 0, kType = 1, kImport = 2, kFunction = 3, kTable = 4, kMemory = 5, kGlobal = 6, kExport = 7, kStart = 8, kElement = 9, kCode = 10, kData = 11, kDataCount = 12, kTag = 13 };

// The order the sections come in (the tag section is between memory and global, the data count before code).
int RankOf(uint8_t id) {
    switch (id) {
        case kType: return 1;
        case kImport: return 2;
        case kFunction: return 3;
        case kTable: return 4;
        case kMemory: return 5;
        case kTag: return 6;
        case kGlobal: return 7;
        case kExport: return 8;
        case kStart: return 9;
        case kElement: return 10;
        case kDataCount: return 11;
        case kCode: return 12;
        case kData: return 13;
        default: return -1;
    }
}

class Decoder {
public:
    Decoder(Module& m) : m_(m) {}

    bool Run() {
        Reader r(m_.bytes.data(), m_.bytes.size());
        if (m_.bytes.size() < 4 || std::memcmp(m_.bytes.data(), "\0asm", 4) != 0) {
            error_ = "expected magic word 00 61 73 6d";
            return false;
        }
        if (m_.bytes.size() < 8 || std::memcmp(m_.bytes.data() + 4, "\x01\x00\x00\x00", 4) != 0) {
            error_ = "expected version 01 00 00 00";
            return false;
        }
        r.skip(8);
        int lastRank = 0;
        while (!r.atEnd() && r.ok()) {
            const uint8_t id = r.u8();
            const uint32_t size = r.u32();
            if (!r.ok()) break;
            if (size > r.remaining()) {
                r.fail("section out of bounds");
                break;
            }
            const size_t start = r.offset();
            Reader section(r.cursor(), size, start);
            r.skip(size);
            if (id != kCustom) {
                const int rank = RankOf(id);
                if (rank < 0) {
                    r.fail("unknown section code " + std::to_string(id));
                    break;
                }
                if (rank <= lastRank) {
                    r.fail("unexpected section");
                    break;
                }
                lastRank = rank;
            }
            DecodeSection(id, section, start);
            if (!section.ok()) {
                error_ = section.error();
                return false;
            }
            if (!section.atEnd()) {
                error_ = "section was shorter than expected size @+" + std::to_string(section.offset());
                return false;
            }
        }
        if (!r.ok()) {
            error_ = r.error();
            return false;
        }
        if (!error_.empty()) return false;
        return Finish();
    }

    const std::string& error() const { return error_; }

private:
    Module& m_;
    std::string error_;
    uint32_t definedFuncs_ = 0;
    bool sawFunction_ = false, sawCode_ = false, sawData_ = false;

    uint32_t Count(Reader& r, uint32_t max, const char* what) {
        const uint32_t n = r.u32();
        if (!r.ok()) return 0;
        if (n > max) { r.fail(std::string("too many ") + what); return 0; }
        if (n > r.remaining()) { r.fail("unexpected end"); return 0; }  // (each item takes a byte at least)
        return n;
    }

    std::string Name(Reader& r) {
        const uint32_t n = r.u32();
        if (!r.ok()) return {};
        const uint8_t* p = r.bytes(n);
        if (!r.ok()) return {};
        if (!ValidUtf8(p, n)) { r.fail("malformed UTF-8 encoding"); return {}; }
        return std::string(reinterpret_cast<const char*>(p), n);
    }

    Limits ReadLimits(Reader& r, bool memory) {
        Limits l;
        const uint8_t flags = r.u8();
        if (!r.ok()) return l;
        if (flags & ~0x07) { r.fail("invalid limits flags"); return l; }
        l.hasMax = flags & 0x01;
        l.shared = flags & 0x02;
        l.is64 = flags & 0x04;
        if (l.shared) { r.fail("shared memory is not supported"); return l; }
        (void)memory;
        l.min = l.is64 ? r.u64() : r.u32();
        if (l.hasMax) l.max = l.is64 ? r.u64() : r.u32();
        return l;
    }

    TableType ReadTableType(Reader& r) {
        TableType t;
        t.elem = r.valType();
        if (r.ok() && !t.elem.IsRef()) r.fail("malformed reference type");
        t.limits = ReadLimits(r, false);
        return t;
    }

    GlobalType ReadGlobalType(Reader& r) {
        GlobalType g;
        g.type = r.valType();
        const uint8_t mut = r.u8();
        if (r.ok() && mut > 1) r.fail("malformed mutability");
        g.mut = mut == 1;
        return g;
    }

    FieldType ReadFieldType(Reader& r) {
        FieldType f;
        const uint8_t code = r.peek();
        if (r.ok() && code == 0x78) { r.u8(); f.type.pack = StorageType::Pack::I8; }
        else if (r.ok() && code == 0x77) { r.u8(); f.type.pack = StorageType::Pack::I16; }
        else f.type.val = r.valType();
        const uint8_t mut = r.u8();
        if (r.ok() && mut > 1) r.fail("malformed mutability");
        f.mut = mut == 1;
        return f;
    }

    // One type of a recursion group.
    TypeDef ReadSubType(Reader& r) {
        TypeDef t;
        uint8_t code = r.peek();
        if (!r.ok()) return t;
        if (code == 0x50 || code == 0x4F) {
            r.u8();
            t.final = code == 0x4F;
            const uint32_t n = Count(r, 1, "supertypes");
            for (uint32_t i = 0; i < n; ++i) t.supers.push_back(r.u32());
            if (r.ok() && n == 0 && false) r.fail("");
            code = r.u8();
        } else {
            code = r.u8();
        }
        if (!r.ok()) return t;
        switch (code) {
            case 0x60: {
                t.kind = TypeDef::Kind::Func;
                const uint32_t np = Count(r, 1000, "parameters");
                for (uint32_t i = 0; i < np && r.ok(); ++i) t.func.params.push_back(r.valType());
                const uint32_t nr = Count(r, 1000, "results");
                for (uint32_t i = 0; i < nr && r.ok(); ++i) t.func.results.push_back(r.valType());
                break;
            }
            case 0x5F: {
                t.kind = TypeDef::Kind::Struct;
                const uint32_t n = Count(r, 10000, "fields");
                for (uint32_t i = 0; i < n && r.ok(); ++i) t.str.fields.push_back(ReadFieldType(r));
                break;
            }
            case 0x5E:
                t.kind = TypeDef::Kind::Array;
                t.arr.element = ReadFieldType(r);
                break;
            default: r.fail("malformed type definition"); break;
        }
        return t;
    }

    void CheckTypeRefs(Reader& r, const ValType& v, uint32_t limit) {
        if (v.IsRef() && v.heap >= 0 && static_cast<uint32_t>(v.heap) >= limit) r.fail("unknown type " + std::to_string(v.heap));
    }

    void DecodeTypes(Reader& r) {
        const uint32_t n = Count(r, kMaxTypes, "types");
        uint32_t group = 0;
        uint32_t read = 0;
        while (read < n && r.ok()) {
            const uint32_t first = static_cast<uint32_t>(m_.types.size());
            uint32_t size = 1;
            if (r.peek() == 0x4E) {
                r.u8();
                size = Count(r, kMaxTypes, "types");
                if (!r.ok()) return;
                for (uint32_t i = 0; i < size && r.ok(); ++i) m_.types.push_back(ReadSubType(r));
            } else {
                m_.types.push_back(ReadSubType(r));
            }
            if (!r.ok()) return;
            if (m_.types.size() > kMaxTypes) { r.fail("too many types"); return; }
            for (uint32_t i = 0; i < size; ++i) {
                TypeDef& t = m_.types[first + i];
                t.group = group;
                t.groupStart = first;
                t.groupSize = size;
            }
            // (A type may name the ones after it in its own group, no others.)
            const uint32_t limit = first + size;
            for (uint32_t i = first; i < limit; ++i) {
                const TypeDef& t = m_.types[i];
                for (uint32_t s : t.supers) if (s >= i) r.fail("unknown type " + std::to_string(s));
                for (const ValType& v : t.func.params) CheckTypeRefs(r, v, limit);
                for (const ValType& v : t.func.results) CheckTypeRefs(r, v, limit);
                for (const FieldType& f : t.str.fields) CheckTypeRefs(r, f.type.val, limit);
                if (t.kind == TypeDef::Kind::Array) CheckTypeRefs(r, t.arr.element.type.val, limit);
            }
            ++group;
            ++read;
        }
    }

    void DecodeImports(Reader& r) {
        const uint32_t n = Count(r, kMaxImports, "imports");
        for (uint32_t i = 0; i < n && r.ok(); ++i) {
            Import im;
            im.module = Name(r);
            im.name = Name(r);
            const uint8_t kind = r.u8();
            if (!r.ok()) return;
            switch (kind) {
                case 0: {
                    im.kind = ExternKind::Func;
                    im.type = r.u32();
                    if (r.ok() && im.type >= m_.types.size()) { r.fail("unknown type"); return; }
                    m_.funcTypes.push_back(im.type);
                    ++m_.importedFuncs;
                    break;
                }
                case 1:
                    im.kind = ExternKind::Table;
                    im.table = ReadTableType(r);
                    m_.tableTypes.push_back(im.table);
                    ++m_.importedTables;
                    break;
                case 2:
                    im.kind = ExternKind::Memory;
                    im.memory.limits = ReadLimits(r, true);
                    m_.memTypes.push_back(im.memory);
                    ++m_.importedMemories;
                    break;
                case 3:
                    im.kind = ExternKind::Global;
                    im.global = ReadGlobalType(r);
                    m_.globalTypes.push_back(im.global);
                    ++m_.importedGlobals;
                    break;
                case 4: {
                    im.kind = ExternKind::Tag;
                    const uint8_t attribute = r.u8();
                    if (r.ok() && attribute != 0) { r.fail("malformed tag attribute"); return; }
                    im.type = r.u32();
                    if (r.ok() && im.type >= m_.types.size()) { r.fail("unknown type"); return; }
                    m_.tagTypes.push_back(TagType{im.type});
                    ++m_.importedTags;
                    break;
                }
                default: r.fail("malformed import kind"); return;
            }
            m_.imports.push_back(std::move(im));
        }
    }

    void DecodeFunctions(Reader& r) {
        sawFunction_ = true;
        const uint32_t n = Count(r, kMaxFunctions, "functions");
        for (uint32_t i = 0; i < n && r.ok(); ++i) {
            const uint32_t type = r.u32();
            if (r.ok() && type >= m_.types.size()) { r.fail("unknown type"); return; }
            m_.funcTypes.push_back(type);
            ++definedFuncs_;
        }
    }

    // The end of a constant expression: the instructions are read just far enough to tell where it ends (the end opcode may also be an immediate).
    ByteRange ScanConstExpr(Reader& r) {
        ByteRange range;
        range.offset = static_cast<uint32_t>(r.offset());
        while (r.ok()) {
            const uint8_t op = r.u8();
            if (!r.ok()) break;
            switch (op) {
                case 0x0B:
                    range.size = static_cast<uint32_t>(r.offset()) - range.offset;
                    return range;
                case 0x41: r.s32(); break;
                case 0x42: r.s64(); break;
                case 0x43: r.skip(4); break;
                case 0x44: r.skip(8); break;
                case 0xD0: r.heapType(); break;
                case 0xD2: case 0x23: r.u32(); break;
                case 0xFB: {
                    const uint32_t sub = r.u32();
                    if (sub == 0x00 || sub == 0x01 || sub == 0x06 || sub == 0x07) r.u32();
                    else if (sub == 0x08) { r.u32(); r.u32(); }
                    break;
                }
                case 0xFD: {
                    const uint32_t sub = r.u32();
                    if (sub == 0x0C) r.skip(16);
                    break;
                }
                default: break;  // (an opcode that has no immediate, or one that has no place here, which Validate says)
            }
        }
        range.size = 0;
        return range;
    }

    void DecodeTables(Reader& r) {
        const uint32_t n = Count(r, kMaxTables, "tables");
        for (uint32_t i = 0; i < n && r.ok(); ++i) {
            Table t;
            if (r.peek() == 0x40) {
                r.u8();
                const uint8_t zero = r.u8();
                if (r.ok() && zero != 0x00) { r.fail("malformed table"); return; }
                t.type = ReadTableType(r);
                t.hasInit = true;
                t.init = ScanConstExpr(r);
            } else {
                t.type = ReadTableType(r);
            }
            if (!r.ok()) return;
            m_.tableTypes.push_back(t.type);
            m_.tables.push_back(std::move(t));
        }
    }

    void DecodeMemories(Reader& r) {
        const uint32_t n = Count(r, kMaxMemories, "memories");
        for (uint32_t i = 0; i < n && r.ok(); ++i) {
            MemType t;
            t.limits = ReadLimits(r, true);
            if (!r.ok()) return;
            m_.memTypes.push_back(t);
        }
    }

    void DecodeTags(Reader& r) {
        const uint32_t n = Count(r, kMaxTags, "tags");
        for (uint32_t i = 0; i < n && r.ok(); ++i) {
            const uint8_t attribute = r.u8();
            if (r.ok() && attribute != 0) { r.fail("malformed tag attribute"); return; }
            const uint32_t type = r.u32();
            if (!r.ok()) return;
            if (type >= m_.types.size()) { r.fail("unknown type"); return; }
            m_.tagTypes.push_back(TagType{type});
        }
    }

    void DecodeGlobals(Reader& r) {
        const uint32_t n = Count(r, kMaxGlobals, "globals");
        for (uint32_t i = 0; i < n && r.ok(); ++i) {
            Global g;
            g.type = ReadGlobalType(r);
            g.init = ScanConstExpr(r);
            if (!r.ok()) return;
            m_.globalTypes.push_back(g.type);
            m_.globals.push_back(g);
        }
    }

    void DecodeExports(Reader& r) {
        const uint32_t n = Count(r, kMaxExports, "exports");
        for (uint32_t i = 0; i < n && r.ok(); ++i) {
            Export e;
            e.name = Name(r);
            const uint8_t kind = r.u8();
            if (r.ok() && kind > 4) { r.fail("malformed export kind"); return; }
            e.kind = static_cast<ExternKind>(kind);
            e.index = r.u32();
            if (!r.ok()) return;
            m_.exports.push_back(std::move(e));
        }
    }

    void DecodeElements(Reader& r) {
        const uint32_t n = Count(r, kMaxSegments * 100, "element segments");
        for (uint32_t i = 0; i < n && r.ok(); ++i) {
            ElemSegment e;
            const uint32_t flags = r.u32();
            if (!r.ok()) return;
            if (flags > 7) { r.fail("malformed element segment flags"); return; }
            const bool passiveOrDeclarative = flags & 1;
            const bool explicitOrDeclarative = flags & 2;
            const bool exprs = flags & 4;
            if (!passiveOrDeclarative) {
                e.mode = ElemSegment::Mode::Active;
                if (explicitOrDeclarative) e.table = r.u32();
                e.offset = ScanConstExpr(r);
            } else {
                e.mode = explicitOrDeclarative ? ElemSegment::Mode::Declarative : ElemSegment::Mode::Passive;
            }
            if (!r.ok()) return;
            if (!exprs) {
                // (A list of functions: for the ones that have an explicit table or are passive, the kind of element comes first.)
                if (passiveOrDeclarative || explicitOrDeclarative) {
                    const uint8_t kind = r.u8();
                    if (r.ok() && kind != 0x00) { r.fail("malformed element kind"); return; }
                }
                e.isFuncIndex = true;
                e.type = ValType::Ref(false, Heap::Func);
                const uint32_t count = Count(r, 0xFFFFFFFF, "elements");
                for (uint32_t k = 0; k < count && r.ok(); ++k) e.funcs.push_back(r.u32());
            } else {
                if (passiveOrDeclarative || explicitOrDeclarative) {
                    e.type = r.valType();
                    if (r.ok() && !e.type.IsRef()) { r.fail("malformed reference type"); return; }
                } else {
                    e.type = ValType::Ref(true, Heap::Func);
                }
                const uint32_t count = Count(r, 0xFFFFFFFF, "elements");
                for (uint32_t k = 0; k < count && r.ok(); ++k) e.exprs.push_back(ScanConstExpr(r));
            }
            if (!r.ok()) return;
            m_.elems.push_back(std::move(e));
        }
    }

    void DecodeCode(Reader& r) {
        sawCode_ = true;
        const uint32_t n = Count(r, kMaxFunctions, "functions");
        if (!r.ok()) return;
        if (n != definedFuncs_) { r.fail("function and code section have inconsistent lengths"); return; }
        for (uint32_t i = 0; i < n && r.ok(); ++i) {
            const uint32_t size = r.u32();
            if (!r.ok()) return;
            if (size > r.remaining()) { r.fail("function body out of bounds"); return; }
            Reader body(r.cursor(), size, r.offset());
            const uint32_t bodyOffset = static_cast<uint32_t>(r.offset());
            r.skip(size);
            FuncBody f;
            const uint32_t groups = body.u32();
            uint64_t total = 0;
            for (uint32_t g = 0; g < groups && body.ok(); ++g) {
                const uint32_t count = body.u32();
                const ValType type = body.valType();
                if (!body.ok()) break;
                total += count;
                if (total > kMaxLocals) { body.fail("local count too large"); break; }
                for (uint32_t k = 0; k < count; ++k) f.locals.push_back(type);
            }
            if (!body.ok()) { r.fail(body.error()); return; }
            f.code.offset = static_cast<uint32_t>(body.offset());
            f.code.size = bodyOffset + size - f.code.offset;
            m_.bodies.push_back(std::move(f));
        }
    }

    void DecodeData(Reader& r) {
        sawData_ = true;
        const uint32_t n = Count(r, kMaxSegments, "data segments");
        if (!r.ok()) return;
        if (m_.hasDataCount && n != m_.dataCount) { r.fail("data count and data section have inconsistent lengths"); return; }
        for (uint32_t i = 0; i < n && r.ok(); ++i) {
            DataSegment d;
            const uint32_t flags = r.u32();
            if (!r.ok()) return;
            if (flags > 2) { r.fail("malformed data segment flags"); return; }
            if (flags == 0 || flags == 2) {
                d.active = true;
                if (flags == 2) d.memory = r.u32();
                d.offset = ScanConstExpr(r);
            }
            const uint32_t size = r.u32();
            if (!r.ok()) return;
            d.bytes.offset = static_cast<uint32_t>(r.offset());
            d.bytes.size = size;
            r.skip(size);
            if (!r.ok()) return;
            m_.datas.push_back(d);
        }
    }

    void DecodeSection(uint8_t id, Reader& r, size_t start) {
        (void)start;
        switch (id) {
            case kCustom: {
                CustomSection c;
                c.name = Name(r);
                if (!r.ok()) return;
                c.bytes.offset = static_cast<uint32_t>(r.offset());
                c.bytes.size = static_cast<uint32_t>(r.remaining());
                r.skip(r.remaining());
                m_.customs.push_back(std::move(c));
                break;
            }
            case kType: DecodeTypes(r); break;
            case kImport: DecodeImports(r); break;
            case kFunction: DecodeFunctions(r); break;
            case kTable: DecodeTables(r); break;
            case kMemory: DecodeMemories(r); break;
            case kTag: DecodeTags(r); break;
            case kGlobal: DecodeGlobals(r); break;
            case kExport: DecodeExports(r); break;
            case kStart:
                m_.hasStart = true;
                m_.start = r.u32();
                break;
            case kElement: DecodeElements(r); break;
            case kDataCount:
                m_.hasDataCount = true;
                m_.dataCount = r.u32();
                break;
            case kCode: DecodeCode(r); break;
            case kData: DecodeData(r); break;
            default: r.fail("unknown section"); break;
        }
    }

    bool Finish() {
        if (!sawCode_ && definedFuncs_ != 0) {
            error_ = "function and code section have inconsistent lengths";
            return false;
        }
        if (m_.hasDataCount && !sawData_ && m_.dataCount != 0) {
            error_ = "data count and data section have inconsistent lengths";
            return false;
        }
        return true;
    }
};

}  // namespace

bool Decode(const uint8_t* data, size_t size, Module& out, std::string& error) {
    out = Module{};
    out.bytes.assign(data, data + size);
    Decoder decoder(out);
    if (decoder.Run()) return true;
    error = decoder.error();
    return false;
}

}  // namespace Quanta::Wasm
