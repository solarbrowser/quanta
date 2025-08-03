#ifndef QUANTA_ERROR_H
#define QUANTA_ERROR_H

#include "Value.h"
#include "Object.h"
#include <string>
#include <memory>

namespace Quanta {

/**
 * JavaScript Error object implementation
 * Supports all standard error types: Error, TypeError, ReferenceError, etc.
 */
class Error : public Object {
public:
    enum class Type {
        Error,           // Generic error
        TypeError,       // Type-related errors
        ReferenceError,  // Reference errors (undefined variables)
        SyntaxError,     // Syntax errors
        RangeError,      // Range/bounds errors
        URIError,        // URI encoding/decoding errors
        EvalError,       // Eval-related errors (deprecated but included)
        AggregateError   // ES2021 aggregate errors
    };

private:
    Type error_type_;
    std::string message_;
    std::string name_;
    std::string stack_trace_;
    int line_number_;
    int column_number_;
    std::string filename_;

public:
    // Constructors
    Error(Type type = Type::Error, const std::string& message = "");
    Error(Type type, const std::string& message, const std::string& filename, int line, int column);
    
    // Copy constructor (deleted since Object is non-copyable)
    Error(const Error& other) = delete;
    Error& operator=(const Error& other) = delete;
    
    virtual ~Error() = default;

    // Error properties
    Type get_error_type() const { return error_type_; }
    const std::string& get_message() const { return message_; }
    const std::string& get_name() const { return name_; }
    const std::string& get_stack_trace() const { return stack_trace_; }
    int get_line_number() const { return line_number_; }
    int get_column_number() const { return column_number_; }
    const std::string& get_filename() const { return filename_; }
    
    // Set properties
    void set_message(const std::string& message) { message_ = message; }
    void set_stack_trace(const std::string& stack) { stack_trace_ = stack; }
    void set_location(const std::string& filename, int line, int column);
    
    // JavaScript Error interface
    std::string to_string() const;
    
    // Static factory methods
    static std::unique_ptr<Error> create_error(const std::string& message = "");
    static std::unique_ptr<Error> create_type_error(const std::string& message = "");
    static std::unique_ptr<Error> create_reference_error(const std::string& message = "");
    static std::unique_ptr<Error> create_syntax_error(const std::string& message = "");
    static std::unique_ptr<Error> create_range_error(const std::string& message = "");
    static std::unique_ptr<Error> create_uri_error(const std::string& message = "");
    static std::unique_ptr<Error> create_eval_error(const std::string& message = "");
    
    // Exception throwing helpers
    static void throw_error(const std::string& message = "");
    static void throw_type_error(const std::string& message = "");
    static void throw_reference_error(const std::string& message = "");
    static void throw_syntax_error(const std::string& message = "");
    static void throw_range_error(const std::string& message = "");
    
    // Stack trace generation
    void generate_stack_trace();
    
    // Static type name mapping
    static std::string type_to_name(Type type);
    
    // ES2025 Static methods
    static Value isError(Context& ctx, const std::vector<Value>& args);
    
private:
    void initialize_properties();
    void set_error_name();
};

/**
 * JavaScript exception class for throwing errors
 */
class JavaScriptException : public std::exception {
private:
    std::unique_ptr<Error> error_;
    std::string what_message_;

public:
    explicit JavaScriptException(std::unique_ptr<Error> error);
    
    // std::exception interface
    const char* what() const noexcept override;
    
    // Access to the JavaScript error object
    const Error* get_error() const { return error_.get(); }
    Error* get_error() { return error_.get(); }
    
    // Release ownership of the error
    std::unique_ptr<Error> release_error() { return std::move(error_); }
};

// Convenience macros for error throwing
#define JS_THROW_ERROR(msg) Error::throw_error(msg)
#define JS_THROW_TYPE_ERROR(msg) Error::throw_type_error(msg)
#define JS_THROW_REFERENCE_ERROR(msg) Error::throw_reference_error(msg)
#define JS_THROW_SYNTAX_ERROR(msg) Error::throw_syntax_error(msg)
#define JS_THROW_RANGE_ERROR(msg) Error::throw_range_error(msg)

} // namespace Quanta

#endif // QUANTA_ERROR_H