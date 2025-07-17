#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include "Value.h"

namespace Quanta {

class Context;

/**
 * JavaScript Symbol implementation
 * Symbols are unique identifiers that can be used as object keys
 */
class Symbol {
private:
    std::string description_;
    static uint64_t next_id_;
    uint64_t id_;
    
    // Well-known symbols registry
    static std::unordered_map<std::string, std::unique_ptr<Symbol>> well_known_symbols_;
    
    // Global symbol registry
    static std::unordered_map<std::string, std::unique_ptr<Symbol>> global_registry_;
    
    Symbol(const std::string& description);
    
public:
    ~Symbol() = default;
    
    // Create new symbol
    static std::unique_ptr<Symbol> create(const std::string& description = "");
    
    // Get or create global symbol
    static Symbol* for_key(const std::string& key);
    
    // Get key for global symbol
    static std::string key_for(Symbol* symbol);
    
    // Get well-known symbol
    static Symbol* get_well_known(const std::string& name);
    
    // Initialize well-known symbols
    static void initialize_well_known_symbols();
    
    // Symbol properties
    std::string get_description() const { return description_; }
    uint64_t get_id() const { return id_; }
    std::string to_string() const;
    
    // Comparison
    bool equals(const Symbol* other) const;
    
    // Built-in Symbol methods
    static Value symbol_constructor(Context& ctx, const std::vector<Value>& args);
    static Value symbol_for(Context& ctx, const std::vector<Value>& args);
    static Value symbol_key_for(Context& ctx, const std::vector<Value>& args);
    static Value symbol_to_string(Context& ctx, const std::vector<Value>& args);
    static Value symbol_value_of(Context& ctx, const std::vector<Value>& args);
    
    // Well-known symbol names
    static const std::string ITERATOR;
    static const std::string ASYNC_ITERATOR;
    static const std::string MATCH;
    static const std::string REPLACE;
    static const std::string SEARCH;
    static const std::string SPLIT;
    static const std::string HAS_INSTANCE;
    static const std::string IS_CONCAT_SPREADABLE;
    static const std::string SPECIES;
    static const std::string TO_PRIMITIVE;
    static const std::string TO_STRING_TAG;
    static const std::string UNSCOPABLES;
};

} // namespace Quanta