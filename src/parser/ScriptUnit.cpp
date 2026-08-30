#include "quanta/parser/ScriptUnit.h"
#include "quanta/parser/Parser.h"
#include "quanta/lexer/Lexer.h"

#include "quanta/parser/AST.h"

namespace Quanta {

constinit thread_local ScriptUnit* ScriptUnit::building_ = nullptr;

ScriptUnit::~ScriptUnit() = default;

ExecutableRef<ScriptUnit> ScriptUnit::create(std::unique_ptr<ASTNode> root) {
    auto* unit = new ScriptUnit();
    ExecutableRef<ScriptUnit> ref(unit);
    unit->root_ = std::move(root);
    return ref;
}

void ScriptUnit::set_root(std::unique_ptr<ASTNode> root) {
    root_ = std::move(root);
}

std::unique_ptr<ASTNode> ScriptUnit::parse_body_from_source(const Position& start,
                                                            uint32_t end_offset, bool strict,
                                                            bool is_generator, bool is_async,
                                                            bool concise) {
    if (!source_ || end_offset <= start.offset || end_offset > source_->size()) return nullptr;
    Lexer::LexerOptions opts;
    opts.strict_mode = strict;
    opts.reparsing_accepted_source = true;
    // Pumped as the parse asks for it rather than laid out first: a body read
    // back can be most of a file, and the whole token run was peak memory paid
    // to hand back a tree.
    TokenSequence toks = Lexer::stream_range(source_, start, end_offset, opts);
    if (toks.lexed_count() <= 1 && toks.size() != SIZE_MAX) return nullptr;
    Parser parser(std::move(toks));
    parser.set_source(source_);
    BuildScope scope(this);
    return concise ? parser.parse_concise_body_at(0, strict, is_generator, is_async)
                   : parser.parse_body_at(0, strict, is_generator, is_async);
}


}  // namespace Quanta
