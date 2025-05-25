// Stage 2: Interpreter Engine - Runtime Object System Implementation
// Purpose: JavaScript runtime objects and built-in types implementation
// Max Lines: 5000 (Current: ~500)

#include "../include/runtime_objects.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <random>
#include <limits>
#include <limits>

namespace Quanta {

//<---------BASE JAVASCRIPT OBJECT IMPLEMENTATION--------->
void JSObject::setProperty(const std::string& name, const JSValue& value) {
    properties[name] = value;
}

JSValue JSObject::getProperty(const std::string& name) {
    auto it = properties.find(name);
    if (it != properties.end()) {
        return it->second;
    }
    return nullptr; // undefined
}

bool JSObject::hasProperty(const std::string& name) {
    return properties.find(name) != properties.end();
}

void JSObject::deleteProperty(const std::string& name) {
    properties.erase(name);
}

std::vector<std::string> JSObject::getPropertyNames() {
    std::vector<std::string> names;
    for (const auto& pair : properties) {
        names.push_back(pair.first);
    }
    return names;
}

std::string JSObject::toString() {
    return "[object Object]";
}

//<---------JAVASCRIPT ARRAY IMPLEMENTATION--------->
JSArray::JSArray(const std::vector<JSValue>& elements) : elements(elements) {
    // Set length property
    setProperty("length", static_cast<double>(elements.size()));
}

void JSArray::push(const JSValue& value) {
    elements.push_back(value);
    setProperty("length", static_cast<double>(elements.size()));
}

JSValue JSArray::pop() {
    if (elements.empty()) {
        return nullptr; // undefined
    }
    
    JSValue value = elements.back();
    elements.pop_back();
    setProperty("length", static_cast<double>(elements.size()));
    return value;
}

JSValue JSArray::shift() {
    if (elements.empty()) {
        return nullptr; // undefined
    }
    
    JSValue value = elements.front();
    elements.erase(elements.begin());
    setProperty("length", static_cast<double>(elements.size()));
    return value;
}

void JSArray::unshift(const JSValue& value) {
    elements.insert(elements.begin(), value);
    setProperty("length", static_cast<double>(elements.size()));
}

JSValue JSArray::get(size_t index) {
    if (index >= elements.size()) {
        return nullptr; // undefined
    }
    return elements[index];
}

void JSArray::set(size_t index, const JSValue& value) {
    if (index >= elements.size()) {
        elements.resize(index + 1, nullptr);
    }
    elements[index] = value;
    setProperty("length", static_cast<double>(elements.size()));
}

std::string JSArray::toString() {
    std::stringstream ss;
    ss << "[";
    for (size_t i = 0; i < elements.size(); ++i) {
        if (i > 0) ss << ", ";
        ss << Quanta::toString(elements[i]);
    }
    ss << "]";
    return ss.str();
}

//<---------JAVASCRIPT FUNCTION IMPLEMENTATION--------->
JSFunction::JSFunction(const std::string& name, NativeFunction func)
    : name(name), nativeFunction(func) {
    setProperty("name", name);
}

JSValue JSFunction::call(const std::vector<JSValue>& args) {
    if (nativeFunction) {
        return nativeFunction(args);
    }
    return nullptr; // undefined
}

std::string JSFunction::toString() {
    return "function " + name + "() { [native code] }";
}

//<---------CONSOLE OBJECT IMPLEMENTATION--------->
ConsoleObject::ConsoleObject() {
    // Set up console methods
    auto logFunc = std::make_shared<JSFunction>("log", 
        [this](const std::vector<JSValue>& args) { return log(args); });
    
    auto errorFunc = std::make_shared<JSFunction>("error", 
        [this](const std::vector<JSValue>& args) { return error(args); });
    
    auto warnFunc = std::make_shared<JSFunction>("warn", 
        [this](const std::vector<JSValue>& args) { return warn(args); });
    
    auto infoFunc = std::make_shared<JSFunction>("info", 
        [this](const std::vector<JSValue>& args) { return info(args); });
    
    // Note: For now, we'll store these as nullptr in properties
    // In a full implementation, we'd need a way to store function objects
    setProperty("log", nullptr);
    setProperty("error", nullptr);
    setProperty("warn", nullptr);
    setProperty("info", nullptr);
}

JSValue ConsoleObject::log(const std::vector<JSValue>& args) {
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) std::cout << " ";
        std::cout << Quanta::toString(args[i]);
    }
    std::cout << std::endl;
    return nullptr; // undefined
}

JSValue ConsoleObject::error(const std::vector<JSValue>& args) {
    std::cerr << "[ERROR] ";
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) std::cerr << " ";
        std::cerr << Quanta::toString(args[i]);
    }
    std::cerr << std::endl;
    return nullptr; // undefined
}

JSValue ConsoleObject::warn(const std::vector<JSValue>& args) {
    std::cout << "[WARN] ";
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) std::cout << " ";
        std::cout << Quanta::toString(args[i]);
    }
    std::cout << std::endl;
    return nullptr; // undefined
}

JSValue ConsoleObject::info(const std::vector<JSValue>& args) {
    std::cout << "[INFO] ";
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) std::cout << " ";
        std::cout << Quanta::toString(args[i]);
    }
    std::cout << std::endl;
    return nullptr; // undefined
}

//<---------MATH OBJECT IMPLEMENTATION--------->
MathObject::MathObject() {
    // Set up Math constants
    setProperty("PI", PI);
    setProperty("E", E);
    
    // Set up Math methods - for now store as nullptr placeholders
    setProperty("abs", nullptr);
    setProperty("floor", nullptr);
    setProperty("ceil", nullptr);
    setProperty("round", nullptr);
    setProperty("max", nullptr);
    setProperty("min", nullptr);
    setProperty("pow", nullptr);
    setProperty("sqrt", nullptr);
    setProperty("random", nullptr);
}

JSValue MathObject::abs(const std::vector<JSValue>& args) {
    if (args.empty()) return std::numeric_limits<double>::quiet_NaN();
    double num = toNumber(args[0]);
    return std::abs(num);
}

JSValue MathObject::floor(const std::vector<JSValue>& args) {
    if (args.empty()) return std::numeric_limits<double>::quiet_NaN();
    double num = toNumber(args[0]);
    return std::floor(num);
}

JSValue MathObject::ceil(const std::vector<JSValue>& args) {
    if (args.empty()) return std::numeric_limits<double>::quiet_NaN();
    double num = toNumber(args[0]);
    return std::ceil(num);
}

JSValue MathObject::round(const std::vector<JSValue>& args) {
    if (args.empty()) return std::numeric_limits<double>::quiet_NaN();
    double num = toNumber(args[0]);
    return std::round(num);
}

JSValue MathObject::max(const std::vector<JSValue>& args) {
    if (args.empty()) return -std::numeric_limits<double>::infinity();
    
    double maxVal = -std::numeric_limits<double>::infinity();
    for (const auto& arg : args) {
        double num = toNumber(arg);
        if (std::isnan(num)) return std::numeric_limits<double>::quiet_NaN();
        maxVal = std::max(maxVal, num);
    }
    return maxVal;
}

JSValue MathObject::min(const std::vector<JSValue>& args) {
    if (args.empty()) return std::numeric_limits<double>::infinity();
    
    double minVal = std::numeric_limits<double>::infinity();
    for (const auto& arg : args) {
        double num = toNumber(arg);
        if (std::isnan(num)) return std::numeric_limits<double>::quiet_NaN();
        minVal = std::min(minVal, num);
    }
    return minVal;
}

JSValue MathObject::pow(const std::vector<JSValue>& args) {
    if (args.size() < 2) return std::numeric_limits<double>::quiet_NaN();
    double base = toNumber(args[0]);
    double exponent = toNumber(args[1]);
    return std::pow(base, exponent);
}

JSValue MathObject::sqrt(const std::vector<JSValue>& args) {
    if (args.empty()) return std::numeric_limits<double>::quiet_NaN();
    double num = toNumber(args[0]);
    return std::sqrt(num);
}

JSValue MathObject::random(const std::vector<JSValue>& /* args */) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_real_distribution<> dis(0.0, 1.0);
    return dis(gen);
}

//<---------TYPE CHECKING UTILITIES--------->
bool isNumber(const JSValue& value) {
    return std::holds_alternative<double>(value);
}

bool isString(const JSValue& value) {
    return std::holds_alternative<std::string>(value);
}

bool isBoolean(const JSValue& value) {
    return std::holds_alternative<bool>(value);
}

bool isNull(const JSValue& value) {
    return std::holds_alternative<std::nullptr_t>(value);
}

bool isUndefined(const JSValue& value) {
    return std::holds_alternative<std::nullptr_t>(value);
}

//<---------TYPE CONVERSION UTILITIES--------->
double toNumber(const JSValue& value) {
    if (std::holds_alternative<double>(value)) {
        return std::get<double>(value);
    } else if (std::holds_alternative<bool>(value)) {
        return std::get<bool>(value) ? 1.0 : 0.0;
    } else if (std::holds_alternative<std::string>(value)) {
        const std::string& str = std::get<std::string>(value);
        if (str.empty()) return 0.0;
        try {
            return std::stod(str);
        } catch (...) {
            return std::numeric_limits<double>::quiet_NaN();
        }
    } else {
        return 0.0; // null/undefined -> 0
    }
}

std::string toString(const JSValue& value) {
    if (std::holds_alternative<std::string>(value)) {
        return std::get<std::string>(value);
    } else if (std::holds_alternative<double>(value)) {
        double num = std::get<double>(value);
        if (std::isnan(num)) return "NaN";
        if (std::isinf(num)) return num > 0 ? "Infinity" : "-Infinity";
        
        // Check if it's a whole number
        if (num == std::floor(num)) {
            return std::to_string(static_cast<long long>(num));
        } else {
            return std::to_string(num);
        }
    } else if (std::holds_alternative<bool>(value)) {
        return std::get<bool>(value) ? "true" : "false";
    } else if (std::holds_alternative<std::nullptr_t>(value)) {
        return "null";
    } else {
        return "[object Object]";
    }
}

bool toBoolean(const JSValue& value) {
    if (std::holds_alternative<bool>(value)) {
        return std::get<bool>(value);
    } else if (std::holds_alternative<double>(value)) {
        double num = std::get<double>(value);
        return num != 0.0 && !std::isnan(num);
    } else if (std::holds_alternative<std::string>(value)) {
        return !std::get<std::string>(value).empty();
    } else {
        return false; // null/undefined -> false
    }
}

//<---------OBJECT CREATION HELPERS--------->
std::shared_ptr<JSObject> createObject() {
    return std::make_shared<JSObject>();
}

std::shared_ptr<JSArray> createArray(const std::vector<JSValue>& elements) {
    return std::make_shared<JSArray>(elements);
}

std::shared_ptr<JSFunction> createFunction(const std::string& name, NativeFunction func) {
    return std::make_shared<JSFunction>(name, func);
}

} // namespace Quanta
