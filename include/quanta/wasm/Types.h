/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_WASM_TYPES_H
#define QUANTA_WASM_TYPES_H

#include <cstdint>
#include <string>
#include <vector>

// WebAssembly 3.0 (core, GC, exceptions, memory64, multi-memory, tail calls, extended constants, typed function
// references, SIMD, relaxed SIMD): the types of the binary format and of a decoded module.
namespace Quanta::Wasm {

// A heap type is the index of a defined type (>= 0) or one of the abstract ones (negative).
using HeapType = int32_t;
namespace Heap {
constexpr HeapType Func = -1, Extern = -2, Any = -3, Eq = -4, I31 = -5, Struct = -6, Array = -7, Exn = -8, None = -9, NoExtern = -10, NoFunc = -11, NoExn = -12;
}

struct ValType {
    enum class Kind : uint8_t { I32, I64, F32, F64, V128, Ref, Bottom };
    Kind kind = Kind::Bottom;
    bool nullable = false;  // (a Ref only)
    HeapType heap = 0;      // (a Ref only)

    static ValType I32() { return {Kind::I32, false, 0}; }
    static ValType I64() { return {Kind::I64, false, 0}; }
    static ValType F32() { return {Kind::F32, false, 0}; }
    static ValType F64() { return {Kind::F64, false, 0}; }
    static ValType V128() { return {Kind::V128, false, 0}; }
    static ValType Ref(bool nullable, HeapType heap) { return {Kind::Ref, nullable, heap}; }
    // What an unreachable stack position has: it matches any type.
    static ValType Bottom() { return {Kind::Bottom, false, 0}; }

    bool IsRef() const { return kind == Kind::Ref; }
    bool IsNum() const { return kind == Kind::I32 || kind == Kind::I64 || kind == Kind::F32 || kind == Kind::F64; }
    bool IsBottom() const { return kind == Kind::Bottom; }
    bool operator==(const ValType& o) const { return kind == o.kind && (kind != Kind::Ref || (nullable == o.nullable && heap == o.heap)); }
    bool operator!=(const ValType& o) const { return !(*this == o); }
};

// The type of a field of a struct or array: a value type or a packed one.
struct StorageType {
    enum class Pack : uint8_t { None, I8, I16 };
    ValType val;
    Pack pack = Pack::None;
    bool operator==(const StorageType& o) const { return pack == o.pack && (pack != Pack::None || val == o.val); }
    // What a read of it gives (a packed field reads as an i32).
    ValType Unpacked() const { return pack == Pack::None ? val : ValType::I32(); }
};

struct FieldType {
    StorageType type;
    bool mut = false;
    bool operator==(const FieldType& o) const { return mut == o.mut && type == o.type; }
};

struct FuncType {
    std::vector<ValType> params, results;
};

struct StructType {
    std::vector<FieldType> fields;
};

struct ArrayType {
    FieldType element;
};

// One entry of the type section: a type of a recursion group, with its supertypes.
struct TypeDef {
    enum class Kind : uint8_t { Func, Struct, Array };
    Kind kind = Kind::Func;
    FuncType func;
    StructType str;
    ArrayType arr;
    bool final = true;
    std::vector<uint32_t> supers;  // (at most one in this version of the format)
    uint32_t group = 0;            // the recursion group it belongs to
    uint32_t groupStart = 0;       // the index of the first type of its group
    uint32_t groupSize = 1;
};

struct Limits {
    uint64_t min = 0;
    uint64_t max = 0;
    bool hasMax = false;
    bool is64 = false;
    bool shared = false;
};

struct TableType {
    ValType elem;
    Limits limits;
};

struct MemType {
    Limits limits;
    uint32_t pageSizeLog2 = 16;
};

struct GlobalType {
    ValType type;
    bool mut = false;
};

struct TagType {
    uint32_t type = 0;  // the function type it takes (its results are empty)
};

enum class ExternKind : uint8_t { Func = 0, Table = 1, Memory = 2, Global = 3, Tag = 4 };

}  // namespace Quanta::Wasm

#endif
