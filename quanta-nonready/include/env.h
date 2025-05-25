//<---------QUANTA JS ENGINE - ENVIRONMENT HEADER--------->
// Stage 1: Core Engine & Runtime - Scope System
// Purpose: Handle variable scoping, hoisting, and environment chains
// Max Lines: 5000 (Current: ~120)

#ifndef QUANTA_ENV_H
#define QUANTA_ENV_H

#include <string>
#include "hash_workaround.h"
#include <memory>
#include <variant>

namespace Quanta {

//<---------VALUE TYPES--------->
using JSValue = std::variant<
    double,           // Number
    std::string,      // String
    bool,             // Boolean
    std::nullptr_t    // Null/Undefined
>;

//<---------VARIABLE INFO--------->
struct Variable {
    JSValue value;
    bool isConst;
    bool isInitialized;
    
    Variable(const JSValue& v, bool c = false) 
        : value(v), isConst(c), isInitialized(true) {}
    
    Variable() : value(nullptr), isConst(false), isInitialized(false) {}
};

//<---------ENVIRONMENT CLASS--------->
class Environment : public std::enable_shared_from_this<Environment> {
private:
    SimpleMap<std::string, Variable> variables;
    std::shared_ptr<Environment> parent;
    
public:
    Environment(std::shared_ptr<Environment> parentEnv = nullptr);
    
    // Variable operations
    void define(const std::string& name, const JSValue& value, bool isConst = false);
    void assign(const std::string& name, const JSValue& value);
    JSValue get(const std::string& name);
    bool has(const std::string& name);
    
    // Scope operations
    std::shared_ptr<Environment> createChild();
    std::shared_ptr<Environment> getParent() const;
    
    // Hoisting support
    void hoist(const std::string& name);
    void initialize(const std::string& name, const JSValue& value);
};

//<---------SCOPE MANAGER--------->
class ScopeManager {
private:
    std::shared_ptr<Environment> current;
    std::shared_ptr<Environment> global;
    
public:
    ScopeManager();
    
    void enterScope();
    void exitScope();
    
    Environment& getCurrentScope();
    Environment& getGlobalScope();
    
    // Convenience methods
    void defineVariable(const std::string& name, const JSValue& value, bool isConst = false);
    void assignVariable(const std::string& name, const JSValue& value);
    JSValue getVariable(const std::string& name);
};

} // namespace Quanta

#endif // QUANTA_ENV_H
