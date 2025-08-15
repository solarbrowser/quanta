/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_WEBAPI_INTERFACE_H
#define QUANTA_WEBAPI_INTERFACE_H

#include "Value.h"
#include <functional>
#include <string>
#include <vector>
#include <unordered_map>

namespace Quanta {

class Context;

/**
 * Interface for external Web API implementations
 * Allows browser/host environment to provide Web APIs without polluting the core JS engine
 */
class WebAPIInterface {
public:
    using APIFunction = std::function<Value(Context&, const std::vector<Value>&)>;
    
    virtual ~WebAPIInterface() = default;
    
    // Register a Web API function that can be called from JavaScript
    virtual void registerAPI(const std::string& name, APIFunction func) = 0;
    
    // Check if an API is available
    virtual bool hasAPI(const std::string& name) const = 0;
    
    // Call a registered API function
    virtual Value callAPI(const std::string& name, Context& ctx, const std::vector<Value>& args) = 0;
    
    // Get all available API names
    virtual std::vector<std::string> getAvailableAPIs() const = 0;
};

/**
 * Default implementation of WebAPIInterface
 */
class DefaultWebAPIInterface : public WebAPIInterface {
private:
    std::unordered_map<std::string, APIFunction> api_functions_;
    
public:
    void registerAPI(const std::string& name, APIFunction func) override {
        api_functions_[name] = func;
    }
    
    bool hasAPI(const std::string& name) const override {
        return api_functions_.find(name) != api_functions_.end();
    }
    
    Value callAPI(const std::string& name, Context& ctx, const std::vector<Value>& args) override {
        auto it = api_functions_.find(name);
        if (it != api_functions_.end()) {
            return it->second(ctx, args);
        }
        return Value(); // undefined
    }
    
    std::vector<std::string> getAvailableAPIs() const override {
        std::vector<std::string> names;
        for (const auto& pair : api_functions_) {
            names.push_back(pair.first);
        }
        return names;
    }
};

} // namespace Quanta

#endif // QUANTA_WEBAPI_INTERFACE_H