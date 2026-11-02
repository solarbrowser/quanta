/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_WASM_MODULE_H
#define QUANTA_WASM_MODULE_H

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "quanta/wasm/Code.h"
#include "quanta/wasm/Types.h"

namespace Quanta::Wasm {

// A range of the module's bytes (a constant expression, a function body, the bytes of a segment).
struct ByteRange {
    uint32_t offset = 0;
    uint32_t size = 0;
    int32_t compiled = -1;  // (a constant expression: its place in Module::constExprs, once validated)
};

struct Import {
    std::string module, name;
    ExternKind kind = ExternKind::Func;
    uint32_t type = 0;  // (a function or a tag: the index of its type)
    TableType table;
    MemType memory;
    GlobalType global;
};

struct Export {
    std::string name;
    ExternKind kind = ExternKind::Func;
    uint32_t index = 0;
};

struct Global {
    GlobalType type;
    ByteRange init;
};

struct Table {
    TableType type;
    bool hasInit = false;  // a table with an initial value of its own (the type allows no null)
    ByteRange init;
};

struct ElemSegment {
    enum class Mode : uint8_t { Active, Passive, Declarative };
    Mode mode = Mode::Passive;
    ValType type = ValType::Ref(true, Heap::Func);
    uint32_t table = 0;
    ByteRange offset;
    // The items: function indices (isFuncIndex) or constant expressions.
    bool isFuncIndex = false;
    std::vector<uint32_t> funcs;
    std::vector<ByteRange> exprs;
    uint32_t count() const { return static_cast<uint32_t>(isFuncIndex ? funcs.size() : exprs.size()); }
};

struct DataSegment {
    bool active = false;
    uint32_t memory = 0;
    ByteRange offset;
    ByteRange bytes;
};

struct CustomSection {
    std::string name;
    ByteRange bytes;  // (what follows the name)
};

struct FuncBody {
    ByteRange code;  // (the instructions, after the locals)
    std::vector<ValType> locals;  // (the declared ones, not the parameters)
};

// A module as the binary format decodes. Functions, tables, memories, globals and tags are numbered with the imported ones first.
struct Module {
    std::vector<uint8_t> bytes;
    std::vector<TypeDef> types;
    std::vector<Import> imports;
    // (The indices of the whole index space.)
    std::vector<uint32_t> funcTypes;  // the type of each function, imported and defined
    std::vector<TableType> tableTypes;
    std::vector<MemType> memTypes;
    std::vector<GlobalType> globalTypes;
    std::vector<TagType> tagTypes;
    uint32_t importedFuncs = 0, importedTables = 0, importedMemories = 0, importedGlobals = 0, importedTags = 0;
    std::vector<Table> tables;    // (the defined ones)
    std::vector<Global> globals;  // (the defined ones)
    std::vector<Export> exports;
    bool hasStart = false;
    uint32_t start = 0;
    std::vector<ElemSegment> elems;
    std::vector<DataSegment> datas;
    bool hasDataCount = false;
    uint32_t dataCount = 0;
    std::vector<FuncBody> bodies;
    std::vector<CustomSection> customs;
    // The functions that are named in a place that makes a reference to them valid (an export, an element segment, a global's initializer).
    std::vector<bool> declaredFuncs;

    // What validation makes of it.
    std::vector<uint32_t> typeIds;      // the canonical id of each type
    std::vector<uint32_t> funcSigs;     // the canonical id of the type of each function, imported and defined
    std::vector<uint32_t> tagSigs;      // and of each tag
    std::vector<CompiledFunction> compiled;    // the defined functions
    std::vector<CompiledFunction> constExprs;  // the constant expressions

    uint32_t numFuncs() const { return static_cast<uint32_t>(funcTypes.size()); }
};

}  // namespace Quanta::Wasm

#endif
