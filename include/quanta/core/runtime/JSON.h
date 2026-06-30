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
#include <unordered_map>

namespace Quanta {

class Context;

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
        bool has_replacer_array;

        StringifyOptions() : indent(""), max_depth(100), quote_keys(true), escape_unicode(false), replacer_function(nullptr), has_replacer_array(false) {}
    };

    // Per-key source text of primitive children, recorded for the json-parse-with-source reviver context arg (objects/arrays never get one).
    using SourceMap = std::unordered_map<Object*, std::unordered_map<std::string, std::pair<std::string, Value>>>;

public:
    static Value parse(const std::string& json_string, const ParseOptions& options = ParseOptions());
    static Value parse(const std::string& json_string, const ParseOptions& options, SourceMap& out_source_map,
        std::string& out_root_source);
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
        SourceMap source_map_;
        std::string root_source_;

    public:
        Parser(const std::string& json, const ParseOptions& options);

        Value parse();
        const SourceMap& source_map() const { return source_map_; }
        const std::string& root_source() const { return root_source_; }

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

        // Records source text for a just-parsed primitive child, keyed by parent object + property name.
        void record_source(Object* parent, const std::string& key, size_t start, const Value& value);
    };
    
    class Stringifier {
    private:
        StringifyOptions options_;
        size_t depth_;
        std::set<const Object*> visited_;
        Context* context_;
        std::string current_key_;
        bool skip_bigint_toJSON_ = false;

    public:
        Stringifier(const StringifyOptions& options, Context* ctx = nullptr);
        
        std::string stringify(const Value& value);
        
    private:
        std::string stringify_value(const Value& value);
        Function* get_to_json(const Value& v);
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
