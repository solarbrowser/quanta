/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_JSON_H
#define QUANTA_JSON_H

#include "quanta/core/runtime/Value.h"
#include "quanta/core/runtime/Object.h"
#include <string>
#include <memory>
#include <set>

namespace Quanta {

class Context;

/**
 * JavaScript JSON object implementation
 * Provides JSON.parse() and JSON.stringify() functionality
 */
class JSON {
public:
    struct ParseOptions {
        bool allow_comments;
        bool allow_trailing_commas;
        bool allow_single_quotes;
        size_t max_depth;
        Function* reviver_function;
        Context* context;

        ParseOptions() : allow_comments(false), allow_trailing_commas(false),
                        allow_single_quotes(false), max_depth(100), reviver_function(nullptr), context(nullptr) {}
    };

    struct StringifyOptions {
        std::string indent;
        size_t max_depth;
        bool quote_keys;
        bool escape_unicode;
        Function* replacer_function;
        std::vector<std::string> replacer_array;

        StringifyOptions() : indent(""), max_depth(100), quote_keys(true), escape_unicode(false), replacer_function(nullptr) {}
    };

public:
    static Value parse(const std::string& json_string, const ParseOptions& options = ParseOptions());
    static std::string stringify(const Value& value, const StringifyOptions& options = StringifyOptions());
    
    static Value js_parse(Context& ctx, const std::vector<Value>& args);
    static Value js_stringify(Context& ctx, const std::vector<Value>& args);
    
    static std::unique_ptr<Object> create_json_object();

private:
    class Parser {
    private:
        std::string json_;
        size_t position_;
        size_t line_;
        size_t column_;
        size_t depth_;
        ParseOptions options_;
        
    public:
        Parser(const std::string& json, const ParseOptions& options);
        
        Value parse();
        
    private:
        Value parse_value();
        Value parse_object();
        Value parse_array();
        Value parse_string();
        Value parse_number();
        Value parse_boolean();
        Value parse_null();
        
        char current_char() const;
        char peek_char(size_t offset = 1) const;
        void advance();
        void skip_whitespace();
        bool at_end() const;
        
        [[noreturn]] void throw_syntax_error(const std::string& message);
        
        std::string parse_string_literal();
        std::string parse_escape_sequence();
        uint32_t parse_unicode_escape();
        
        double parse_number_literal();
        bool is_digit(char ch) const;
        bool is_hex_digit(char ch) const;
    };
    
    class Stringifier {
    private:
        StringifyOptions options_;
        size_t depth_;
        std::set<const Object*> visited_;
        Context* context_;

    public:
        Stringifier(const StringifyOptions& options, Context* ctx = nullptr);
        
        std::string stringify(const Value& value);
        
    private:
        std::string stringify_value(const Value& value);
        std::string stringify_object(const Object* obj);
        std::string stringify_array(const Object* arr);
        std::string stringify_string(const std::string& str);
        std::string stringify_number(double num);
        std::string stringify_boolean(bool value);
        
        std::string escape_string(const std::string& str);
        std::string get_indent() const;
        std::string get_newline() const;
        bool should_quote_key(const std::string& key) const;
        
        bool is_serializable(const Value& value) const;
        bool is_circular_reference(const Object* obj) const;
    };
    
    static bool is_whitespace(char ch);
    static bool is_valid_identifier_char(char ch);
    static std::string escape_json_string(const std::string& str);
    static std::string unescape_json_string(const std::string& str);
};

}

#endif
