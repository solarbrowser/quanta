#pragma once

#include <memory>
#include <string>

#include "quanta/lexer/Token.h"

#include "quanta/parser/FunctionExecutable.h"

namespace Quanta {

class ASTNode;

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
    const std::string& source() const { return source_; }

    // The token stream the tree was parsed from, kept so a function body can
    // be parsed later from the tokens it already occupies instead of being
    // held as a tree the whole time. Cheap next to what it replaces: for a
    // 3MB script the tokens come to single-digit megabytes.
    const TokenSequence& tokens() const { return tokens_; }
    void set_tokens(TokenSequence t) { tokens_ = std::move(t); }
    void set_source(std::string src) { source_ = std::move(src); }
    std::string source_range(uint32_t start, uint32_t end) const {
        if (start >= source_.size() || end <= start) return std::string();
        if (end > source_.size()) end = static_cast<uint32_t>(source_.size());
        return source_.substr(start, end - start);
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

private:
    ScriptUnit() = default;
    ~ScriptUnit();

    std::unique_ptr<ASTNode> root_;
    std::string source_;
    TokenSequence tokens_;
    mutable uint32_t ref_count_ = 0;

    static thread_local ScriptUnit* building_;
};

using ScriptUnitRef = ExecutableRef<ScriptUnit>;

}  // namespace Quanta
