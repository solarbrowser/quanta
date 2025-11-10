/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "../../include/Value.h"
#include <string>
#include <memory>
#include <vector>

namespace Quanta {

class Object;
class Context;

/**
 * High-performance JSON parser and stringifier
 */
class JSONParser {
public:
    enum class ParseError {
        None,
        UnexpectedToken,
        InvalidNumber,
        InvalidString,
        InvalidEscape,
        UnterminatedString,
        UnterminatedArray,
        UnterminatedObject,
        TrailingComma,
        DuplicateKey
    };

    struct ParseResult {
        Value value;
        ParseError error;
        size_t error_position;
        std::string error_message;

        ParseResult() : error(ParseError::None), error_position(0) {}
        ParseResult(ParseError err, size_t pos, const std::string& msg)
            : error(err), error_position(pos), error_message(msg) {}
    };

private:
    const char* input_;
    size_t position_;
    size_t length_;
    bool strict_mode_;

public:
    JSONParser(bool strict = false);
    ~JSONParser() = default;

    // Parsing
    ParseResult parse(const std::string& json_string);
    ParseResult parse(const char* json_data, size_t length);

    // Stringification
    std::string stringify(const Value& value, bool pretty = false);
    std::string stringify(const Value& value, const Value& replacer, const Value& space);

    // Configuration
    void set_strict_mode(bool strict) { strict_mode_ = strict; }
    bool get_strict_mode() const { return strict_mode_; }

    // Static methods for JavaScript binding
    static Value json_parse(Context& ctx, const std::vector<Value>& args);
    static Value json_stringify(Context& ctx, const std::vector<Value>& args);

    // Setup
    static void setup_json_object(Context& ctx);

private:
    // Parsing helpers
    Value parse_value();
    Value parse_object();
    Value parse_array();
    Value parse_string();
    Value parse_number();
    Value parse_literal();

    // Utility
    void skip_whitespace();
    char peek_char() const;
    char consume_char();
    bool match_string(const char* str);
    ParseError set_error(ParseError error, const std::string& message);

    // Stringification helpers
    std::string stringify_value(const Value& value, int indent_level);
    std::string stringify_object(Object* obj, int indent_level);
    std::string stringify_array(Object* arr, int indent_level);
    std::string stringify_string(const std::string& str);
    std::string escape_string(const std::string& str);
    std::string create_indent(int level);
};

} // namespace Quanta