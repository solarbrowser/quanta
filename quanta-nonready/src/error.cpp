//<---------QUANTA JS ENGINE - ERROR HANDLER IMPLEMENTATION--------->
// Stage 1: Core Engine & Runtime - Error Handler
// Purpose: Manage compilation and runtime errors with proper reporting
// Max Lines: 5000 (Current: ~150)

#include "../include/error.h"
#include <iostream>
#include <sstream>

namespace Quanta {

//<---------ERROR HANDLER CONSTRUCTOR--------->
ErrorHandler::ErrorHandler() : hasErrors(false) {}

//<---------ERROR REPORTING--------->
void ErrorHandler::reportError(ErrorType type, const std::string& message, size_t line, size_t column, const std::string& source) {
    errors.emplace_back(type, message, line, column, source);
    hasErrors = true;
}

void ErrorHandler::reportSyntaxError(const std::string& message, size_t line, size_t column) {
    reportError(ErrorType::SYNTAX_ERROR, message, line, column);
}

void ErrorHandler::reportReferenceError(const std::string& message, size_t line, size_t column) {
    reportError(ErrorType::REFERENCE_ERROR, message, line, column);
}

void ErrorHandler::reportTypeError(const std::string& message, size_t line, size_t column) {
    reportError(ErrorType::TYPE_ERROR, message, line, column);
}

void ErrorHandler::reportRuntimeError(const std::string& message, size_t line, size_t column) {
    reportError(ErrorType::RUNTIME_ERROR, message, line, column);
}

//<---------ERROR STATE--------->
bool ErrorHandler::hasError() const {
    return hasErrors;
}

void ErrorHandler::clearErrors() {
    errors.clear();
    hasErrors = false;
}

const std::vector<Error>& ErrorHandler::getErrors() const {
    return errors;
}

//<---------ERROR FORMATTING--------->
std::string ErrorHandler::formatError(const Error& error) const {
    std::ostringstream oss;
    
    // Error type
    switch (error.type) {
        case ErrorType::SYNTAX_ERROR:
            oss << "SyntaxError";
            break;
        case ErrorType::REFERENCE_ERROR:
            oss << "ReferenceError";
            break;
        case ErrorType::TYPE_ERROR:
            oss << "TypeError";
            break;
        case ErrorType::RANGE_ERROR:
            oss << "RangeError";
            break;
        case ErrorType::LEXICAL_ERROR:
            oss << "LexicalError";
            break;
        case ErrorType::RUNTIME_ERROR:
            oss << "RuntimeError";
            break;
    }
    
    oss << ": " << error.message;
    
    if (error.line > 0) {
        oss << " at line " << error.line;
        if (error.column > 0) {
            oss << ", column " << error.column;
        }
    }
    
    if (!error.source.empty()) {
        oss << "\n  in: " << error.source;
    }
    
    return oss.str();
}

void ErrorHandler::printErrors() const {
    for (const auto& error : errors) {
        std::cerr << formatError(error) << std::endl;
    }
}

} // namespace Quanta
