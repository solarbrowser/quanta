//<---------QUANTA JS ENGINE - RUNTIME OBJECTS HEADER--------->
// Stage 2: Interpreter Engine - Runtime Object System
// Purpose: JavaScript runtime objects and built-in types
// Max Lines: 5000 (Current: ~200)

#ifndef QUANTA_RUNTIME_OBJECTS_H
#define QUANTA_RUNTIME_OBJECTS_H

#include "env.h"
#include <string>
#include <vector>
#include "hash_workaround.h"
#include <functional>
#include <memory>

namespace Quanta {

//<---------EXTENDED VALUE TYPES--------->
// Forward declarations
class JSObject;
class JSArray;
class JSFunction;

// Extended JSValue to include objects
using JSValueExtended = std::variant<
    double,              // Number
    std::string,         // String
    bool,                // Boolean
    std::nullptr_t,      // Null/Undefined
    std::shared_ptr<JSObject>,  // Object
    std::shared_ptr<JSArray>,   // Array
    std::shared_ptr<JSFunction> // Function
>;

//<---------BASE JAVASCRIPT OBJECT--------->
class JSObject {
protected:
    SimpleMap<std::string, JSValue> properties;
    
public:
    virtual ~JSObject() = default;
    
    // Property operations
    virtual void setProperty(const std::string& name, const JSValue& value);
    virtual JSValue getProperty(const std::string& name);
    virtual bool hasProperty(const std::string& name);
    virtual void deleteProperty(const std::string& name);
    
    // Object introspection
    virtual std::vector<std::string> getPropertyNames();
    virtual std::string toString();
    virtual std::string getType() { return "object"; }
};

//<---------JAVASCRIPT ARRAY--------->
class JSArray : public JSObject {
private:
    std::vector<JSValue> elements;
    
public:
    JSArray() = default;
    JSArray(const std::vector<JSValue>& elements);
    
    // Array-specific operations
    void push(const JSValue& value);
    JSValue pop();
    JSValue shift();
    void unshift(const JSValue& value);
    size_t length() const { return elements.size(); }
    
    // Index operations
    JSValue get(size_t index);
    void set(size_t index, const JSValue& value);
    
    // Overrides
    std::string getType() override { return "array"; }
    std::string toString() override;
    
    // Iterator support
    std::vector<JSValue>::iterator begin() { return elements.begin(); }
    std::vector<JSValue>::iterator end() { return elements.end(); }
};

//<---------NATIVE FUNCTION TYPE--------->
using NativeFunction = std::function<JSValue(const std::vector<JSValue>&)>;

//<---------JAVASCRIPT FUNCTION--------->
class JSFunction : public JSObject {
private:
    std::string name;
    NativeFunction nativeFunction;
    
public:
    JSFunction(const std::string& name, NativeFunction func);
    
    // Function call
    JSValue call(const std::vector<JSValue>& args);
    
    // Overrides
    std::string getType() override { return "function"; }
    std::string toString() override;
    std::string getName() const { return name; }
};

//<---------CONSOLE OBJECT--------->
class ConsoleObject : public JSObject {
public:
    ConsoleObject();
    
    // Console methods
    JSValue log(const std::vector<JSValue>& args);
    JSValue error(const std::vector<JSValue>& args);
    JSValue warn(const std::vector<JSValue>& args);
    JSValue info(const std::vector<JSValue>& args);
    
    std::string getType() override { return "console"; }
};

//<---------MATH OBJECT--------->
class MathObject : public JSObject {
public:
    MathObject();
    
    // Math constants
    static constexpr double PI = 3.141592653589793;
    static constexpr double E = 2.718281828459045;
    
    // Math methods
    JSValue abs(const std::vector<JSValue>& args);
    JSValue floor(const std::vector<JSValue>& args);
    JSValue ceil(const std::vector<JSValue>& args);
    JSValue round(const std::vector<JSValue>& args);
    JSValue max(const std::vector<JSValue>& args);
    JSValue min(const std::vector<JSValue>& args);
    JSValue pow(const std::vector<JSValue>& args);
    JSValue sqrt(const std::vector<JSValue>& args);
    JSValue random(const std::vector<JSValue>& args);
    
    std::string getType() override { return "math"; }
};

//<---------TYPE CHECKING UTILITIES--------->
bool isNumber(const JSValue& value);
bool isString(const JSValue& value);
bool isBoolean(const JSValue& value);
bool isNull(const JSValue& value);
bool isUndefined(const JSValue& value);

// Type conversion utilities
double toNumber(const JSValue& value);
std::string toString(const JSValue& value);
bool toBoolean(const JSValue& value);

// Object creation helpers
std::shared_ptr<JSObject> createObject();
std::shared_ptr<JSArray> createArray(const std::vector<JSValue>& elements = {});
std::shared_ptr<JSFunction> createFunction(const std::string& name, NativeFunction func);

} // namespace Quanta

#endif // QUANTA_RUNTIME_OBJECTS_H
