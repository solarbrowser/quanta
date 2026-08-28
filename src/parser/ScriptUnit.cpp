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
                                                            bool is_generator, bool is_async) {
    if (!source_ || end_offset <= start.offset || end_offset > source_->size()) return nullptr;
    Lexer::LexerOptions opts;
    opts.strict_mode = strict;
    Lexer lexer(source_, opts);
    TokenSequence toks = lexer.tokenize_range(start, end_offset);
    if (toks.size() <= 1) return nullptr;
    Parser parser(std::move(toks));
    parser.set_source(*source_);
    BuildScope scope(this);
    return parser.parse_body_at(0, strict, is_generator, is_async);
}


}  // namespace Quanta
