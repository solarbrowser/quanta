#include "quanta/core/runtime/StackFloor.h"
#include <cstdlib>
/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_PARSER_H
#define QUANTA_PARSER_H

#include "quanta/parser/AST.h"
#include "quanta/parser/ScriptUnit.h"
#include "quanta/lexer/Token.h"
#include "quanta/lexer/Lexer.h"
#include <memory>
#include <vector>
#include <unordered_set>
#include <string>
#include <string>
#include <functional>

namespace Quanta {

class Parser {
public:
    struct ParseOptions {
        bool allow_return_outside_function = false;
        bool allow_await_outside_async = false;
        bool strict_mode = false;
        bool source_type_module = false;
        bool in_async_body = false;
        bool in_generator_body = false;
        bool in_arrow_params = false;
        bool in_class_field_init = false;
        bool in_array_element = false;
        bool in_class_method = false;
        bool in_constructor = false;
        bool class_has_heritage = false;
        bool in_class_static_block = false;
        bool in_eval_context = false;
        bool eval_in_function_code = false;  // true if eval is called from inside a function/method
        bool eval_in_method_code = false;    // true if eval is called from inside a method with [[HomeObject]]
        bool in_substatement_body = false;   
        bool in_switch_case_list = false;   
        bool in_block_context = false;      
        int loop_depth = 0;
        int switch_depth = 0;
        int function_depth = 0;
        int non_arrow_function_depth = 0;
        int class_depth = 0;
        bool in_binary_expr = false;
        bool in_unary_operand = false;
        std::unordered_set<std::string> active_labels;
        std::unordered_set<std::string> loop_labels;
        std::unordered_set<std::string> eval_private_names;
    };

    struct ParseError {
        std::string message;
        Position position;
        std::string severity;
        
        ParseError(const std::string& msg, const Position& pos, const std::string& sev = "error")
            : message(msg), position(pos), severity(sev) {}
        
        std::string to_string() const {
            return severity + " at " + position.to_string() + ": " + message;
        }
    };

private:
    TokenSequence tokens_;
    bool detached_tokens_ = false;
    // Token span of the most recently parsed function body, so the literal
    // built right after it can record where its body lives. Only read
    // immediately after that body's parse returns, before any nested parse
    // can overwrite it.
    size_t last_body_tok_first_ = 0;
    uint32_t last_body_src_first_ = 0;
    size_t last_body_tok_last_ = 0;

    // Subtree facts gathered on the way up (see SubtreeFlags). A bit is set
    // where the parser recognises the construct; whoever closes a subtree
    // reads what accumulated inside it, hands it to the node, and passes on
    // only the bits that cross that boundary.
    uint32_t subtree_acc_ = 0;

    // One per function being read, innermost last. `all` gathers every name
    // the function mentions, `captured` only the ones something nested in it
    // mentions -- which is the set that has to stay in the environment. A
    // function closing folds its whole `all` into its parent's `captured`,
    // because from the parent's side every one of those names is inside a
    // closure.
    struct NameScope {
        std::unordered_set<std::string> all;
        std::unordered_set<std::string> captured;
        bool eval_in_nested = false;
        bool class_expression = false;
    };
    std::vector<NameScope> name_scopes_;

    // Turns name recording off for a stretch that reads a property name
    // rather than a binding.
    struct NameRecording {
        Parser& p;
        bool saved;
        explicit NameRecording(Parser& parser) : p(parser), saved(parser.recording_names_) {
            p.recording_names_ = false;
        }
        NameRecording(const NameRecording&) = delete;
        NameRecording& operator=(const NameRecording&) = delete;
        ~NameRecording() { p.recording_names_ = saved; }
    };
    bool recording_names_ = true;
    // A destructuring form is handed whole to the tree-walker, which reads its
    // names out of the environment, so every name one touches has to be there
    // whether or not a closure could otherwise see it.
    int forcing_capture_ = 0;
    struct ForceCapture {
        Parser& p;
        explicit ForceCapture(Parser& parser) : p(parser) { p.forcing_capture_++; }
        ForceCapture(const ForceCapture&) = delete;
        ForceCapture& operator=(const ForceCapture&) = delete;
        ~ForceCapture() { p.forcing_capture_--; }
    };

    void note_class_expression() {
        if (!name_scopes_.empty()) name_scopes_.back().class_expression = true;
    }

    void note_name(const std::string& n) {
        if (!recording_names_) return;
        if (name_scopes_.empty()) return;
        name_scopes_.back().all.insert(n);
        if (forcing_capture_ > 0) name_scopes_.back().captured.insert(n);
    }
    // Opens the scope of a function literal; closing it hands what the
    // function mentioned to whatever encloses it.
    struct FunctionNames {
        Parser& p;
        explicit FunctionNames(Parser& parser) : p(parser) {
            p.name_scopes_.emplace_back();
        }
        FunctionNames(const FunctionNames&) = delete;
        FunctionNames& operator=(const FunctionNames&) = delete;
        // Where the body this function owns opens, learned once the body has
        // been read; the info is filed under it so a body parsed back later
        // finds the same entry.
        uint32_t body_src = 0;
        bool has_body = false;
        void record_body(uint32_t src) { body_src = src; has_body = true; }
        BodyScopeInfo take() const {
            BodyScopeInfo info;
            const NameScope& mine = p.name_scopes_.back();
            info.captured = mine.captured;
            info.eval_anywhere = mine.all.count("eval") != 0;
            info.super_anywhere = mine.all.count("super") != 0;
            info.eval_in_nested = mine.eval_in_nested;
            info.class_expression = mine.class_expression;
            return info;
        }
        ~FunctionNames() {
            if (has_body) {
                if (ScriptUnit* unit = ScriptUnit::building()) {
                    unit->set_scope_info_at(body_src, take());
                }
            }
            NameScope mine = std::move(p.name_scopes_.back());
            p.name_scopes_.pop_back();
            if (p.name_scopes_.empty()) return;
            NameScope& parent = p.name_scopes_.back();
            const bool eval_here = mine.all.count("eval") != 0 || mine.eval_in_nested;
            for (auto& n : mine.all) {
                parent.captured.insert(n);
                parent.all.insert(std::move(n));
            }
            parent.eval_in_nested = parent.eval_in_nested || eval_here;
            parent.class_expression = parent.class_expression || mine.class_expression;
        }
    };
    bool previous_token_is_dot() const;

    // Opens a fresh accumulator for one subtree and, however that parse
    // leaves, folds back into the enclosing one only the bits that cross this
    // boundary. The default crosses everything, which is what a statement or a
    // block does; a nested function passes a narrower mask.
    struct SubtreeScope {
        Parser& p;
        uint32_t saved;
        uint32_t crossing;
        explicit SubtreeScope(Parser& parser, uint32_t crossing_mask = ~0u)
            : p(parser), saved(parser.subtree_acc_), crossing(crossing_mask) {
            p.subtree_acc_ = 0;
        }
        SubtreeScope(const SubtreeScope&) = delete;
        SubtreeScope& operator=(const SubtreeScope&) = delete;
        uint32_t flags() const { return p.subtree_acc_ | kSubtreeComputed; }
        // What the construct contributes to whatever encloses it, whether or
        // not it lets the same bit travel out from inside: a class stops the
        // lexical bindings in its body and is one itself.
        void contribute(uint32_t bits) { saved |= bits; }
        ~SubtreeScope() { p.subtree_acc_ = saved | (p.subtree_acc_ & crossing); }
    };

    ParseOptions options_;
    std::vector<ParseError> errors_;
    // Shared, not owned: a function body lexed back out of a script points
    // this at the script's own buffer, and there is one of those per body.
    // Copying the whole script for each was most of what compiling a large
    // one spent its time doing.
    std::shared_ptr<const std::string> source_;
    static const std::string& no_source() { static const std::string e; return e; }

    size_t current_token_index_;
    bool no_in_mode_ = false; // when true, 'in' is not parsed as a relational operator
    bool last_expr_was_parenthesized_ = false;
    // Nesting of `? :` alternates. A member rather than a recursion parameter
    // because the alternate is parsed as a full AssignmentExpression, so the
    // chain leaves and re-enters parse_conditional_expression and a parameter
    // would reset to zero at every link.
    // A recursive-descent parser is bounded by the C++ stack and nothing
    // else: nested parentheses, array literals and function expressions each
    // cost a frame per level, and their per-level cost differs by more than a
    // factor of two, so counting levels cannot say how close the stack is.
    // Measuring it can.
    //
    // Two ways to measure, because there are two kinds of stack to be on. A
    // fiber's extent is known exactly, so the check is against its floor. A
    // thread's is not tracked here, so the check is how far this parse has
    // come from where it started, against a budget taken from the thread's
    // limit. Which applies is decided per parse rather than per parser: a
    // generator or async function resumes on a fiber of its own, and anything
    // parsed while it runs is parsed over there.
    const char* stack_base_ = nullptr;
    const char* stack_floor_ = nullptr;
    size_t stack_budget_ = 0;
    size_t parse_depth_ = 0;
    static size_t thread_stack_budget();
    struct StackMark {
        Parser* parser;
        explicit StackMark(Parser* p) : parser(p) { parser->parse_depth_++; }
        ~StackMark() { parser->parse_depth_--; }
    };
    bool stack_exhausted(const char* here) {
        if (parse_depth_ == 0) {
            stack_floor_ = current_stack_floor();
            stack_base_ = here;
            stack_budget_ = thread_stack_budget();
            return false;
        }
        if (stack_floor_) return here < stack_floor_;
        return static_cast<size_t>(stack_base_ - here) > stack_budget_;
    }
    std::vector<std::unordered_set<std::string>> private_scope_stack_; // private names per class depth

public:
    explicit Parser(TokenSequence tokens);
    Parser(TokenSequence tokens, const ParseOptions& options);
    
    std::unique_ptr<Program> parse_program();
    // Same parse, wrapped in a ScriptUnit so the function literals inside are
    // stamped with an owner and their executables can borrow their bodies
    // instead of copying them. Callers that still hand a bare tree to
    // parse_program() keep the copying behaviour.
    ExecutableRef<ScriptUnit> parse_program_unit();
    std::unique_ptr<ASTNode> parse_statement();
    std::unique_ptr<ASTNode> parse_expression();
    
    std::unique_ptr<ASTNode> parse_variable_declaration();
    std::unique_ptr<ASTNode> parse_variable_declaration(bool consume_semicolon);
    std::unique_ptr<ASTNode> parse_block_statement(bool is_function_body = false);
    std::unique_ptr<ASTNode> parse_if_statement();
    std::unique_ptr<ASTNode> parse_for_statement();
    std::unique_ptr<ASTNode> parse_while_statement();
    std::unique_ptr<ASTNode> parse_do_while_statement();
    std::unique_ptr<ASTNode> parse_with_statement();
    std::unique_ptr<ASTNode> parse_function_declaration();
    std::unique_ptr<ASTNode> parse_async_function_declaration();
    std::unique_ptr<ASTNode> parse_class_declaration();
    std::unique_ptr<ASTNode> parse_class_expression();
    std::unique_ptr<ASTNode> parse_method_definition();
    std::unique_ptr<ASTNode> parse_return_statement();
    std::unique_ptr<ASTNode> parse_break_statement();
    std::unique_ptr<ASTNode> parse_continue_statement();
    std::unique_ptr<ASTNode> parse_expression_statement();
    
    std::unique_ptr<ASTNode> parse_try_statement();
    std::unique_ptr<ASTNode> parse_throw_statement();
    std::unique_ptr<ASTNode> parse_switch_statement();
    std::unique_ptr<ASTNode> parse_catch_clause();
    
    std::unique_ptr<ASTNode> parse_using_declaration(bool is_await, bool consume_semicolon = true);
    std::unique_ptr<ASTNode> parse_import_statement();
    std::unique_ptr<ASTNode> parse_export_statement();
    std::unique_ptr<ImportSpecifier> parse_import_specifier();
    std::unique_ptr<ExportSpecifier> parse_export_specifier();
    
    std::unique_ptr<ASTNode> parse_assignment_expression();
    std::unique_ptr<ASTNode> parse_conditional_expression();
    std::unique_ptr<ASTNode> parse_logical_or_expression();
    std::unique_ptr<ASTNode> parse_nullish_coalescing_expression();
    int binary_precedence(TokenType type) const;
    std::unique_ptr<ASTNode> parse_binary_chain(int min_precedence);
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
    bool validate_binding_pattern(ASTNode* pattern);
    std::unique_ptr<ASTNode> parse_destructuring_pattern(int depth = 0);
    std::unique_ptr<ASTNode> parse_spread_element();
    void extract_variable_names_recursive(ASTNode* node, std::vector<std::string>& names);
    
    std::unique_ptr<ASTNode> parse_number_literal();
    std::unique_ptr<ASTNode> parse_string_literal();
    std::unique_ptr<ASTNode> parse_private_field();
    std::unique_ptr<ASTNode> parse_this_expression();
    std::unique_ptr<ASTNode> parse_super_expression();
    std::unique_ptr<ASTNode> parse_template_literal();
    
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
    
    // A token records where its text is, not the text itself, so reading it
    // goes through the sequence that owns the source. The result borrows from
    // that source and stays valid for as long as this parser does.
    std::string_view token_text(const Token& token) const { return tokens_.text_of(token); }
    // Same text, copied, for the callers that have to own it -- a name that
    // goes into an AST node, a key that outlives the parse.
    std::string token_string(const Token& token) const { return std::string(token_text(token)); }

    const Token& current_token() const;
    const Token& peek_token(size_t offset = 1) const;
    const Token& previous_token() const;
    void advance();
    bool match(TokenType type);
    bool match_any(const std::vector<TokenType>& types);
    bool consume(TokenType type);
    bool consume_if_match(TokenType type);
    // Contextual keywords that are never reserved and so may be binding names.
    static bool token_is_unreserved_contextual(TokenType type);
    bool is_reserved_word_as_property_name();
    void check_for_use_strict_directive();
    bool is_strict_mode() const { return options_.strict_mode; }
    // Parses one function body starting at `tok_index`, which must be the
    // index of its opening brace. Used to build a body that was recorded as a
    // token range instead of being kept as a tree; the caller supplies the
    // context the body was originally parsed in, since a body's grammar
    // depends on it (yield and await are identifiers or operators depending
    // on the enclosing function's kind).
    std::unique_ptr<ASTNode> parse_body_at(size_t tok_index, bool strict,
                                           bool is_generator, bool is_async);
    // Hands the token stream to whoever will keep the tree, so a body recorded
    // as a range can be parsed back later. The parser is finished with it by
    // then -- parse_program_unit does the same thing at the end of a parse.
    TokenSequence take_tokens() { return std::move(tokens_); }

    // A parser whose tokens are its own throwaway stream, not the one its unit
    // keeps -- template substitutions are re-lexed and parsed this way. The
    // literals it builds are still stamped with the enclosing unit, so they
    // must not record body ranges: those indices address a stream nobody can
    // parse from later, and a body rebuilt at one would be arbitrary code.
    void set_detached_tokens(bool v) { detached_tokens_ = v; }
    bool check_substatement_restrictions(bool is_loop_body = true);
    bool validate_array_destructuring(ArrayLiteral* arr);
    bool validate_object_destructuring(ObjectLiteral* obj);
    
    void add_error(const std::string& message);
    void add_error(const std::string& message, const Position& position);
    const std::vector<ParseError>& get_errors() const { return errors_; }
    bool has_errors() const { return !errors_.empty(); }
    
    void set_source(const std::string& src) {
        source_ = std::make_shared<const std::string>(src);
    }
    void set_source(std::shared_ptr<const std::string> src) { source_ = std::move(src); }
    const std::string& source_text() const { return source_ ? *source_ : no_source(); }
    std::string get_source_slice(size_t start_offset, size_t end_offset) const {
        const std::string& s = source_text();
        if (s.empty() || start_offset >= s.size()) return "";
        if (end_offset > s.size()) end_offset = s.size();
        return s.substr(start_offset, end_offset - start_offset);
    }
    // advance() skips trivia, so previous_token() points at the last skipped NEWLINE/COMMENT, not the real token
    const Token& last_meaningful_token() const {
        size_t idx = current_token_index_;
        while (idx > 0) {
            idx--;
            TokenType t = tokens_[idx].get_type();
            if (t != TokenType::WHITESPACE && t != TokenType::NEWLINE && t != TokenType::COMMENT)
                return tokens_[idx];
        }
        return tokens_[0];
    }
    Position get_current_position() const;
    
    bool at_end() const;
    
private:
    
    BinaryExpression::Operator token_to_binary_operator(TokenType type);
    UnaryExpression::Operator token_to_unary_operator(TokenType type);
    
    void skip_to_statement_boundary();
    void skip_to(TokenType type);
    void skip_decorator_list();
    
    bool is_assignment_operator(TokenType type) const;
    bool is_binary_operator(TokenType type) const;
    bool is_unary_operator(TokenType type) const;
    bool is_keyword_token(TokenType type) const;
    bool is_valid_assignment_target(ASTNode* node) const;

    // Spec early errors: "It is a Syntax Error if FormalParameters Contains
    // YieldExpression/AwaitExpression is true" (GeneratorDeclaration/Expression,
    // AsyncFunctionDeclaration/Expression, AsyncGeneratorDeclaration/Expression).
    // Scans each parameter's default value / destructuring-pattern initializer for a
    // YieldExpression (if check_yield) or AwaitExpression (if check_await), without
    // crossing into nested function/class boundaries (they have their own [Yield]/[Await]
    // scope). Returns "yield"/"await" naming the first forbidden expression found, or ""
    // if neither is present.
    std::string find_forbidden_expr_in_params(
        const std::vector<std::unique_ptr<Parameter>>& params, bool check_yield, bool check_await) const;
};

namespace ParserFactory {
    std::unique_ptr<Parser> create_expression_parser(const std::string& source);
    std::unique_ptr<Parser> create_statement_parser(const std::string& source);
    std::unique_ptr<Parser> create_module_parser(const std::string& source);
}

}

#endif
