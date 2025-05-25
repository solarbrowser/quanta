//<---------QUANTA JS ENGINE - STANDARD LIBRARY IMPLEMENTATION--------->
// Stage 5: Final Optimizations & Library Support - Standard Library Implementation
// Purpose: Enhanced built-in objects, utility functions, and JavaScript compatibility

#include "../include/stdlib.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <random>
#include <cctype>

namespace Quanta {

//<---------ENHANCED JS ARRAY IMPLEMENTATION--------->
EnhancedJSArray::EnhancedJSArray() : JSArray() {
    setSize(sizeof(EnhancedJSArray));
}

EnhancedJSArray::EnhancedJSArray(size_t length) : JSArray() {
    // Initialize array with specified length
    for (size_t i = 0; i < length; ++i) {
        push(JSValue(nullptr)); // Fill with null values
    }
    setSize(sizeof(EnhancedJSArray) + length * sizeof(JSValue));
}

EnhancedJSArray::EnhancedJSArray(std::initializer_list<JSValue> values) : JSArray() {
    for (const auto& value : values) {
        push(value);
    }
    setSize(sizeof(EnhancedJSArray) + values.size() * sizeof(JSValue));
}

JSValue EnhancedJSArray::map(JSFunction* callback, JSObject* thisArg) {
    if (!callback) {
        return JSValue(nullptr); // Return null
    }
    
    auto result = std::make_shared<EnhancedJSArray>();
    
    for (size_t i = 0; i < length(); ++i) {
        JSValue element = get(i);
        std::vector<JSValue> args = {element, JSValue(static_cast<double>(i))};
        JSValue mappedValue = callback->call(args);
        result->push(mappedValue);
    }
    
    return JSValue(nullptr); // Would return the result array as object reference
}

JSValue EnhancedJSArray::filter(JSFunction* callback, JSObject* thisArg) {
    if (!callback) {
        return JSValue(nullptr); // Return null
    }
    
    auto result = std::make_shared<EnhancedJSArray>();
    
    for (size_t i = 0; i < length(); ++i) {
        JSValue element = get(i);
        std::vector<JSValue> args = {element, JSValue(static_cast<double>(i))};
        JSValue shouldInclude = callback->call(args);
        
        // Check if result is truthy (for boolean type)
        if (std::holds_alternative<bool>(shouldInclude) && std::get<bool>(shouldInclude)) {
            result->push(element);
        }
    }
    
    return JSValue(nullptr); // Would return the result array as object reference
}

JSValue EnhancedJSArray::includes(const JSValue& searchElement, int fromIndex) {
    size_t startIndex = (fromIndex < 0) ? 
        std::max(0, static_cast<int>(length()) + fromIndex) : 
        static_cast<size_t>(fromIndex);
    
    for (size_t i = startIndex; i < length(); ++i) {
        JSValue element = get(i);
        if (element == searchElement) {
            return JSValue(true);
        }
    }      return JSValue(false);
}

JSValue EnhancedJSArray::push(const JSValue& element) {
    JSArray::push(element);
    return JSValue(static_cast<double>(length()));
}

JSValue EnhancedJSArray::push(const std::vector<JSValue>& elements) {
    for (const auto& element : elements) {
        JSArray::push(element);
    }
    return JSValue(static_cast<double>(length()));
}

JSValue EnhancedJSArray::pop() {
    return JSArray::pop();
}

JSValue EnhancedJSArray::shift() {
    return JSArray::shift();
}

JSValue EnhancedJSArray::unshift(const JSValue& element) {
    JSArray::unshift(element);
    return JSValue(static_cast<double>(length()));
}

JSValue EnhancedJSArray::unshift(const std::vector<JSValue>& elements) {
    // Insert elements at the beginning
    for (size_t i = 0; i < elements.size(); ++i) {
        JSArray::unshift(elements[elements.size() - 1 - i]);
    }
    return JSValue(static_cast<double>(length()));
}

JSValue EnhancedJSArray::splice(int start, int deleteCount, const std::vector<JSValue>& items) {
    // This is a simplified implementation
    // In a full implementation, this would remove elements and insert new ones
    auto deletedArray = std::make_shared<EnhancedJSArray>();
    return JSValue(nullptr); // Would return array of deleted elements
}

JSValue EnhancedJSArray::sort(JSFunction* compareFn) {
    // Simple sort implementation
    // In a full implementation, this would use the compare function
    return JSValue(nullptr); // Would return sorted array
}

JSValue EnhancedJSArray::reverse() {
    // Reverse the array in place
    size_t len = length();
    for (size_t i = 0; i < len / 2; ++i) {
        JSValue temp = get(i);
        set(i, get(len - 1 - i));
        set(len - 1 - i, temp);
    }
    return JSValue(nullptr); // Would return reference to this array
}

JSValue EnhancedJSArray::slice(int start, int end) {
    auto result = std::make_shared<EnhancedJSArray>();
    size_t len = length();
    
    size_t startIdx = (start < 0) ? std::max(0, static_cast<int>(len) + start) : static_cast<size_t>(start);
    size_t endIdx = (end < 0) ? std::max(0, static_cast<int>(len) + end) : 
                    ((end == -1) ? len : static_cast<size_t>(end));
      for (size_t i = startIdx; i < std::min(endIdx, len); ++i) {
        result->push(get(i));
    }
    
    return JSValue(nullptr); // Would return new array
}

JSValue EnhancedJSArray::concat(const std::vector<JSValue>& arrays) {
    auto result = std::make_shared<EnhancedJSArray>();
      // Copy current array
    for (size_t i = 0; i < length(); ++i) {
        result->push(get(i));
    }
    
    // Add elements from other arrays
    for (const auto& arr : arrays) {
        // In a full implementation, we'd check if arr is an array and iterate through it
        result->push(arr);
    }
    
    return JSValue(nullptr); // Would return new concatenated array
}

JSValue EnhancedJSArray::indexOf(const JSValue& searchElement, int fromIndex) {
    size_t startIndex = (fromIndex < 0) ? 
        std::max(0, static_cast<int>(length()) + fromIndex) : 
        static_cast<size_t>(fromIndex);
    
    for (size_t i = startIndex; i < length(); ++i) {
        JSValue element = get(i);
        if (element == searchElement) {
            return JSValue(static_cast<double>(i));
        }
    }
    
    return JSValue(-1.0);
}

JSValue EnhancedJSArray::join(const std::string& separator) {
    if (length() == 0) {
        return JSValue(std::string(""));
    }
    
    std::stringstream ss;
    for (size_t i = 0; i < length(); ++i) {
        if (i > 0) {
            ss << separator;
        }
        
        JSValue element = get(i);
        if (std::holds_alternative<std::string>(element)) {
            ss << std::get<std::string>(element);
        } else if (std::holds_alternative<double>(element)) {
            ss << std::get<double>(element);
        } else if (std::holds_alternative<bool>(element)) {
            ss << (std::get<bool>(element) ? "true" : "false");
        }
        // null and undefined become empty string in join
    }
    
    return JSValue(ss.str());
}

std::vector<GCObject*> EnhancedJSArray::getReferences() const {
    std::vector<GCObject*> refs;
    // Add references to GC objects stored in the array
    // This would need to be implemented based on how JSValue stores object references
    return refs;
}

JSValue EnhancedJSArray::getProperty(const std::string& name) {
    if (name == "map") {
        return JSValue(std::string("function"));
    }
    if (name == "filter") {
        return JSValue(std::string("function"));
    }
    if (name == "includes") {
        return JSValue(std::string("function"));
    }
    if (name == "indexOf") {
        return JSValue(std::string("function"));
    }
    if (name == "join") {
        return JSValue(std::string("function"));
    }
    if (name == "push") {
        return JSValue(std::string("function"));
    }
    if (name == "pop") {
        return JSValue(std::string("function"));
    }
    
    return JSArray::getProperty(name);
}

void EnhancedJSArray::setProperty(const std::string& name, const JSValue& value) {
    if (name == "length") {
        if (std::holds_alternative<double>(value)) {
            size_t newLength = static_cast<size_t>(std::get<double>(value));
            // Resize array to new length
            while (length() < newLength) {
                push(JSValue(nullptr));
            }
            // Note: In a full implementation, we'd also handle shrinking
            return;
        }
    }
    
    JSArray::setProperty(name, value);
}

JSValue EnhancedJSArray::callCallback(JSFunction* callback, JSObject* thisArg, const std::vector<JSValue>& args) {
    // This would call the JavaScript function with the given arguments
    // For now, return a dummy value
    return JSValue(true);
}

//<---------ENHANCED JS STRING IMPLEMENTATION--------->
EnhancedJSString::EnhancedJSString() : value_("") {
    setSize(sizeof(EnhancedJSString));
}

EnhancedJSString::EnhancedJSString(const std::string& value) : value_(value) {
    setSize(sizeof(EnhancedJSString) + value.length());
}

JSValue EnhancedJSString::charAt(size_t index) {
    if (index >= value_.length()) {
        return JSValue(std::string(""));
    }
    return JSValue(std::string(1, value_[index]));
}

JSValue EnhancedJSString::charCodeAt(size_t index) {
    if (index >= value_.length()) {
        return JSValue(std::numeric_limits<double>::quiet_NaN());
    }
    return JSValue(static_cast<double>(static_cast<unsigned char>(value_[index])));
}

JSValue EnhancedJSString::includes(const std::string& searchString, int position) {
    size_t pos = (position < 0) ? 0 : static_cast<size_t>(position);
    return JSValue(value_.find(searchString, pos) != std::string::npos);
}

JSValue EnhancedJSString::indexOf(const std::string& searchString, int fromIndex) {
    size_t pos = (fromIndex < 0) ? 0 : static_cast<size_t>(fromIndex);
    size_t found = value_.find(searchString, pos);
    return JSValue(found == std::string::npos ? -1.0 : static_cast<double>(found));
}

JSValue EnhancedJSString::slice(int start, int end) {
    int len = static_cast<int>(value_.length());
    
    int actualStart = (start < 0) ? std::max(0, len + start) : std::min(start, len);
    int actualEnd = (end == -1) ? len : 
                   (end < 0) ? std::max(0, len + end) : std::min(end, len);
    
    if (actualStart >= actualEnd) {
        return JSValue(std::string(""));
    }
    
    return JSValue(value_.substr(actualStart, actualEnd - actualStart));
}

JSValue EnhancedJSString::toLowerCase() {
    std::string result = value_;
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return JSValue(result);
}

JSValue EnhancedJSString::toUpperCase() {
    std::string result = value_;
    std::transform(result.begin(), result.end(), result.begin(), ::toupper);
    return JSValue(result);
}

JSValue EnhancedJSString::trim() {
    std::string result = value_;
    
    // Trim leading whitespace
    result.erase(result.begin(), std::find_if(result.begin(), result.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    
    // Trim trailing whitespace
    result.erase(std::find_if(result.rbegin(), result.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), result.end());
    
    return JSValue(result);
}

std::vector<GCObject*> EnhancedJSString::getReferences() const {
    return {}; // Strings don't reference other GC objects
}

JSValue EnhancedJSString::getProperty(const std::string& name) {
    if (name == "length") {
        return JSValue(static_cast<double>(value_.length()));
    }
    if (name == "charAt") {
        return JSValue(std::string("function"));
    }
    if (name == "charCodeAt") {
        return JSValue(std::string("function"));
    }
    if (name == "includes") {
        return JSValue(std::string("function"));
    }
    if (name == "indexOf") {
        return JSValue(std::string("function"));
    }
    if (name == "slice") {
        return JSValue(std::string("function"));
    }
    if (name == "toLowerCase") {
        return JSValue(std::string("function"));
    }
    if (name == "toUpperCase") {
        return JSValue(std::string("function"));
    }
    if (name == "trim") {
        return JSValue(std::string("function"));
    }
    
    return JSValue(); // undefined
}

void EnhancedJSString::setProperty(const std::string& name, const JSValue& value) {
    // Strings are immutable, so most property sets are ignored
    JSObject::setProperty(name, value);
}

//<---------ENHANCED MATH IMPLEMENTATION--------->
EnhancedMath::EnhancedMath() {
    initializeConstants();
    initializeMethods();
}

void EnhancedMath::initializeConstants() {
    constants_["PI"] = JSValue(M_PI);
    constants_["E"] = JSValue(M_E);
    constants_["LN2"] = JSValue(M_LN2);
    constants_["LN10"] = JSValue(M_LN10);
    constants_["LOG2E"] = JSValue(M_LOG2E);
    constants_["LOG10E"] = JSValue(M_LOG10E);
    constants_["SQRT1_2"] = JSValue(M_SQRT1_2);
    constants_["SQRT2"] = JSValue(M_SQRT2);
}

void EnhancedMath::initializeMethods() {
    // Methods are handled in getProperty
}

JSValue EnhancedMath::getProperty(const std::string& name) {
    // Constants
    auto constIt = constants_.find(name);
    if (constIt != constants_.end()) {
        return constIt->second;
    }
    
    // Methods
    if (name == "abs" || name == "ceil" || name == "floor" || name == "round" ||
        name == "max" || name == "min" || name == "pow" || name == "sqrt" ||
        name == "random" || name == "sin" || name == "cos" || name == "tan" ||
        name == "log" || name == "exp") {
        return JSValue(std::string("function"));
    }
    
    return JSValue(); // undefined
}

void EnhancedMath::setProperty(const std::string& name, const JSValue& value) {
    // Math object properties are read-only
    // Ignore attempts to set them
}

JSValue EnhancedMath::abs(const std::vector<JSValue>& args) {
    if (args.empty() || !std::holds_alternative<double>(args[0])) {
        return JSValue(std::numeric_limits<double>::quiet_NaN());
    }
    
    double x = std::get<double>(args[0]);
    return JSValue(std::abs(x));
}

JSValue EnhancedMath::random(const std::vector<JSValue>& args) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_real_distribution<> dis(0.0, 1.0);
    
    return JSValue(dis(gen));
}

//<---------STANDARD LIBRARY GLOBAL IMPLEMENTATION--------->
StandardLibraryGlobal::StandardLibraryGlobal() {
    math_ = std::make_shared<EnhancedMath>();
    json_ = std::make_shared<JSJSON>();
}

JSValue StandardLibraryGlobal::getProperty(const std::string& name) {
    if (name == "Math") {
        return JSValue(std::string("object"));
    }
    if (name == "JSON") {
        return JSValue(std::string("object"));
    }
    if (name == "Array") {
        return JSValue(std::string("function"));
    }
    if (name == "String") {
        return JSValue(std::string("function"));
    }
    if (name == "Date") {
        return JSValue(std::string("function"));
    }
    if (name == "RegExp") {
        return JSValue(std::string("function"));
    }
    if (name == "Promise") {
        return JSValue(std::string("function"));
    }
    
    // Global functions
    if (name == "parseInt" || name == "parseFloat" || name == "isNaN" || 
        name == "isFinite" || name == "encodeURIComponent" || name == "decodeURIComponent" ||
        name == "setTimeout" || name == "clearTimeout" || name == "setInterval" || name == "clearInterval") {
        return JSValue(std::string("function"));
    }
    
    return JSObject::getProperty(name);
}

void StandardLibraryGlobal::setProperty(const std::string& name, const JSValue& value) {
    JSObject::setProperty(name, value);
}

void StandardLibraryGlobal::initialize() {
    registerGlobalFunctions();
    registerConstructors();
}

void StandardLibraryGlobal::registerGlobalFunctions() {
    // Global functions would be registered here
    // For now, they're handled in getProperty
}

void StandardLibraryGlobal::registerConstructors() {
    // Constructor functions would be registered here
    // For now, they're handled in getProperty
}

JSValue StandardLibraryGlobal::parseInt(const std::vector<JSValue>& args) {
    if (args.empty()) {
        return JSValue(std::numeric_limits<double>::quiet_NaN());
    }
      std::string str;
    if (std::holds_alternative<std::string>(args[0])) {
        str = std::get<std::string>(args[0]);
    } else if (std::holds_alternative<double>(args[0])) {
        str = std::to_string(std::get<double>(args[0]));
    } else {
        return JSValue(std::numeric_limits<double>::quiet_NaN());
    }
    
    // Simple parseInt implementation
    try {
        size_t pos;
        double result = std::stod(str, &pos);
        return JSValue(std::floor(result));
    } catch (...) {
        return JSValue(std::numeric_limits<double>::quiet_NaN());
    }
}

//<---------SIMPLE JSON IMPLEMENTATION--------->
JSJSON::JSJSON() {}

JSValue JSJSON::getProperty(const std::string& name) {
    if (name == "stringify") {
        return JSValue(std::string("function"));
    }
    if (name == "parse") {
        return JSValue(std::string("function"));
    }
    
    return JSValue(); // undefined
}

void JSJSON::setProperty(const std::string& name, const JSValue& value) {
    // JSON object properties are read-only
}

JSValue JSJSON::stringify(const std::vector<JSValue>& args) {
    if (args.empty()) {
        return JSValue(); // undefined
    }
    
    return JSValue(stringifyValue(args[0]));
}

std::string JSJSON::stringifyValue(const JSValue& value, int indent) {
    if (std::holds_alternative<std::string>(value)) {
        return "\"" + std::get<std::string>(value) + "\"";
    } else if (std::holds_alternative<double>(value)) {
        return std::to_string(std::get<double>(value));
    } else if (std::holds_alternative<bool>(value)) {
        return std::get<bool>(value) ? "true" : "false";
    } else if (std::holds_alternative<std::nullptr_t>(value)) {
        return "null";
    } else {
        return "null";
    }
}

//<---------UTILITY FUNCTIONS--------->
std::shared_ptr<StandardLibraryGlobal> createStandardLibrary() {
    auto stdlib = std::make_shared<StandardLibraryGlobal>();
    stdlib->initialize();
    return stdlib;
}

void registerStandardLibrary(JSObject* global) {
    if (!global) return;
    
    // Register enhanced objects
    global->setProperty("Array", JSValue(std::string("function")));
    global->setProperty("String", JSValue(std::string("function")));
    global->setProperty("Math", JSValue(std::string("object")));
    global->setProperty("JSON", JSValue(std::string("object")));
    global->setProperty("Date", JSValue(std::string("function")));
    global->setProperty("RegExp", JSValue(std::string("function")));
    global->setProperty("Promise", JSValue(std::string("function")));
}

JSValue createEnhancedArray(const std::vector<JSValue>& elements) {
    auto array = std::make_shared<EnhancedJSArray>();
    for (const auto& element : elements) {
        array->push(element);
    }
    return JSValue(std::string("object")); // Would return the array object
}

JSValue createEnhancedString(const std::string& value) {
    auto str = std::make_shared<EnhancedJSString>(value);
    return JSValue(std::string("object")); // Would return the string object
}

} // namespace Quanta