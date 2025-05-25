//<---------QUANTA JS ENGINE - STANDARD LIBRARY--------->
// Stage 5: Final Optimizations & Library Support - Standard Library Implementation
// Purpose: Enhanced built-in objects, utility functions, and JavaScript compatibility
// Max Lines: 2000 (Current: ~150)

#ifndef QUANTA_STDLIB_H
#define QUANTA_STDLIB_H

#include "runtime_objects.h"
#include "gc.h"
#include "hash_workaround.h"
#include <string>
#include <vector>
#include <functional>
#include <regex>
#include <chrono>
#include <cmath>

// Define math constants if not available
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_E
#define M_E 2.71828182845904523536
#endif
#ifndef M_LN2
#define M_LN2 0.693147180559945309417
#endif
#ifndef M_LN10
#define M_LN10 2.30258509299404568402
#endif
#ifndef M_LOG2E
#define M_LOG2E 1.44269504088896340736
#endif
#ifndef M_LOG10E
#define M_LOG10E 0.434294481903251827651
#endif
#ifndef M_SQRT1_2
#define M_SQRT1_2 0.707106781186547524401
#endif
#ifndef M_SQRT2
#define M_SQRT2 1.41421356237309504880
#endif

namespace Quanta {

//<---------ENHANCED ARRAY OBJECT--------->
class EnhancedJSArray : public JSArray, public GCObject {
public:
    EnhancedJSArray();
    explicit EnhancedJSArray(size_t length);
    EnhancedJSArray(std::initializer_list<JSValue> values);
    
    // Enhanced array methods
    JSValue map(JSFunction* callback, JSObject* thisArg = nullptr);
    JSValue filter(JSFunction* callback, JSObject* thisArg = nullptr);
    JSValue reduce(JSFunction* callback, const JSValue& initialValue = JSValue());
    JSValue find(JSFunction* callback, JSObject* thisArg = nullptr);
    JSValue forEach(JSFunction* callback, JSObject* thisArg = nullptr);
    
    // Search methods
    JSValue includes(const JSValue& searchElement, int fromIndex = 0);
    JSValue indexOf(const JSValue& searchElement, int fromIndex = 0);
    JSValue lastIndexOf(const JSValue& searchElement, int fromIndex = -1);
      // Mutation methods
    JSValue push(const JSValue& element);
    JSValue push(const std::vector<JSValue>& elements);
    JSValue pop();
    JSValue shift();
    JSValue unshift(const JSValue& element);
    JSValue unshift(const std::vector<JSValue>& elements);
    JSValue splice(int start, int deleteCount, const std::vector<JSValue>& items = {});
    JSValue sort(JSFunction* compareFn = nullptr);
    JSValue reverse();
    
    // Utility methods
    JSValue join(const std::string& separator = ",");
    JSValue slice(int start = 0, int end = -1);
    JSValue concat(const std::vector<JSValue>& arrays);
    
    // GC integration
    std::vector<GCObject*> getReferences() const override;
    std::string getGCType() const override { return "Array"; }
    
    // JS Object interface
    JSValue getProperty(const std::string& name) override;
    void setProperty(const std::string& name, const JSValue& value) override;

private:
    void ensureCapacity(size_t newSize);
    JSValue callCallback(JSFunction* callback, JSObject* thisArg, const std::vector<JSValue>& args);
};

//<---------ENHANCED STRING OBJECT--------->
class EnhancedJSString : public JSObject, public GCObject {
public:
    EnhancedJSString();
    explicit EnhancedJSString(const std::string& value);
    
    // String methods
    JSValue charAt(size_t index);
    JSValue charCodeAt(size_t index);
    JSValue concat(const std::vector<std::string>& strings);
    JSValue includes(const std::string& searchString, int position = 0);
    JSValue indexOf(const std::string& searchString, int fromIndex = 0);
    JSValue lastIndexOf(const std::string& searchString, int fromIndex = -1);
    JSValue slice(int start, int end = -1);
    JSValue substring(int start, int end = -1);
    JSValue substr(int start, int length = -1);
    JSValue toLowerCase();
    JSValue toUpperCase();
    JSValue trim();
    JSValue trimStart();
    JSValue trimEnd();
    
    // Pattern matching
    JSValue match(const std::string& pattern);
    JSValue replace(const std::string& searchValue, const std::string& replaceValue);
    JSValue search(const std::string& pattern);
    JSValue split(const std::string& separator = "", int limit = -1);
    
    // Utility methods
    JSValue repeat(int count);
    JSValue padStart(int targetLength, const std::string& padString = " ");
    JSValue padEnd(int targetLength, const std::string& padString = " ");
    JSValue startsWith(const std::string& searchString, int position = 0);
    JSValue endsWith(const std::string& searchString, int length = -1);
    
    // GC integration
    std::vector<GCObject*> getReferences() const override;
    std::string getGCType() const override { return "String"; }
    
    // JS Object interface
    JSValue getProperty(const std::string& name) override;
    void setProperty(const std::string& name, const JSValue& value) override;

private:
    std::string value_;
};

//<---------ENHANCED MATH OBJECT--------->
class EnhancedMath : public JSObject {
public:
    EnhancedMath();
    
    JSValue getProperty(const std::string& name) override;
    void setProperty(const std::string& name, const JSValue& value) override;

private:
    SimpleMap<std::string, JSValue> constants_;
    void initializeConstants();
    void initializeMethods();
    
    // Math functions
    static JSValue abs(const std::vector<JSValue>& args);
    static JSValue ceil(const std::vector<JSValue>& args);
    static JSValue floor(const std::vector<JSValue>& args);
    static JSValue round(const std::vector<JSValue>& args);
    static JSValue max(const std::vector<JSValue>& args);
    static JSValue min(const std::vector<JSValue>& args);
    static JSValue pow(const std::vector<JSValue>& args);
    static JSValue sqrt(const std::vector<JSValue>& args);
    static JSValue random(const std::vector<JSValue>& args);
    static JSValue sin(const std::vector<JSValue>& args);
    static JSValue cos(const std::vector<JSValue>& args);
    static JSValue tan(const std::vector<JSValue>& args);
    static JSValue log(const std::vector<JSValue>& args);
    static JSValue exp(const std::vector<JSValue>& args);
};

//<---------DATE OBJECT--------->
class JSDate : public JSObject, public GCObject {
public:
    JSDate();
    JSDate(long long timestamp);
    JSDate(int year, int month, int day = 1, int hour = 0, int minute = 0, int second = 0, int millisecond = 0);
    
    // Date methods
    JSValue getTime();
    JSValue getFullYear();
    JSValue getMonth();
    JSValue getDate();
    JSValue getHours();
    JSValue getMinutes();
    JSValue getSeconds();
    JSValue getMilliseconds();
    JSValue getDay();
    
    // Setters
    void setTime(long long timestamp);
    void setFullYear(int year);
    void setMonth(int month);
    void setDate(int day);
    void setHours(int hour);
    void setMinutes(int minute);
    void setSeconds(int second);
    void setMilliseconds(int millisecond);
      // String conversion - override JSObject::toString()
    std::string toString() override;
    JSValue toDateString();
    JSValue toTimeString();
    JSValue toISOString();
    
    // Static methods
    static JSValue now();
    static JSValue parse(const std::string& dateString);
    
    // GC integration
    std::vector<GCObject*> getReferences() const override;
    std::string getGCType() const override { return "Date"; }
    
    // JS Object interface
    JSValue getProperty(const std::string& name) override;
    void setProperty(const std::string& name, const JSValue& value) override;

private:
    std::chrono::system_clock::time_point timestamp_;
    void updateFromTimePoint();
    std::chrono::system_clock::time_point parseDate(const std::string& dateString);
};

//<---------JSON OBJECT--------->
class JSJSON : public JSObject {
public:
    JSJSON();
      JSValue getProperty(const std::string& name) override;
    void setProperty(const std::string& name, const JSValue& value) override;

    static JSValue stringify(const std::vector<JSValue>& args);
    static JSValue parse(const std::vector<JSValue>& args);

private:
    
    static std::string stringifyValue(const JSValue& value, int indent = 0);
    static JSValue parseValue(const std::string& json, size_t& pos);
    static JSValue parseObject(const std::string& json, size_t& pos);
    static JSValue parseArray(const std::string& json, size_t& pos);
    static JSValue parseString(const std::string& json, size_t& pos);
    static JSValue parseNumber(const std::string& json, size_t& pos);
    static void skipWhitespace(const std::string& json, size_t& pos);
};

//<---------REGEXP OBJECT--------->
class JSRegExp : public JSObject, public GCObject {
public:
    JSRegExp(const std::string& pattern, const std::string& flags = "");
    
    // RegExp methods
    JSValue test(const std::string& string);
    JSValue exec(const std::string& string);
    
    // Properties
    std::string getSource() const { return pattern_; }
    std::string getFlags() const { return flags_; }
    bool getGlobal() const { return global_; }
    bool getIgnoreCase() const { return ignoreCase_; }
    bool getMultiline() const { return multiline_; }
    
    // GC integration
    std::vector<GCObject*> getReferences() const override;
    std::string getGCType() const override { return "RegExp"; }
    
    // JS Object interface
    JSValue getProperty(const std::string& name) override;
    void setProperty(const std::string& name, const JSValue& value) override;

private:
    std::string pattern_;
    std::string flags_;
    std::regex regex_;
    bool global_ = false;
    bool ignoreCase_ = false;
    bool multiline_ = false;
    int lastIndex_ = 0;
    
    void parseFlags(const std::string& flags);
    std::regex::flag_type getRegexFlags() const;
};

//<---------PROMISE OBJECT--------->
class JSPromise : public JSObject, public GCObject {
public:
    enum class State {
        PENDING,
        FULFILLED,
        REJECTED
    };
    
    JSPromise();
    JSPromise(std::function<void(std::function<void(JSValue)>, std::function<void(JSValue)>)> executor);
    
    // Promise methods
    JSValue then(JSFunction* onFulfilled, JSFunction* onRejected = nullptr);
    JSValue catch_(JSFunction* onRejected);
    JSValue finally(JSFunction* onFinally);
      // Static methods
    static JSValue resolve(const JSValue& value);
    static JSValue reject(const JSValue& reason);
    static JSValue all(const std::vector<JSValue>& promises);
    static JSValue race(const std::vector<JSValue>& promises);
    
    // Internal methods (instance)
    void fulfill(const JSValue& value);
    void fail(const JSValue& reason);
    State getState() const { return state_; }
    JSValue getValue() const { return value_; }
    
    // GC integration
    std::vector<GCObject*> getReferences() const override;
    std::string getGCType() const override { return "Promise"; }
    
    // JS Object interface
    JSValue getProperty(const std::string& name) override;
    void setProperty(const std::string& name, const JSValue& value) override;

private:
    State state_ = State::PENDING;
    JSValue value_;
    std::vector<std::pair<JSFunction*, JSFunction*>> callbacks_;
    
    void executeCallbacks();
};

//<---------STANDARD LIBRARY GLOBAL--------->
class StandardLibraryGlobal : public JSObject {
public:
    StandardLibraryGlobal();
    
    JSValue getProperty(const std::string& name) override;
    void setProperty(const std::string& name, const JSValue& value) override;
    
    void initialize();

private:
    std::shared_ptr<EnhancedMath> math_;
    std::shared_ptr<JSJSON> json_;
    
    void registerGlobalFunctions();
    void registerConstructors();
    
    // Global functions
    static JSValue parseInt(const std::vector<JSValue>& args);
    static JSValue parseFloat(const std::vector<JSValue>& args);
    static JSValue isNaN(const std::vector<JSValue>& args);
    static JSValue isFinite(const std::vector<JSValue>& args);
    static JSValue encodeURIComponent(const std::vector<JSValue>& args);
    static JSValue decodeURIComponent(const std::vector<JSValue>& args);
    static JSValue setTimeout(const std::vector<JSValue>& args);
    static JSValue clearTimeout(const std::vector<JSValue>& args);
    static JSValue setInterval(const std::vector<JSValue>& args);
    static JSValue clearInterval(const std::vector<JSValue>& args);
};

//<---------UTILITY FUNCTIONS--------->
std::shared_ptr<StandardLibraryGlobal> createStandardLibrary();
void registerStandardLibrary(JSObject* global);
JSValue createEnhancedArray(const std::vector<JSValue>& elements = {});
JSValue createEnhancedString(const std::string& value = "");
JSValue createDate(const std::vector<JSValue>& args = {});
JSValue createRegExp(const std::string& pattern, const std::string& flags = "");
JSValue createPromise(std::function<void(std::function<void(JSValue)>, std::function<void(JSValue)>)> executor);

} // namespace Quanta

#endif // QUANTA_STDLIB_H