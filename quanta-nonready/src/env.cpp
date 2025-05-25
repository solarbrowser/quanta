//<---------QUANTA JS ENGINE - ENVIRONMENT IMPLEMENTATION--------->
// Stage 1: Core Engine & Runtime - Scope System
// Purpose: Handle variable scoping, hoisting, and environment chains
// Max Lines: 5000 (Current: ~200)

#include "../include/env.h"
#include <stdexcept>

namespace Quanta {

//<---------ENVIRONMENT CONSTRUCTOR--------->
Environment::Environment(std::shared_ptr<Environment> parentEnv)
    : parent(parentEnv) {}

//<---------VARIABLE OPERATIONS--------->
void Environment::define(const std::string& name, const JSValue& value, bool isConst) {
    if (variables.find(name) != variables.end()) {
        throw std::runtime_error("Variable '" + name + "' already defined in this scope");
    }
    variables[name] = Variable(value, isConst);
}

void Environment::assign(const std::string& name, const JSValue& value) {
    auto it = variables.find(name);
    if (it != variables.end()) {
        if (it->second.isConst) {
            throw std::runtime_error("Cannot assign to const variable '" + name + "'");
        }
        it->second.value = value;
        it->second.isInitialized = true;
        return;
    }
    
    // Check parent scopes
    if (parent) {
        parent->assign(name, value);
        return;
    }
    
    throw std::runtime_error("Undefined variable '" + name + "'");
}

JSValue Environment::get(const std::string& name) {
    auto it = variables.find(name);
    if (it != variables.end()) {
        if (!it->second.isInitialized) {
            throw std::runtime_error("Variable '" + name + "' used before initialization");
        }
        return it->second.value;
    }
    
    // Check parent scopes
    if (parent) {
        return parent->get(name);
    }
    
    throw std::runtime_error("Undefined variable '" + name + "'");
}

bool Environment::has(const std::string& name) {
    if (variables.find(name) != variables.end()) {
        return true;
    }
    
    if (parent) {
        return parent->has(name);
    }
    
    return false;
}

//<---------SCOPE OPERATIONS--------->
std::shared_ptr<Environment> Environment::createChild() {
    return std::make_shared<Environment>(shared_from_this());
}

std::shared_ptr<Environment> Environment::getParent() const {
    return parent;
}

//<---------HOISTING SUPPORT--------->
void Environment::hoist(const std::string& name) {
    if (variables.find(name) == variables.end()) {
        variables[name] = Variable(); // Uninitialized variable
    }
}

void Environment::initialize(const std::string& name, const JSValue& value) {
    auto it = variables.find(name);
    if (it != variables.end()) {
        it->second.value = value;
        it->second.isInitialized = true;
    } else {
        throw std::runtime_error("Cannot initialize undefined variable '" + name + "'");
    }
}

//<---------SCOPE MANAGER IMPLEMENTATION--------->
ScopeManager::ScopeManager() {
    global = std::make_shared<Environment>();
    current = global;
}

void ScopeManager::enterScope() {
    current = std::make_shared<Environment>(current);
}

void ScopeManager::exitScope() {
    if (current->getParent()) {
        current = current->getParent();
    }
}

Environment& ScopeManager::getCurrentScope() {
    return *current;
}

Environment& ScopeManager::getGlobalScope() {
    return *global;
}

//<---------CONVENIENCE METHODS--------->
void ScopeManager::defineVariable(const std::string& name, const JSValue& value, bool isConst) {
    current->define(name, value, isConst);
}

void ScopeManager::assignVariable(const std::string& name, const JSValue& value) {
    current->assign(name, value);
}

JSValue ScopeManager::getVariable(const std::string& name) {
    return current->get(name);
}

} // namespace Quanta
