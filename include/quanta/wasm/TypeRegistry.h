/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_WASM_TYPEREGISTRY_H
#define QUANTA_WASM_TYPEREGISTRY_H

#include <cstdint>
#include <vector>

#include "quanta/wasm/Module.h"
#include "quanta/wasm/Types.h"

namespace Quanta::Wasm {

// A defined type as the whole program knows it: two types of different modules are the same type when their recursion groups are the same
// (iso-recursive types), and that is told by one number. The references inside are by those numbers too.
struct CanonType {
    TypeDef::Kind kind = TypeDef::Kind::Func;
    FuncType func;
    StructType str;
    ArrayType arr;
    bool final = true;
    int64_t super = -1;  // the canonical id of its supertype, or -1
    uint32_t depth = 0;  // how many supertypes there are above it

    // How an instance is laid out: a struct has numbers in slots of 64 bits (a vector in two) and references in a list, and says for each
    // field which and where; an array has the bytes of its elements (or its references) in one.
    std::vector<uint32_t> fieldSlot;
    std::vector<uint8_t> fieldIsRef;
    uint32_t numSlots = 0, numRefs = 0;
    uint32_t elemBytes = 0;  // 1, 2, 4, 8 or 16
    bool elemIsRef = false;
};

namespace TypeRegistry {

// The canonical type with this id.
const CanonType& Get(uint32_t id);

// Makes the types of a module canonical: ids[i] is the id of its type i. False (with the reason) if a type does not match its supertype.
bool Canonicalize(const Module& module, std::vector<uint32_t>& ids, std::string& error);

// The same type with the heap types of its references as canonical ids (from a module whose types are numbered as `ids` say).
ValType Map(const ValType& type, const std::vector<uint32_t>& ids);

// Whether `a` is a subtype of `b` (both with canonical ids).
bool HeapSubtype(HeapType a, HeapType b);
bool Matches(const ValType& a, const ValType& b);
bool Matches(const StorageType& a, const StorageType& b);

// The root of the hierarchy a heap type is in: func, extern, any or exn.
HeapType TopOf(HeapType h);

// Whether two function types (canonical) are the same.
bool SameSignature(uint32_t a, uint32_t b);

}  // namespace TypeRegistry

}  // namespace Quanta::Wasm

#endif
