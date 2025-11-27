/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_PARSER_H
#define QUANTA_PARSER_H

#include "AST.h"
#include "../../lexer/include/Token.h"
#include "../../lexer/include/Lexer.h"
#include <memory>
#include <vector>
#include <string>
#include <functional>

namespace Quanta {

/**
 * High-performance recursive descent parser for JavaScript
 * Features:
 * - Operator precedence parsing
 * - Error recovery
 * - Position tracking
 * - Memory-efficient AST construction
 */
class Parser {
public:
    struct ParseOptions {
        bool allow_return_outside_function = false;
        bool allow_await_outside_async = false;
        bool strict_mode = false;
        bool source_type_module = false;
    };

    struct ParseError {
        std::string message;
        Position position;
        std::string severity; // "error", "warning"
        
        ParseError(const std::string& msg, const Position& pos, const std::string& sev = "error")
            : message(msg), position(pos), severity(sev) {}
        
        std::string to_string() const {
            return severity + " at " + position.to_string() + ": " + message;
        }
    };

private:
    TokenSequence tokens_;
    ParseOptions options_;
    std::vector<ParseError> errors_;
    
    // Current parsing state
    size_t current_token_index_;

public:
    // Constructor
    explicit Parser(TokenSequence tokens);
    Parser(TokenSequence tokens, const ParseOptions& options);
    
    // Main parsing methods
    std::unique_ptr<Program> parse_program();
    std::unique_ptr<ASTNode> parse_statement();
    std::unique_ptr<ASTNode> parse_expression();
    
    // Statement parsing
    std::unique_ptr<ASTNode> parse_variable_declaration();
    std::unique_ptr<ASTNode> parse_variable_declaration(bool consume_semicolon);
    std::unique_ptr<ASTNode> parse_block_statement();
    std::unique_ptr<ASTNode> parse_if_statement();
    std::unique_ptr<ASTNode> parse_for_statement();
    std::unique_ptr<ASTNode> parse_while_statement();
    std::unique_ptr<ASTNode> parse_do_while_statement();
    std::unique_ptr<ASTNode> parse_function_declaration();
    std::unique_ptr<ASTNode> parse_async_function_declaration();
    std::unique_ptr<ASTNode> parse_class_declaration();
    std::unique_ptr<ASTNode> parse_method_definition();
    std::unique_ptr<ASTNode> parse_return_statement();
    std::unique_ptr<ASTNode> parse_break_statement();
    std::unique_ptr<ASTNode> parse_continue_statement();
    std::unique_ptr<ASTNode> parse_expression_statement();
    
    // Stage 9: Error handling and advanced control flow
    std::unique_ptr<ASTNode> parse_try_statement();
    std::unique_ptr<ASTNode> parse_throw_statement();
    std::unique_ptr<ASTNode> parse_switch_statement();
    std::unique_ptr<ASTNode> parse_catch_clause();
    
    // Stage 10: Modules
    std::unique_ptr<ASTNode> parse_import_statement();
    std::unique_ptr<ASTNode> parse_export_statement();
    std::unique_ptr<ImportSpecifier> parse_import_specifier();
    std::unique_ptr<ExportSpecifier> parse_export_specifier();
    
    // Expression parsing with precedence
    std::unique_ptr<ASTNode> parse_assignment_expression();
    std::unique_ptr<ASTNode> parse_conditional_expression();
    std::unique_ptr<ASTNode> parse_conditional_expression_impl(int depth);
    std::unique_ptr<ASTNode> parse_logical_or_expression();
    std::unique_ptr<ASTNode> parse_nullish_coalescing_expression();
    std::unique_ptr<ASTNode> parse_logical_and_expression();
    std::unique_ptr<ASTNode> parse_bitwise_or_expression();
    std::unique_ptr<ASTNode> parse_bitwise_xor_expression();
    std::unique_ptr<ASTNode> parse_bitwise_and_expression();
    std::unique_ptr<ASTNode> parse_equality_expression();
    std::unique_ptr<ASTNode> parse_relational_expression();
    std::unique_ptr<ASTNode> parse_shift_expression();
    std::unique_ptr<ASTNode> parse_additive_expression();
    std::unique_ptr<ASTNode> parse_multiplicative_expression();
    std::unique_ptr<ASTNode> parse_exponentiation_expression();
    std::unique_ptr<ASTNode> parse_unary_expression();
    std::unique_ptr<ASTNode> parse_postfix_expression();
    std::unique_ptr<ASTNode> parse_call_expression();
    std::unique_ptr<ASTNode> parse_member_expression();
    std::unique_ptr<ASTNode> parse_primary_expression();
    std::unique_ptr<ASTNode> parse_parenthesized_expression();
    std::unique_ptr<ASTNode> parse_function_expression();
    std::unique_ptr<ASTNode> parse_async_function_expression();
    std::unique_ptr<ASTNode> parse_arrow_function();
    std::unique_ptr<ASTNode> parse_async_arrow_function(Position start);
    std::unique_ptr<ASTNode> parse_async_arrow_function_single_param(Position start);
    std::unique_ptr<ASTNode> parse_yield_expression();
    std::unique_ptr<ASTNode> parse_import_expression();
    bool try_parse_arrow_function_params();
    std::unique_ptr<ASTNode> parse_object_literal();
    std::unique_ptr<ASTNode> parse_array_literal();
    std::unique_ptr<ASTNode> parse_destructuring_pattern(int depth = 0);
    std::unique_ptr<ASTNode> parse_spread_element();
    std::string extract_nested_variable_names(ASTNode* node);
    std::string generate_proper_nested_pattern(ASTNode* node, int depth);
    void extract_variable_names_recursive(ASTNode* node, std::vector<std::string>& names);
    
    // Literal parsing
    std::unique_ptr<ASTNode> parse_number_literal();
    std::unique_ptr<ASTNode> parse_string_literal();
    std::unique_ptr<ASTNode> parse_private_field();
    std::unique_ptr<ASTNode> parse_this_expression();
    std::unique_ptr<ASTNode> parse_super_expression();
    std::unique_ptr<ASTNode> parse_template_literal();
    
    // JSX parsing
    std::unique_ptr<ASTNode> parse_jsx_element();
    std::unique_ptr<ASTNode> parse_jsx_text();
    std::unique_ptr<ASTNode> parse_jsx_expression();
    std::unique_ptr<ASTNode> parse_jsx_attribute();
    std::unique_ptr<ASTNode> parse_regex_literal();
    std::unique_ptr<ASTNode> parse_boolean_literal();
    std::unique_ptr<ASTNode> parse_null_literal();
    std::unique_ptr<ASTNode> parse_bigint_literal();
    std::unique_ptr<ASTNode> parse_undefined_literal();
    std::unique_ptr<ASTNode> parse_identifier();
    
    // Utility methods
    const Token& current_token() const;
    const Token& peek_token(size_t offset = 1) const;
    void advance();
    bool match(TokenType type);
    bool match_any(const std::vector<TokenType>& types);
    bool consume(TokenType type);
    bool consume_if_match(TokenType type);
    bool is_reserved_word_as_property_name();
    void check_for_use_strict_directive();
    bool is_strict_mode() const { return options_.strict_mode; }
    
    // Error handling
    void add_error(const std::string& message);
    void add_error(const std::string& message, const Position& position);
    const std::vector<ParseError>& get_errors() const { return errors_; }
    bool has_errors() const { return !errors_.empty(); }
    
    // Position tracking
    Position get_current_position() const;
    
    // Utility
    bool at_end() const;
    
private:
    // Helper methods
    std::unique_ptr<ASTNode> parse_binary_expression(
        std::function<std::unique_ptr<ASTNode>()> parse_operand,
        const std::vector<TokenType>& operators
    );
    
    BinaryExpression::Operator token_to_binary_operator(TokenType type);
    UnaryExpression::Operator token_to_unary_operator(TokenType type);
    
    // Error recovery
    void skip_to_statement_boundary();
    void skip_to(TokenType type);
    
    // Validation
    bool is_assignment_operator(TokenType type) const;
    bool is_binary_operator(TokenType type) const;
    bool is_unary_operator(TokenType type) const;
    bool is_keyword_token(TokenType type) const;
    bool is_valid_assignment_target(ASTNode* node) const;
};

/**
 * Parser factory for different parsing modes
 */
namespace ParserFactory {
    std::unique_ptr<Parser> create_expression_parser(const std::string& source);
    std::unique_ptr<Parser> create_statement_parser(const std::string& source);
    std::unique_ptr<Parser> create_module_parser(const std::string& source);
}

} // namespace Quanta

#endif // QUANTA_PARSER_H