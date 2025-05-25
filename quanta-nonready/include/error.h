//<---------QUANTA JS ENGINE - ERROR HANDLER HEADER--------->
// Stage 1: Core Engine & Runtime - Error Handler
// Purpose: Manage compilation and runtime errors with proper reporting
// Max Lines: 5000 (Current: ~80)

#ifndef QUANTA_ERROR_H
#define QUANTA_ERROR_H

#include <string>
#include <vector>
#include <exception>

namespace Quanta {

//<---------ERROR TYPES--------->
enum class ErrorType {
    SYNTAX_ERROR,
    REFERENCE_ERROR,
    TYPE_ERROR,
    RANGE_ERROR,
    LEXICAL_ERROR,
    RUNTIME_ERROR
};

//<---------ERROR STRUCTURE--------->
struct Error {
    ErrorType type;
    std::string message;
    size_t line;
    size_t column;
    std::string source;
    
    Error(ErrorType t, const std::string& msg, size_t l, size_t c, const std::string& src = "")
        : type(t), message(msg), line(l), column(c), source(src) {}
};

//<---------ERROR HANDLER CLASS--------->
class ErrorHandler {
private:
    std::vector<Error> errors;
    bool hasErrors;
    
public:
    ErrorHandler();
      void reportError(ErrorType type, const std::string& message, size_t line, size_t column, const std::string& source = "");
    void reportSyntaxError(const std::string& message, size_t line, size_t column);
    void reportReferenceError(const std::string& message, size_t line, size_t column);
    void reportTypeError(const std::string& message, size_t line, size_t column);
    void reportRuntimeError(const std::string& message, size_t line, size_t column);
    
    bool hasError() const;
    void clearErrors();
    const std::vector<Error>& getErrors() const;
    
    std::string formatError(const Error& error) const;
    void printErrors() const;
};

//<---------EXCEPTION CLASSES--------->
class QuantaException : public std::exception {
private:
    std::string message;
    
public:
    explicit QuantaException(const std::string& msg) : message(msg) {}
    const char* what() const noexcept override { return message.c_str(); }
};

class SyntaxException : public QuantaException {
public:
    explicit SyntaxException(const std::string& msg) : QuantaException("Syntax Error: " + msg) {}
};

class ReferenceException : public QuantaException {
public:
    explicit ReferenceException(const std::string& msg) : QuantaException("Reference Error: " + msg) {}
};

} // namespace Quanta

#endif // QUANTA_ERROR_H
