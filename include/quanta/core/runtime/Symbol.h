/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <string>
#include <span>
#include <vector>
#include <unordered_map>
#include <memory>
#include "quanta/core/runtime/Value.h"

namespace Quanta {

class Context;

class Symbol {
private:
    std::string description_;
    bool has_description_ = false;
    static constinit thread_local uint64_t next_id_;
    uint64_t id_;
    // to_property_key()'s answer, built once. id_ never changes once a
    // Symbol exists, so "@@sym:" + the id is the same string for the rest
    // of the Symbol's life -- every further computed-key access naming this
    // Symbol (o[sym] read or write, defineProperty, ownKeys) was rebuilding
    // it via std::to_string from scratch. Empty means not built yet; a
    // well-known symbol keyed by its description (the common early-return
    // in to_property_key()) never touches this field at all.
    mutable std::string property_key_cache_;

    // Thread-local: each agent has its own well-known/registered symbols,
    // matching real engines' per-isolate Symbol.for() scoping.
    static thread_local std::unordered_map<std::string, std::unique_ptr<Symbol>> well_known_symbols_;

    static thread_local std::unordered_map<std::string, std::unique_ptr<Symbol>> global_registry_;

    // Registry of all user-created symbols by property key for getOwnPropertySymbols/Reflect.ownKeys
    static thread_local std::unordered_map<std::string, Symbol*> user_symbol_registry_;

    Symbol(const std::string& description);

public:
    // GC cell protocol (see Object.h).
    static void* operator new(size_t size);
    static void  operator delete(void* p) noexcept;
    static void* operator new[](size_t) = delete;
    static void  operator delete[](void*) = delete;

    ~Symbol() = default;

    static std::unique_ptr<Symbol> create(const std::string& description = "", bool has_description = true);
    
    static Symbol* for_key(const std::string& key);
    
    static std::string key_for(Symbol* symbol);
    
    static Symbol* get_well_known(const std::string& name);
    
    static void initialize_well_known_symbols();
    // GC roots: registries own symbols that property-key strings resolve
    // back into, so they stay strongly rooted.
    static void gc_trace_roots(class Visitor& v);
    
    std::string get_description() const { return description_; }
    bool get_has_description() const { return has_description_; }
    uint64_t get_id() const { return id_; }
    std::string to_string() const;
    std::string to_property_key() const;

    bool equals(const Symbol* other) const;

    // Find a user-created symbol by its property key (e.g., "@@sym:5")
    static Symbol* find_by_property_key(const std::string& key);
    
    static Value symbol_constructor(Context& ctx, std::span<const Value> args, Value receiver);
    static Value symbol_for(Context& ctx, std::span<const Value> args, Value receiver);
    static Value symbol_key_for(Context& ctx, std::span<const Value> args, Value receiver);
    static Value symbol_to_string(Context& ctx, std::span<const Value> args, Value receiver);
    static Value symbol_value_of(Context& ctx, std::span<const Value> args, Value receiver);
    
    static const std::string ITERATOR;
    static const std::string ASYNC_ITERATOR;
    static const std::string MATCH;
    static const std::string MATCH_ALL;
    static const std::string REPLACE;
    static const std::string SEARCH;
    static const std::string SPLIT;
    static const std::string HAS_INSTANCE;
    static const std::string IS_CONCAT_SPREADABLE;
    static const std::string SPECIES;
    static const std::string TO_PRIMITIVE;
    static const std::string TO_STRING_TAG;
    static const std::string UNSCOPABLES;
    static const std::string DISPOSE;
    static const std::string ASYNC_DISPOSE;
};

}
