/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/runtime/Symbol.h"
#include "quanta/core/engine/Context.h"
#include <sstream>
#include <atomic>

namespace Quanta {

uint64_t Symbol::next_id_ = 1;
std::unordered_map<std::string, std::unique_ptr<Symbol>> Symbol::well_known_symbols_;
std::unordered_map<std::string, std::unique_ptr<Symbol>> Symbol::global_registry_;

const std::string Symbol::ITERATOR = "Symbol.iterator";
const std::string Symbol::ASYNC_ITERATOR = "Symbol.asyncIterator";
const std::string Symbol::MATCH = "Symbol.match";
const std::string Symbol::REPLACE = "Symbol.replace";
const std::string Symbol::SEARCH = "Symbol.search";
const std::string Symbol::SPLIT = "Symbol.split";
const std::string Symbol::HAS_INSTANCE = "Symbol.hasInstance";
const std::string Symbol::IS_CONCAT_SPREADABLE = "Symbol.isConcatSpreadable";
const std::string Symbol::SPECIES = "Symbol.species";
const std::string Symbol::TO_PRIMITIVE = "Symbol.toPrimitive";
const std::string Symbol::TO_STRING_TAG = "Symbol.toStringTag";
const std::string Symbol::UNSCOPABLES = "Symbol.unscopables";

Symbol::Symbol(const std::string& description) : description_(description), id_(next_id_++) {}

std::unique_ptr<Symbol> Symbol::create(const std::string& description) {
    return std::unique_ptr<Symbol>(new Symbol(description));
}

Symbol* Symbol::for_key(const std::string& key) {
    auto it = global_registry_.find(key);
    if (it != global_registry_.end()) {
        return it->second.get();
    }
    
    auto symbol = create(key);
    Symbol* ptr = symbol.get();
    global_registry_[key] = std::move(symbol);
    return ptr;
}

std::string Symbol::key_for(Symbol* symbol) {
    for (const auto& pair : global_registry_) {
        if (pair.second->equals(symbol)) {
            return pair.first;
        }
    }
    return "";
}

Symbol* Symbol::get_well_known(const std::string& name) {
    auto it = well_known_symbols_.find(name);
    if (it != well_known_symbols_.end()) {
        return it->second.get();
    }
    return nullptr;
}

void Symbol::initialize_well_known_symbols() {
    well_known_symbols_[ITERATOR] = create(ITERATOR);
    well_known_symbols_[ASYNC_ITERATOR] = create(ASYNC_ITERATOR);
    well_known_symbols_[MATCH] = create(MATCH);
    well_known_symbols_[REPLACE] = create(REPLACE);
    well_known_symbols_[SEARCH] = create(SEARCH);
    well_known_symbols_[SPLIT] = create(SPLIT);
    well_known_symbols_[HAS_INSTANCE] = create(HAS_INSTANCE);
    well_known_symbols_[IS_CONCAT_SPREADABLE] = create(IS_CONCAT_SPREADABLE);
    well_known_symbols_[SPECIES] = create(SPECIES);
    well_known_symbols_[TO_PRIMITIVE] = create(TO_PRIMITIVE);
    well_known_symbols_[TO_STRING_TAG] = create(TO_STRING_TAG);
    well_known_symbols_[UNSCOPABLES] = create(UNSCOPABLES);
}

std::string Symbol::to_string() const {
    std::ostringstream oss;
    oss << "Symbol(";
    if (!description_.empty()) {
        oss << description_;
    }
    oss << ")";
    return oss.str();
}

std::string Symbol::to_property_key() const {
    // Symbols as property keys must be unique based on their ID
    // Use a special prefix to distinguish from string keys
    std::ostringstream oss;
    oss << "@@symbol_" << id_;
    return oss.str();
}

bool Symbol::equals(const Symbol* other) const {
    return other && id_ == other->id_;
}

Value Symbol::symbol_constructor(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    std::string description = "";
    if (!args.empty() && !args[0].is_undefined()) {
        description = args[0].to_string();
    }
    
    auto symbol = create(description);
    return Value(symbol.release());
}

Value Symbol::symbol_for(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        ctx.throw_exception(Value("Symbol.for requires a key argument"));
        return Value();
    }
    
    std::string key = args[0].to_string();
    Symbol* symbol = for_key(key);
    return Value(symbol);
}

Value Symbol::symbol_key_for(Context& ctx, const std::vector<Value>& args) {
    if (args.empty() || !args[0].is_symbol()) {
        ctx.throw_exception(Value("Symbol.keyFor requires a symbol argument"));
        return Value();
    }
    
    Symbol* symbol = args[0].as_symbol();
    std::string key = key_for(symbol);
    if (key.empty()) {
        return Value();
    }
    return Value(key);
}

Value Symbol::symbol_to_string(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    
    Value this_value = ctx.get_binding("this");
    if (!this_value.is_symbol()) {
        ctx.throw_exception(Value("Symbol.prototype.toString called on non-symbol"));
        return Value();
    }
    
    Symbol* symbol = this_value.as_symbol();
    return Value(symbol->to_string());
}

Value Symbol::symbol_value_of(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    
    Value this_value = ctx.get_binding("this");
    if (!this_value.is_symbol()) {
        ctx.throw_exception(Value("Symbol.prototype.valueOf called on non-symbol"));
        return Value();
    }
    
    return this_value;
}

}
