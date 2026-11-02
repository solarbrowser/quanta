/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_WASM_INTERNAL_H
#define QUANTA_WASM_INTERNAL_H

#include <string>
#include <vector>

#include "quanta/wasm/Module.h"
#include "quanta/wasm/TypeRegistry.h"

namespace Quanta::Wasm {

// What validating the code of a module needs to know about the rest of it, with every type canonical.
struct ModuleContext {
    const Module& module;
    std::vector<uint32_t> typeIds;
    std::vector<uint32_t> funcSigs;
    std::vector<TableType> tables;
    std::vector<MemType> memories;
    std::vector<GlobalType> globals;
    std::vector<uint32_t> tagSigs;
    std::vector<ValType> elemTypes;
    std::vector<bool> declared;  // the functions a ref.func may name
    explicit ModuleContext(const Module& m) : module(m) {}

    ValType Canon(const ValType& v) const { return TypeRegistry::Map(v, typeIds); }
};

// Checks the body of a defined function (its index in the whole space of functions) and makes its code.
bool CompileFunction(ModuleContext& cx, uint32_t funcIndex, CompiledFunction& out, std::string& error);

// Checks a constant expression to give a value of `expected`, with only the globals before `globalLimit` to read, and makes its code.
bool CompileConstExpr(ModuleContext& cx, const ByteRange& range, const ValType& expected, uint32_t globalLimit, CompiledFunction& out, std::string& error);

}  // namespace Quanta::Wasm

#endif
