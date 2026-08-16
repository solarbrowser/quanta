#include "quanta/parser/ScriptUnit.h"
#include "quanta/parser/Parser.h"

#include "quanta/parser/AST.h"

namespace Quanta {

thread_local ScriptUnit* ScriptUnit::building_ = nullptr;

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

std::unique_ptr<ASTNode> ScriptUnit::parse_body_at(uint32_t tok_first, bool strict,
                                                   bool is_generator, bool is_async) {
    // Parser takes its TokenSequence by value, so one per body would copy the
    // whole stream every time -- built once here and reused.
    // MOVED, not copied: Parser takes its TokenSequence by value, and for a
    // bundle the stream runs to tens of megabytes -- a second copy costs more
    // than every body it would let us drop. The unit reads the stream back
    // through the parser from here on, which is why can_reparse_bodies() also
    // accepts a parser that already holds it.
    if (!body_parser_) body_parser_ = std::make_unique<Parser>(std::move(tokens_));
    // Stamped with this unit, exactly as the original parse was.
    BuildScope scope(this);
    return body_parser_->parse_body_at(tok_first, strict, is_generator, is_async);
}

}  // namespace Quanta
