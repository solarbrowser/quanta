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
#include <unordered_map>
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
    bool in_program_unit_ = false;
    bool reading_back_ = false;
    // Token span of the most recently parsed function body, so the literal
    // built right after it can record where its body lives. Only read
    // immediately after that body's parse returns, before any nested parse
    // can overwrite it.
    size_t last_body_tok_first_ = 0;
    uint32_t last_body_src_first_ = 0;
    uint32_t last_body_src_last_ = 0;
    bool last_body_strict_ = false;
    // Set while one body is being read back out of the source. A function
    // written inside it is not read: what its own body would have said was
    // written down the first time round, so the parse steps over it and the
    // literal keeps only the range. Only bodies deeper than the one being read
    // are stepped over -- that one is the point of the read.
    bool lazy_inner_bodies_ = false;
    int lazy_base_depth_ = 0;
    // Whether the body just asked for was stepped over rather than read. The
    // literal still gets built -- from the range, with no subtree.
    bool last_body_skipped_ = false;
    bool skip_recorded_body();
    size_t last_body_tok_last_ = 0;

    // Subtree facts gathered on the way up (see SubtreeFlags). A bit is set
    // where the parser recognises the construct; whoever closes a subtree
    // reads what accumulated inside it, hands it to the node, and passes on
    // only the bits that cross that boundary.
    uint32_t subtree_acc_ = 0;

    // One per lexical scope being read, innermost last -- a unified stack
    // that holds BOTH function-literal frames (FunctionNames) and plain
    // block frames (LexicalBlockScope: a `{}`, a catch parameter, a switch
    // body, a for(let/const) head), mirroring the real nesting of the JS
    // scope tree instead of approximating it with one flat set per function.
    // `all` gathers every name mentioned inside this scope (directly, or
    // folded up from something nested in it); `captured` is the subset that
    // crossed an actual closure boundary somewhere below and so has to stay
    // in an Environment rather than a register. A scope closing folds into
    // whatever is now the new top of the stack via fold_into -- see its own
    // comment for exactly how `all` and `captured` propagate differently.
    struct NameScope {
        // Interned (NamePool) ids, not text: a bundle-sized file closes
        // tens of thousands of these, and the same few thousand names
        // recur across nearly all of them -- an id set dedupes that for
        // free and costs 4 bytes a name instead of a whole string plus a
        // hash-set node.
        IdSet all;
        IdSet captured;
        // Every name this scope itself declares -- a function's own simple
        // (non-destructured) parameters, var declarators, and a named
        // function/class expression's own self-reference for a FUNCTION
        // frame (record_params/record_param, declare_local_name); a
        // let/const/class/catch-param declarator for a BLOCK frame
        // (declare_block_scoped_name). Withheld entirely from `all` (not
        // just from `captured`) when this scope folds into its parent, see
        // fold_into: a name fully contained here never reaches any
        // ancestor's `all` at all, which is what makes the exclusion
        // correct without ever exporting a flag across levels -- re-decided
        // fresh from only THIS scope's own local set, every single time.
        // `all` still keeps every one of these names for THIS scope's own
        // snapshot (a plain read of its own declaration is recorded exactly
        // like any other identifier, note_name does not know the
        // difference) -- only the fold into the parent withholds them.
        IdSet declared_here;
        bool eval_in_nested = false;
        bool class_expression = false;
        // True for a frame opened by a function literal (FunctionNames);
        // false for a frame opened by a plain lexical block
        // (LexicalBlockScope). name_scopes_ is a unified stack of both kinds
        // -- a block nested in a function, a function nested in a block, and
        // a block nested in a block are all just adjacent stack entries.
        // fold_into treats every level the same way for `all` (exclude this
        // frame's own declared_here, full stop); the one place frame kind
        // matters is `captured`'s relay -- see fold_into's own comment.
        bool is_function = false;

        // Folds `all - declared_here` into `parent`, exactly the rule
        // FunctionNames::~FunctionNames used to inline (see git history for
        // the original), now shared by every frame kind so a function
        // nested inside a block behaves identically to a function nested in
        // a function, and a block nested in a block just repeats the same
        // rule one more time. Never exports anything beyond this one fold: a
        // name in `declared_here` stops here, full stop, re-decided fresh at
        // every single level from only that level's own local set -- the
        // "re-decide fresh, never export a flag" discipline that fixed two
        // earlier reverted attempts (7d230c1c/580734dd) at this exact
        // problem, now generalized from function-only to every lexical
        // level.
        void fold_into(NameScope& parent) const {
            for (auto n : all) {
                if (declared_here.count(n)) continue;
                parent.all.insert(n);
                // A function frame closing always crosses a closure
                // boundary, so every surviving name becomes captured in the
                // parent too -- the exact behavior this replaces.
                if (is_function) parent.captured.insert(n);
            }
            // `captured` is a DIFFERENT question from `all`'s and is never
            // gated by declared_here for a block close: it answers "does
            // some declared name of mine need environment residency," fed
            // straight into the ENCLOSING FUNCTION's own compile pass (which
            // checks it against that function's OWN locally declared names,
            // wherever within it they live -- a for-loop's own `let i`, a
            // bare block's own `let e`, whatever). Whether THIS block itself
            // owns the declaration is irrelevant to that question -- a name
            // this block declares and a closure directly inside it captures
            // still has to reach the function that will actually allocate
            // storage for it, however many further block-only hops separate
            // them. `all`'s exclusion is the one guarding against same-text
            // confusion with an unrelated binding further out -- without
            // this decoupling, a for-loop/bare-block's own let, captured by
            // a closure inside it, would silently lose its residency mark
            // once it stopped short at the block that legitimately excludes
            // it from `all`. A function frame never
            // needs this second loop: every name already relayed via the
            // branch above (mine.all, minus mine.declared_here) already
            // becomes captured there, unconditionally, so nothing here would
            // add anything new.
            if (!is_function) {
                for (auto n : captured) parent.captured.insert(n);
            }
            const bool eval_here = all.count(NamePool::intern("eval")) != 0 || eval_in_nested;
            parent.eval_in_nested = parent.eval_in_nested || eval_here;
            parent.class_expression = parent.class_expression || class_expression;
        }
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
    void note_class_expression() {
        if (!name_scopes_.empty()) name_scopes_.back().class_expression = true;
    }

    void note_name(const std::string& n) {
        if (!recording_names_) return;
        if (name_scopes_.empty()) return;
        name_scopes_.back().all.insert(NamePool::intern(n));
    }
    // For a declaration form that is unambiguously function-scoped
    // regardless of which block it is textually nested in (var, a named
    // function expression's own self-reference) -- see NameScope::
    // declared_here's own comment for why block-scoped forms (let/const/
    // class/catch params) must never call this. Unlike note_name, this
    // does not need a recording_names_ guard: it is only ever called from
    // a declaration's own parse, never from the property-name contexts
    // NameRecording suppresses.
    //
    // Walks up to the nearest FUNCTION frame rather than using whatever is
    // innermost, because name_scopes_ is a unified stack that can also hold
    // plain block frames (LexicalBlockScope) now -- a `var` declared inside
    // a block is still function-scoped by spec, so it must land in the
    // function's own declared_here, never a block's, or that block's own
    // close would wrongly withhold it from the function that actually owns
    // it. A no-op change at every call site reached with no block open
    // (params, a function/class expression's own self-name -- all recorded
    // immediately after their own FunctionNames is pushed, before any block
    // exists), and exactly the fix needed once a block CAN be open.
    void declare_local_name(const std::string& n) {
        if (name_scopes_.empty()) return;
        const uint32_t id = NamePool::intern(n);
        for (auto it = name_scopes_.rbegin(); it != name_scopes_.rend(); ++it) {
            if (it->is_function) { it->declared_here.insert(id); return; }
        }
    }
    // For a declaration form that IS block-scoped (let/const/class/catch
    // params/a for-loop's own let/const binder) -- writes into whichever
    // frame is innermost right now, block or function. Safe to add
    // unconditionally, unlike declare_local_name: a block frame's own close
    // (LexicalBlockScope's destructor) applies the exact same "re-decide
    // fresh from only this frame's own declared_here" rule FunctionNames
    // already uses (see NameScope::fold_into), so a name declared here can
    // never be confused with an unrelated same-text binding that lives
    // outside this specific block.
    void declare_block_scoped_name(const std::string& n) {
        if (name_scopes_.empty()) return;
        name_scopes_.back().declared_here.insert(NamePool::intern(n));
    }
    // Opens the scope of a function literal; closing it hands what the
    // function mentioned to whatever encloses it.
    struct FunctionNames {
        Parser& p;
        explicit FunctionNames(Parser& parser) : p(parser) {
            p.name_scopes_.emplace_back();
            p.name_scopes_.back().is_function = true;
        }
        FunctionNames(const FunctionNames&) = delete;
        FunctionNames& operator=(const FunctionNames&) = delete;
        // Where the body this function owns opens, learned once the body has
        // been read; the info is filed under it so a body parsed back later
        // finds the same entry.
        uint32_t body_src = 0;
        bool has_body = false;
        uint32_t body_end = 0;
        bool body_strict = false;
        bool captures_outer = true;
        void record_body(uint32_t src) { body_src = src; has_body = true; }
        void record_body_span(uint32_t end, bool strict) {
            body_end = end;
            body_strict = strict;
        }
        // Only the forms that can act on it bother to work this out; the rest
        // leave the safe default standing.
        void record_capture(bool captures) { captures_outer = captures; }
        IdSet free_names;
        bool free_valid = false;
        bool free_unknown = false;
        bool free_saw_eval = false;
        bool free_saw_class = false;
        bool method_super_valid = false;
        bool method_references_super = true;
        void record_method_super(bool references) {
            method_super_valid = true;
            method_references_super = references;
        }
        void record_free(const std::vector<std::string>& names, bool unknown, bool saw_eval, bool saw_class) {
            for (const auto& n : names) free_names.insert(NamePool::intern(n));
            free_valid = true;
            free_unknown = unknown;
            free_saw_eval = saw_eval;
            free_saw_class = saw_class;
            captures_outer = saw_eval || saw_class || unknown || !names.empty();
        }
        // Called once the parameter list is fully parsed, before the body:
        // a simple (non-destructured) parameter's own name is never one
        // this function needs FROM its enclosing scope, however many times
        // its own body reads it -- see NameScope::declared_here' own comment.
        // A destructured parameter's synthetic name is skipped: nothing in
        // the body ever reads it by that name, so there is nothing to
        // exclude, and the names the pattern itself binds are its own,
        // separate declarations (not parameters), out of scope for this.
        void record_params(const std::vector<std::unique_ptr<Parameter>>& params) {
            if (p.name_scopes_.empty()) return;
            NameScope& mine = p.name_scopes_.back();
            for (const auto& param : params) {
                if (param->has_destructuring()) continue;
                if (const Identifier* id = param->get_name()) mine.declared_here.insert(NamePool::intern(id->get_name()));
            }
        }
        // Single-identifier arrow form (`x => ...`), which never builds a
        // Parameter vector at all.
        void record_param(const std::string& name) {
            if (p.name_scopes_.empty()) return;
            p.name_scopes_.back().declared_here.insert(NamePool::intern(name));
        }
        BodyScopeInfo take() const {
            BodyScopeInfo info;
            const NameScope& mine = p.name_scopes_.back();
            info.captured = FrozenIds(mine.captured);
            info.all_names = FrozenIds(mine.all);
            info.free_names = FrozenIds(free_names);
            info.free_valid = free_valid;
            info.free_unknown = free_unknown;
            info.free_saw_eval = free_saw_eval;
            info.free_saw_class = free_saw_class;
            info.method_super_valid = method_super_valid;
            info.method_references_super = method_references_super;
            info.eval_anywhere = mine.all.count(NamePool::intern("eval")) != 0;
            // Folds up through every nested scope (arrow or not) the same way
            // all_names does, so it sees a `super` however deep an arrow
            // chain carries it -- unlike captures_outer, this needs no
            // separate per-form opt-in: whatever named `super` at all is
            // already in `mine.all` before this reads it.
            info.super_anywhere = mine.all.count(NamePool::intern("super")) != 0;
            info.eval_in_nested = mine.eval_in_nested;
            info.class_expression = mine.class_expression;
            info.body_end = body_end;
            info.body_strict = body_strict;
            info.captures_outer = captures_outer;
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
            mine.fold_into(p.name_scopes_.back());
        }
    };
    bool private_name_declared(const std::string& name) const;
    bool release_ok() const;
    // Files, with a function literal's record, what summarize_free_names says of
    // its body. Has to run while the body is still in hand.
    void record_free_summary(FunctionNames& names, const std::vector<std::unique_ptr<Parameter>>& params,
                             const ASTNode* body, bool is_arrow);
    // Opens a plain lexical block's own scope: a `{}` BlockStatement, a
    // catch clause's parameter list, a switch body, or a for(let/const)
    // head -- anything that can hold its own let/const/class/catch-param
    // declarations distinct from whatever encloses it, but is not itself a
    // closure boundary and never gets its own BodyScopeInfo (nothing
    // downstream ever asks a block for one -- see NameScope::fold_into for
    // why a block still relays an already-captured name onward instead of
    // silently dropping it). Closes exactly the way FunctionNames does,
    // minus the take()/record_scope_info step.
    struct LexicalBlockScope {
        Parser& p;
        explicit LexicalBlockScope(Parser& parser) : p(parser) {
            p.name_scopes_.emplace_back();
        }
        LexicalBlockScope(const LexicalBlockScope&) = delete;
        LexicalBlockScope& operator=(const LexicalBlockScope&) = delete;
        ~LexicalBlockScope() {
            NameScope mine = std::move(p.name_scopes_.back());
            p.name_scopes_.pop_back();
            if (p.name_scopes_.empty()) return;
            mine.fold_into(p.name_scopes_.back());
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
    // Tries the narrow tape path via try_tape_assignment first, restoring
    // current_token_index_ and falling back to the real, unmodified
    // parse_expression() on any bail -- used at parse_expression_statement's
    // own call site. parse_assignment_maybe_tape is the same idea for a
    // position that binds one AssignmentExpression rather than a full
    // Expression (VariableDeclarator's initializer); both share
    // last_consumed_token_end for computing the tape's own end position.
    std::unique_ptr<ASTNode> parse_expression_maybe_tape();
    std::unique_ptr<ASTNode> parse_assignment_maybe_tape();
    Position last_consumed_token_end(const Position& fallback) const;
    // One buffer the outermost tape attempt builds into: a bail then costs no
    // allocation, and a successful tape is copied out at exactly its size
    // instead of carrying a growth vector's spare capacity. An attempt that
    // starts while another is running (inside a function the first one is
    // parsing) uses a buffer of its own.
    ExprTape tape_scratch_;
    bool tape_scratch_in_use_ = false;

    // A function, arrow or class parsed while a tape attempt is running,
    // waiting to be embedded in the tape or, if the attempt gives up, to be
    // handed back to the real parse of the same tokens.
    // Which grammar production read the node. A cached node is only handed
    // back to the same production at the same token: the real parse reading a
    // `function` keyword as part of a longer call chain must not be given the
    // bare function.
    enum class CacheKind : uint8_t { Function, Arrow, Primary, Chain };
    struct TapeEmbedded {
        std::unique_ptr<ASTNode> node;
        size_t start_token;
        size_t end_token;
        CacheKind kind;
    };
    struct TapeAttempt {
        std::vector<TapeEmbedded> embedded;
        // Where the tokens read since the last embedded node (or since the
        // attempt began) start, and the ranges already set aside: the token
        // stream lets go of what the parser has moved far past, and a bailed
        // attempt is re-read from its start, so everything but the embedded
        // bodies themselves is held until the attempt is settled.
        size_t gap_start = 0;
        std::vector<std::pair<size_t, size_t>> pinned;
    };
    TapeAttempt* tape_attempt_ = nullptr;
    // What an attempt that gave up parsed, keyed by the token the node
    // starts at and the production that read it. The real parse takes a node
    // from here instead of reading the same source twice.
    struct CachedNode {
        std::unique_ptr<ASTNode> node;
        size_t end_token;
    };
    std::unordered_map<size_t, CachedNode> tape_node_cache_;
    // A function costs nothing to read, so an attempt may go on for as long
    // as the source between embedded nodes stays this short (tokens); a
    // longer run of plain expression would have to be re-read after a bail
    // from further back than the token stream keeps.
    static constexpr size_t kTapeGapBudget = 4096;
    bool tape_gap_exhausted() const;
    static size_t tape_cache_key(size_t token, CacheKind kind) { return token * 4 + static_cast<size_t>(kind); }
    bool try_tape_embed(ExprTape& tape, std::unique_ptr<ASTNode> (Parser::*parse)(), CacheKind kind);
    std::unique_ptr<ASTNode> take_cached_node(CacheKind kind);
    bool try_tape_call_or_member_inner(ExprTape& tape);
    bool tape_chain_can_start(TokenType type) const;
    std::unique_ptr<ASTNode> parse_tape_or_tree(bool sequence, bool for_init = false);
    std::unique_ptr<ASTNode> parse_for_init_maybe_tape();
    // A tape rebuilt as the tree the real parser would have made for the same
    // source, for the places that read a parsed expression back as something
    // else (an assignment pattern reads its elements as targets and defaults).
    // Consumes the tape's embedded nodes.
    static std::unique_ptr<ASTNode> materialize_tape(TapedExpression& tape);
    // Rebuilds, in place, every tape among the elements and values of an
    // array or object literal about to be read as a pattern (nested
    // literals and rest elements included).
    static void untape_pattern(ASTNode* node);

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

    // Phase 1a: narrow, self-contained mirrors of the spine above that
    // append to `tape` instead of building ASTNodes, covering exactly what
    // BytecodeCompiler::compile_tape_expr's 7 tags accept. Each returns
    // false the instant it sees anything outside that narrow set,
    // touching only `tape`/`current_token_index_` (via advance()) so the
    // caller can restore current_token_index_ and fall back to the real,
    // unmodified spine with zero observable difference. They must NEVER call
    // add_error() or the real parse_identifier/parse_number_literal/etc --
    // those have side effects (note_name, subtree_acc_, the error list) a
    // discarded attempt must not leave behind.
    // Whether `name` (an already-lexed TokenType::IDENTIFIER's text) can be
    // treated as a plain identifier reference at all in the current
    // context -- shared by try_tape_primary (a value read) and
    // try_tape_assignment's own IDENTIFIER-'='-fast-path (an assignment
    // target), since parse_assignment_expression's real LHS reaches this
    // exact same parse_primary_expression/parse_identifier validation for a
    // bare identifier target too (Parser.cpp:1904-1912, :2925-2932). Does
    // NOT check has_escaped_keyword() -- callers check that themselves
    // against the actual Token, since this takes just the decoded name.
    bool try_tape_identifier_ref_ok(const std::string& name) const;
    bool try_tape_primary(ExprTape& tape);
    bool try_tape_call_or_member(ExprTape& tape);
    // Mirrors parse_unary_expression's own recursive structure
    // (Parser.cpp:1138-1250) for exactly the six prefix operators
    // compile_tape_expr's Unary case supports (+, -, !, ~, typeof, void).
    // delete and prefix ++/-- fall through to try_tape_call_or_member
    // (this function's own "no operator here" branch) and bail there
    // exactly like await/any other keyword token would -- try_tape_primary
    // only ever matches NUMBER/STRING/IDENTIFIER, so none of them are ever
    // mistaken for a primary.
    bool try_tape_unary(ExprTape& tape);
    bool try_tape_binary(ExprTape& tape, int min_precedence);
    // Mirrors parse_nullish_coalescing_expression's own loop on `??`, one
    // grammar level above try_tape_binary and one below try_tape_logical_or
    // -- see this function's own definition comment (Parser.cpp) for the
    // unparenthesized-mixing restriction it enforces against `&&`.
    bool try_tape_nullish(ExprTape& tape);
    // Mirrors parse_logical_or_expression's own loop on `||`, one grammar
    // level above try_tape_nullish -- see that function's own comment for
    // the mixing restriction this one enforces in the other direction,
    // against `??`.
    bool try_tape_logical_or(ExprTape& tape);
    // Mirrors parse_conditional_expression's own structure, one grammar
    // level above try_tape_logical_or -- the `?`'s consequent and
    // alternate each mutually recurse into try_tape_assignment, not into
    // this function, matching the real function calling parse_
    // assignment_expression() twice rather than itself.
    bool try_tape_conditional(ExprTape& tape);
    bool try_tape_assignment(ExprTape& tape);
    bool try_tape_expression(ExprTape& tape);
    // Whether the subtree at `index` is something `++`/`--` may write to: a
    // member, or an identifier other than `this` (and, in strict mode, `eval`
    // / `arguments`).
    bool tape_update_target_ok(const ExprTape& tape, size_t index) const;
    // The AssignmentExpression operator for an assignment token, false for
    // `**=` (which the real parser rewrites into `a = a ** b`).
    static bool tape_assignment_operator(TokenType type, AssignmentExpression::Operator& op);

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
    // The six words ES5 reserves for strict mode. They are only reserved where
    // a name is bound or read -- `{ public: 1 }` and `x.private` are ordinary
    // strict code -- so the answer belongs to whoever knows the position, not
    // to the lexer.
    static bool is_strict_reserved_name(std::string_view name);
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
    // The same, for a concise arrow: what sits at the range is one expression,
    // and the value it comes to is what the arrow answers.
    std::unique_ptr<ASTNode> parse_concise_body_at(size_t tok_index, bool strict,
                                                   bool is_generator, bool is_async);
    // A single expression at a token range, no enclosing function context at
    // all -- unlike parse_concise_body_at (an arrow's concise body, which
    // hardcodes class/constructor context that would wrongly legalize
    // super()/super.x here), this is for TapedExpression's own compile-time
    // fallback: reparsing a tape's source range as a plain tree when
    // compile_tape_expr turns out not to support it. Always safe to call with
    // no context, because a tape-eligible range can never contain anything
    // (closure/await/yield/super/private field) that would have needed any.
    std::unique_ptr<ASTNode> parse_expression_at(size_t tok_index, bool strict);
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

    // Whether this token names a binding where the code is being read. Some
    // words the lexer gives their own type are still names, and some of those
    // stop being names only in a particular mode, so the answer needs the
    // options as well as the token.
    bool token_names_a_binding(TokenType type) const;
    // How such a token is spelled. A keyword token is known to the lexer by
    // its type, so the spelling has to be written out rather than read back
    // out of the source, where an escape would have left it unrecognisable.
    std::string_view binding_name_text(const Token& token) const;

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
