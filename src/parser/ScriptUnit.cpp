#include "quanta/parser/ScriptUnit.h"

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

}  // namespace Quanta
