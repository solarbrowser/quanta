#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "quanta/lexer/Token.h"

#include "quanta/parser/FunctionExecutable.h"

namespace Quanta {

class ASTNode;
class Parser;

// Owns one parse tree and keeps it alive for exactly as long as anything still
// points into it.
//
// A FunctionExecutable used to hold a private clone of its body because parse
// trees are locals: the Program tree, and every eval/new Function() parse, is
// destroyed the moment the call that ran it returns, while an executable can
// outlive that call inside an escaping closure. Cloning bought that safety by
// duplicating the tree -- and duplicating it per nesting level, since an inner
// function's body is copied again inside every ancestor's copy.
//
// A unit buys the same safety by reference instead: the executable holds a
// ScriptUnitRef, so the tree outlives it by construction and nothing is copied.
//
// Refcounted through ExecutableRef (the counter is deliberately non-atomic --
// see that template's comment) rather than shared_ptr: instantiating a closure
// copies the executable's refs on a hot path, and this build is -pthread, so a
// shared_ptr there would be two atomic read-modify-writes per closure.
// What one function body's parse learned about the names inside it.
struct BodyScopeInfo {
    // Every identifier that appears inside a function nested in this body, at
    // any depth. Such a name is reachable from a closure, so it has to stay in
    // the environment rather than take a register. Collecting a name that no
    // closure actually reads only costs it a register; missing one that a
    // closure does read would be wrong.
    std::unordered_set<std::string> captured;
    // `eval` named anywhere in the body, nested or not: its text can reach any
    // binding here, so nothing may take a register.
    bool eval_anywhere = false;
    // `super` named anywhere in the body, which needs the environment bindings
    // that carry it.
    bool super_anywhere = false;
    // `eval` named inside something nested, which is the narrower question the
    // selective scan asks.
    bool eval_in_nested = false;
    // A class expression anywhere in the body. Its own body is a scope this
    // analysis does not enter, so the whole function falls back to keeping
    // every local in the environment rather than guessing.
    bool class_expression = false;
};

class ScriptUnit {
public:
    static ExecutableRef<ScriptUnit> create(std::unique_ptr<ASTNode> root);

    void ref() const { ++ref_count_; }
    void unref() const { if (--ref_count_ == 0) delete this; }

    ASTNode* root() const { return root_.get(); }

    // The text the tree was parsed from. Function literals record a range into
    // it rather than each carrying its own copy of their own source: a nested
    // function's text is contained in every ancestor's, so copying meant the
    // same bytes stored once per nesting level.
    const std::string& source() const { return source_ ? *source_ : empty_source(); }
    // The buffer itself, for a re-lex that must not copy it.
    const std::shared_ptr<const std::string>& source_ref() const { return source_; }

    // Whether a body can be rebuilt out of this unit. The source is what it is
    // rebuilt from, so a unit without one -- there is no path that makes a
    // deferred body without also recording the text it came from, but the
    // check costs nothing -- must keep its trees.
    bool can_reparse_bodies() const { return source_ && !source_->empty(); }
    void set_source(std::string src) { source_ = std::make_shared<const std::string>(std::move(src)); }
    // The buffer as it already is, for a caller that holds the same one.
    void set_source(std::shared_ptr<const std::string> src) { source_ = std::move(src); }
    std::string source_range(uint32_t start, uint32_t end) const {
        const std::string& s = source();
        if (start >= s.size() || end <= start) return std::string();
        if (end > s.size()) end = static_cast<uint32_t>(s.size());
        return s.substr(start, end - start);
    }
    // Only used to hand the tree over once the parse that BuildScope wrapped
    // has finished -- the unit must already exist for the stamping to work.
    void set_root(std::unique_ptr<ASTNode> root);

    // Open around parsing (or synthesising) a tree so the function-literal
    // nodes built inside it record which unit owns them. Stamping happens at
    // BUILD time, never at call time: a generator or async body resumes on a
    // separate execution path, so a "currently running unit" read at call time
    // would hand a literal the resumer's unit instead of its own.
    class BuildScope {
    public:
        explicit BuildScope(ScriptUnit* unit) : previous_(building_) { building_ = unit; }
        ~BuildScope() { building_ = previous_; }
        BuildScope(const BuildScope&) = delete;
        BuildScope& operator=(const BuildScope&) = delete;

    private:
        ScriptUnit* previous_;
    };

    // The unit being built right now, or null when a tree is built outside any
    // unit -- which is every path that has not been converted yet, and which
    // keeps taking its own clone.
    static ScriptUnit* building() { return building_; }

    // Re-parses one function body out of this unit's own SOURCE. Backs
    // FunctionExecutable's deferred bodies: a leaf body is dropped once its
    // analyses are cached and lexed back here the first time anything needs
    // the tree. Positions stay absolute -- the lexer is pointed at the same
    // buffer the whole script was parsed from and told where to stop -- so the
    // rebuilt tree reports the same lines the original did.
    std::unique_ptr<ASTNode> parse_body_from_source(const Position& start, uint32_t end_offset,
                                                    bool strict, bool is_generator, bool is_async);

    // Where a function literal's executable is remembered. It used to live on
    // the literal's own node, which made the node's ADDRESS the key -- and a
    // body parsed back from the tokens comes back as different nodes, so the
    // executable was lost and a second one built for the same declaration.
    // The token the body opens at says the same thing and survives a rebuild,
    // so that is the key. A literal with no token range of its own (an arrow
    // with an expression body, a synthesized constructor) keeps its node-local
    // copy; there are a handful of those against thousands of these.
    // What the parser worked out about one function body while it was reading
    // it: the names anything nested inside it can see, which is what decides
    // whether a local may live in a register. Keyed the same way an executable
    // is, on where the body opens in the source, so a body parsed back later
    // finds the same entry.
    const BodyScopeInfo* scope_info_at(uint32_t body_src) const {
        auto it = body_scopes_.find(body_src);
        return it == body_scopes_.end() ? nullptr : &it->second;
    }
    void set_scope_info_at(uint32_t body_src, BodyScopeInfo info) {
        body_scopes_[body_src] = std::move(info);
    }

    const ExecutableRef<FunctionExecutable>& executable_at(uint32_t body_tok) const {
        static const ExecutableRef<FunctionExecutable> kNone;
        auto it = executables_.find(body_tok);
        return it == executables_.end() ? kNone : it->second;
    }
    void set_executable_at(uint32_t body_tok, ExecutableRef<FunctionExecutable> exe) {
        executables_[body_tok] = std::move(exe);
    }
    // A class site has two: the class's own and the constructor it builds.
    const ExecutableRef<FunctionExecutable>& ctor_executable_at(uint32_t body_tok) const {
        static const ExecutableRef<FunctionExecutable> kNone;
        auto it = ctor_executables_.find(body_tok);
        return it == ctor_executables_.end() ? kNone : it->second;
    }
    void set_ctor_executable_at(uint32_t body_tok, ExecutableRef<FunctionExecutable> exe) {
        ctor_executables_[body_tok] = std::move(exe);
    }

private:
    ScriptUnit() = default;
    ~ScriptUnit();

    std::unique_ptr<ASTNode> root_;
    std::shared_ptr<const std::string> source_;
    static const std::string& empty_source() { static const std::string e; return e; }
    // Built on the first deferred body this unit is asked for, then reused.
    // See executable_at.
    std::unordered_map<uint32_t, ExecutableRef<FunctionExecutable>> executables_;
    // See scope_info_at.
    std::unordered_map<uint32_t, BodyScopeInfo> body_scopes_;
    std::unordered_map<uint32_t, ExecutableRef<FunctionExecutable>> ctor_executables_;
    mutable uint32_t ref_count_ = 0;

    static constinit thread_local ScriptUnit* building_;
};

using ScriptUnitRef = ExecutableRef<ScriptUnit>;

}  // namespace Quanta
