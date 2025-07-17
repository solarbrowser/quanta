#include "../include/RegExp.h"
#include "../include/Object.h"
#include <iostream>
#include <sstream>

namespace Quanta {

RegExp::RegExp(const std::string& pattern, const std::string& flags)
    : pattern_(pattern), flags_(flags), global_(false), ignore_case_(false), 
      multiline_(false), unicode_(false), sticky_(false), last_index_(0) {
    
    parse_flags(flags);
    
    try {
        regex_ = std::regex(pattern_, get_regex_flags());
    } catch (const std::regex_error& e) {
        // For now, create a simple regex that matches nothing
        regex_ = std::regex("(?!)");
    }
}

bool RegExp::test(const std::string& str) {
    try {
        return std::regex_search(str, regex_);
    } catch (const std::regex_error& e) {
        return false;
    }
}

Value RegExp::exec(const std::string& str) {
    try {
        std::smatch match;
        if (std::regex_search(str, match, regex_)) {
            // Create array-like object with match results
            auto result = new Object();
            result->set_property("0", Value(match[0].str()));
            result->set_property("index", Value(static_cast<double>(match.position())));
            result->set_property("input", Value(str));
            result->set_property("length", Value(static_cast<double>(match.size())));
            
            // Add captured groups
            for (size_t i = 1; i < match.size(); ++i) {
                result->set_property(std::to_string(i), Value(match[i].str()));
            }
            
            return Value(result);
        }
    } catch (const std::regex_error& e) {
        // Return null on error
    }
    
    return Value(); // null
}

std::string RegExp::to_string() const {
    return "/" + pattern_ + "/" + flags_;
}

void RegExp::parse_flags(const std::string& flags) {
    for (char flag : flags) {
        switch (flag) {
            case 'g':
                global_ = true;
                break;
            case 'i':
                ignore_case_ = true;
                break;
            case 'm':
                multiline_ = true;
                break;
            case 'u':
                unicode_ = true;
                break;
            case 'y':
                sticky_ = true;
                break;
            default:
                // Ignore unknown flags
                break;
        }
    }
}

std::regex::flag_type RegExp::get_regex_flags() const {
    std::regex::flag_type flags = std::regex::ECMAScript;
    
    if (ignore_case_) {
        flags |= std::regex::icase;
    }
    
    if (multiline_) {
        flags |= std::regex::multiline;
    }
    
    return flags;
}

} // namespace Quanta