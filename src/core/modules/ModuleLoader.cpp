/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/modules/ModuleLoader.h"
#include "quanta/core/engine/Engine.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/gc/Visitor.h"
#include "quanta/parser/Parser.h"
#include "quanta/parser/AST.h"
#include "quanta/lexer/Lexer.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/Error.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/core/runtime/Promise.h"
#include "quanta/core/runtime/Async.h"
#include "quanta/core/vm/BytecodeCompiler.h"
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <iostream>


namespace Quanta {

// ModuleNamespaceObject now lives in ModuleLoader.h (CustomObjectBase's own
// switch dispatch needs to name it directly -- see its doc comment there).

Module::Module(const std::string& id, const std::string& filename)
    : id_(id), filename_(filename), loaded_(false), loading_(false) {
}

void Module::declare_export(const std::string& name, const std::string& local_name) {
    // Records that the name is exported without claiming a value for it yet:
    // an insert only, so a later add_export with the real value still wins.
    exports_.emplace(name, Value());
    if (!local_name.empty()) export_local_names_.emplace(name, local_name);
}

void Module::add_export(const std::string& name, const Value& value, const std::string& local_name) {
    exports_[name] = value;
    if (!local_name.empty()) {
        export_local_names_[name] = local_name;
    }
}

namespace {
// A module's own scope ends where the realm's globals begin. The module
// environment chains into the global one so the module sees the realm's
// intrinsics, and that must not make a plain global variable answer for one of
// the module's own bindings -- an export it never declared would resolve, and a
// link error would go unreported.
Environment* module_scope_owner(Context* mc, const std::string& name) {
    if (!mc) return nullptr;
    Environment* realm_globals = nullptr;
    if (Engine* e = mc->get_engine()) {
        if (Context* g = e->get_global_context()) realm_globals = g->get_variable_environment();
    }
    for (Environment* env = mc->get_lexical_environment(); env && env != realm_globals;
         env = env->get_outer()) {
        if (env->has_own_binding(name)) return env;
    }
    return nullptr;
}

// An export can be an alias for another module's export -- `export { x } from`
// records the link, not the value, so a name that only resolves once the whole
// cycle is loaded still resolves. Reading one follows the chain.
Value unwrap_import_binding(const Value& v) {
    Object* o = v.as_object_or_null();
    if (!o || o->get_type() != Object::ObjectType::Custom) return v;
    auto* custom = static_cast<CustomObjectBase*>(o);
    if (custom->get_custom_kind() != CustomObjectBase::CustomKind::ImportBinding) return v;
    return static_cast<const ImportBindingObject*>(o)->resolve();
}
}  // namespace

Value Module::get_export(const std::string& name) const {
    // Live binding: if this export is a direct alias for a module-scope binding
    // (the common case -- `export var x`, `export { x }`, `export default function fn(){}`),
    // read the binding's CURRENT value so later reassignments are observable
    // through the module namespace, per ES module live-binding semantics.
    Resolution r = resolve_export(name);
    if (!r) return Value();
    if (r.name == "*") {
        return ModuleLoader::build_module_namespace(const_cast<Module*>(r.module));
    }
    if (r.module != this) return r.module->get_export(r.name);

    auto local_it = export_local_names_.find(r.name);
    if (local_it != export_local_names_.end() && module_context_ &&
        module_scope_owner(module_context_.get(), local_it->second)) {
        return module_context_->get_binding(local_it->second);
    }

    auto it = exports_.find(r.name);
    if (it != exports_.end()) {
        return unwrap_import_binding(it->second);
    }
    return Value();
}

Module::~Module() = default;

void Module::set_program(std::unique_ptr<ASTNode> program) {
    program_ = std::move(program);
}

void Module::add_star_source(Module* source) {
    if (!source || source == this) return;
    for (Module* m : star_sources_) if (m == source) return;
    star_sources_.push_back(source);
}

void Module::add_async_parent(Module* parent) {
    if (!parent) return;
    for (Module* m : async_parents_) if (m == parent) return;
    async_parents_.push_back(parent);
}

void Module::add_deferred_request(Module* dep) {
    if (!dep) return;
    for (Module* m : deferred_requests_) if (m == dep) return;
    deferred_requests_.push_back(dep);
}

void Module::add_requested(Module* dep) {
    if (!dep) return;
    for (Module* m : requested_) if (m == dep) return;
    requested_.push_back(dep);
}

void Module::add_cycle_member(Module* member) {
    if (!member || member == this) return;
    for (Module* m : cycle_members_) if (m == member) return;
    cycle_members_.push_back(member);
}

void Module::declare_indirect_export(const std::string& export_name, Module* source,
                                     const std::string& import_name) {
    if (!source) return;
    indirect_exports_[export_name] = {source, import_name};
}

bool Module::has_own_export(const std::string& name) const {
    return exports_.find(name) != exports_.end() &&
           indirect_exports_.find(name) == indirect_exports_.end();
}

namespace {
using ResolveSet = std::vector<std::pair<const Module*, std::string>>;

// ResolveExport. A module reached again for the same name on one walk is a
// cycle: it resolves to nothing rather than to itself, which is what lets a
// sibling star source answer instead.
Module::Resolution resolve_export_impl(const Module* module, const std::string& name,
                                       ResolveSet& seen) {
    Module::Resolution none;
    if (!module) return none;
    for (const auto& e : seen) if (e.first == module && e.second == name) return none;
    seen.emplace_back(module, name);

    if (const auto* link = module->indirect_export(name)) {
        if (link->second == "*") {
            // The source's namespace, which every path to it shares: two
            // re-exports of the same module's namespace agree.
            Module::Resolution r;
            r.module = link->first;
            r.name = "*";
            return r;
        }
        return resolve_export_impl(link->first, link->second, seen);
    }
    if (module->has_own_export(name)) {
        Module::Resolution r;
        r.module = module;
        r.name = name;
        return r;
    }
    if (name == "default") return none;  // `export *` never carries default

    Module::Resolution found;
    for (Module* src : module->star_sources()) {
        Module::Resolution r = resolve_export_impl(src, name, seen);
        if (r.ambiguous) return r;
        if (!r) continue;
        if (!found) {
            found = r;
        } else if (found.module != r.module || found.name != r.name) {
            Module::Resolution amb;
            amb.ambiguous = true;
            return amb;
        }
    }
    return found;
}
}  // namespace

Module::Resolution Module::resolve_export(const std::string& name) const {
    ResolveSet seen;
    return resolve_export_impl(this, name, seen);
}

bool Module::export_is_uninitialized(const std::string& name) const {
    Resolution r = resolve_export(name);
    if (!r) return false;
    const Module* owner = r.module;
    Context* mc = owner->get_context();
    if (!mc) return false;
    auto local_it = owner->export_local_names_.find(r.name);
    if (local_it == owner->export_local_names_.end()) return false;
    const std::string& local = local_it->second;
    Environment* env = module_scope_owner(mc, local);
    if (!env) return true;  // declared as an export, no binding yet
    // Only a declarative slot can be uninitialized. An object environment holds
    // the binding as a property, and a property that is there has a value.
    if (env->get_type() == Environment::Type::Object) return false;
    return !env->is_initialized_binding(local);
}

bool Module::has_export(const std::string& name) const {
    Resolution r = resolve_export(name);
    return r.module != nullptr || r.ambiguous;
}

// The same walk has_export does, collecting instead of answering.
static void collect_export_names_through_stars(const Module* module,
                                               std::vector<const Module*>& seen,
                                               std::vector<std::string>& out, bool with_default) {
    for (const Module* m : seen) if (m == module) return;
    seen.push_back(module);
    for (const auto& n : module->own_export_names()) {
        if (!with_default && n == "default") continue;
        if (std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
    }
    for (Module* src : module->star_sources()) {
        collect_export_names_through_stars(src, seen, out, /*with_default=*/false);
    }
}

std::vector<std::string> Module::own_export_names() const {
    std::vector<std::string> names;
    names.reserve(exports_.size());
    for (const auto& pair : exports_) names.push_back(pair.first);
    return names;
}

std::vector<std::string> Module::get_export_names() const {
    std::vector<std::string> names;
    std::vector<const Module*> seen;
    collect_export_names_through_stars(this, seen, names, /*with_default=*/true);
    // A name two star sources disagree about is ambiguous, and an ambiguous
    // name is not an export at all: it is absent from the namespace, and only
    // an import that asks for it by name is an error.
    names.erase(std::remove_if(names.begin(), names.end(),
                               [this](const std::string& n) {
                                   return resolve_export(n).ambiguous;
                               }),
                names.end());
    return names;
}

void Module::set_context(std::unique_ptr<Context> context) {
    module_context_ = std::move(context);
}

void Module::gc_trace(Visitor& v) const {
    for (const auto& kv : exports_) v.visit(kv.second);
    v.visit(evaluation_promise_);
    v.visit(completion_);
    v.visit(thrown_exception_);
    v.visit(namespace_);
    v.visit(deferred_namespace_);
}

ModuleLoader::ModuleLoader(Engine* engine) : engine_(engine) {
    add_search_path("./");
    add_search_path("./node_modules/");
}

void ModuleLoader::gc_trace(Visitor& v) const {
    for (const auto& kv : modules_) kv.second->gc_trace(v);
    v.visit(last_module_exception_);
}

Module* ModuleLoader::prepare_module(const std::string& module_id, const std::string& from_path) {
    last_module_exception_ = Value();
    std::string resolved_path = resolve_module_path(module_id, from_path);
    std::string normalized_id = normalize_module_id(module_id, from_path);

    auto it = modules_.find(normalized_id);
    if (it != modules_.end()) return it->second.get();

    auto module = create_module(normalized_id, resolved_path);
    if (!module) return nullptr;

    Module* module_ptr = module.get();
    modules_[normalized_id] = std::move(module);
    module_ptr->set_loading(true);

    preparing_.push_back(module_ptr);
    bool prepared = prepare_module_file(module_ptr, resolved_path);
    preparing_.pop_back();

    module_ptr->set_loading(false);
    if (!prepared) {
        modules_.erase(normalized_id);
        return nullptr;
    }
    module_ptr->set_loaded(true);
    return module_ptr;
}

namespace {
bool subgraph_has_tla(Module* module, std::vector<Module*>& seen) {
    if (!module) return false;
    for (Module* m : seen) if (m == module) return false;
    seen.push_back(module);
    if (module->has_top_level_await()) return true;
    for (Module* dep : module->requested()) {
        if (subgraph_has_tla(dep, seen)) return true;
    }
    for (Module* dep : module->deferred_requests()) {
        if (subgraph_has_tla(dep, seen)) return true;
    }
    return false;
}
}  // namespace

namespace {
bool subgraph_evaluating(Module* module, std::vector<Module*>& seen) {
    if (!module) return false;
    for (Module* m : seen) if (m == module) return false;
    seen.push_back(module);
    if (module->evaluation_in_progress()) return true;
    for (Module* dep : module->requested()) {
        if (subgraph_evaluating(dep, seen)) return true;
    }
    for (Module* dep : module->deferred_requests()) {
        if (subgraph_evaluating(dep, seen)) return true;
    }
    return false;
}
}  // namespace

void ModuleLoader::evaluate_module_graph(Module* module) {
    if (!module || module->is_evaluated() || module->has_thrown_exception()) return;
    if (!link_graph(module)) return;
    evaluate_graph(module);
}

void ModuleLoader::collect_async_roots(Module* module, std::vector<Module*>& out) {
    if (!module) return;
    auto visit = [&](Module* child) {
        if (!child || !subgraph_has_top_level_await(child)) return;
        if (child->has_top_level_await()) {
            for (Module* m : out) if (m == child) return;
            out.push_back(child);
            return;
        }
        // Sync itself, so it stays deferred; only what is async under it runs.
        collect_async_roots(child, out);
    };
    for (Module* child : module->requested()) visit(child);
    for (Module* child : module->deferred_requests()) visit(child);
}

bool ModuleLoader::subgraph_evaluation_in_progress(Module* module) {
    std::vector<Module*> seen;
    return subgraph_evaluating(module, seen);
}

bool ModuleLoader::subgraph_has_top_level_await(Module* module) {
    std::vector<Module*> seen;
    return subgraph_has_tla(module, seen);
}

Module* ModuleLoader::load_module(const std::string& module_id, const std::string& from_path) {
    // Preparing reaches every module in the graph; only the request that
    // started it links and evaluates what it found. A request made while
    // preparing is one of those dependencies and is left to that walk. A
    // request made later -- reaching through a deferred namespace, a dynamic
    // import -- is a graph of its own, and one already evaluated is done.
    const bool during_prepare = !preparing_.empty();

    Module* module = prepare_module(module_id, from_path);
    if (!module) return nullptr;
    if (during_prepare) return module;

    if (module->is_evaluated() || module->has_thrown_exception()) return module;
    if (!link_graph(module)) return module;
    evaluate_graph(module);
    return module;
}

namespace {
void collect_graph(Module* module, std::vector<Module*>& out) {
    if (!module) return;
    for (Module* m : out) if (m == module) return;
    out.push_back(module);
    for (Module* dep : module->requested()) collect_graph(dep, out);
    for (Module* dep : module->deferred_requests()) collect_graph(dep, out);
}
}  // namespace

// Every name an import asks for has to resolve before any of the graph runs.
// A name the exporting module does not provide, or one two `export *` sources
// disagree about, is a SyntaxError raised here -- not something the importer
// discovers halfway through its own body.
bool ModuleLoader::link_graph(Module* root) {
    std::vector<Module*> graph;
    collect_graph(root, graph);
    for (Module* module : graph) {
        const Program* program = static_cast<const Program*>(module->program());
        if (!program) continue;
        for (const auto& stmt : program->get_statements()) {
            if (!stmt) continue;
            std::string request;
            std::vector<std::pair<std::string, bool>> wanted;  // name, is_namespace
            if (stmt->get_type() == ASTNode::Type::IMPORT_STATEMENT) {
                const auto* im = static_cast<const ImportStatement*>(stmt.get());
                if (im->is_deferred()) continue;
                request = im->get_module_source();
                if (im->is_default_import() && !im->get_default_alias().empty()) {
                    wanted.emplace_back("default", false);
                }
                for (const auto& spec : im->get_specifiers()) {
                    wanted.emplace_back(spec->get_imported_name(), false);
                }
            } else if (stmt->get_type() == ASTNode::Type::EXPORT_STATEMENT) {
                const auto* ex = static_cast<const ExportStatement*>(stmt.get());
                request = ex->get_source_module();
                if (request.empty() || !ex->is_re_export()) continue;
                for (const auto& spec : ex->get_specifiers()) {
                    const std::string& local = spec->get_local_name();
                    if (local == "*") continue;  // the source's namespace, always resolvable
                    wanted.emplace_back(local, false);
                }
            }
            if (request.empty() || wanted.empty()) continue;
            Module* src = prepare_module(request, module->get_filename());
            if (!src) continue;
            for (const auto& w : wanted) {
                Module::Resolution r = src->resolve_export(w.first);
                if (r && !r.ambiguous) continue;
                auto err = Error::create_syntax_error(
                    r.ambiguous
                        ? "The requested module '" + request +
                              "' contains conflicting star exports for name '" + w.first + "'"
                        : "The requested module '" + request +
                              "' does not provide an export named '" + w.first + "'");
                // The failure belongs to the whole link, not to the module the
                // walk happened to be looking at: what asked for the graph is
                // what has to see the error.
                Value reason(err.release());
                module->set_thrown_exception(reason);
                if (!root->has_thrown_exception()) root->set_thrown_exception(reason);
                last_module_exception_ = reason;
                return false;
            }
        }
    }
    return true;
}

// Dependencies first, each module once. A module whose dependency threw does
// not run, and everything in a cycle reports the one error its root recorded.
void ModuleLoader::evaluate_graph(Module* root) {
    if (!root || root->is_evaluated() || root->has_thrown_exception()) return;
    std::vector<Module*> stack;
    inner_evaluate(root, stack, 0);
}

// InnerModuleEvaluation. The index and the stack are what turn the walk into a
// cycle detector: a module whose ancestor index comes back equal to its own
// index is the root of everything still above it on the stack, and that root is
// what an outsider waits on and what records the error the cycle reports.
int ModuleLoader::inner_evaluate(Module* module, std::vector<Module*>& stack, int index) {
    if (!module) return index;
    if (module->is_evaluated()) return index;   // done, or already on this walk

    module->mark_evaluating();
    module->set_dfs_index(index);
    module->set_dfs_ancestor_index(index);
    ++index;
    stack.push_back(module);

    for (Module* dep : module->requested()) {
        if (!dep) continue;
        index = inner_evaluate(dep, stack, index);
        if (dep->has_thrown_exception()) {
            module->set_thrown_exception(dep->get_thrown_exception());
            return index;
        }
        Module* waited_on = dep;
        if (dep->is_on_eval_stack()) {
            // Still climbing out of the component this module belongs to.
            module->set_dfs_ancestor_index(
                std::min(module->dfs_ancestor_index(), dep->dfs_ancestor_index()));
        } else {
            waited_on = dep->cycle_root();
            if (waited_on->has_thrown_exception()) {
                module->set_thrown_exception(waited_on->get_thrown_exception());
                return index;
            }
        }
        if (waited_on->is_async_evaluating()) {
            module->add_pending_async_dep();
            waited_on->add_async_parent(module);
        }
    }

    if (module->pending_async_deps() > 0 || module->has_top_level_await()) {
        module->set_async_evaluating(true);
        module->set_async_order(next_async_order_++);
        if (module->pending_async_deps() == 0) {
            execute_module_body(module, /*notify_on_success=*/true);
        }
    } else {
        execute_module_body(module, /*notify_on_success=*/true);
    }

    if (module->dfs_ancestor_index() == module->dfs_index()) {
        while (!stack.empty()) {
            Module* member = stack.back();
            stack.pop_back();
            if (member->is_async_evaluating()) member->mark_evaluating_async();
            else member->mark_evaluated();
            member->set_cycle_root(module);
            // The root records the error the whole component reports, so it
            // has to know who else is in it.
            module->add_cycle_member(member);
            if (member == module) break;
        }
    }
    return index;
}

void ModuleLoader::execute_module_body(Module* module, bool notify_on_success) {
    if (!module || module->has_thrown_exception()) return;

    evaluating_.push_back(module);
    evaluate_module(module);
    evaluating_.pop_back();

    if (module->has_thrown_exception()) {
        async_module_finished(module, /*ok=*/false, module->get_thrown_exception());
        return;
    }

    // A body that suspended left a promise for its own completion behind.
    Value completion = module->get_evaluation_promise();
    Promise* pending = AsyncUtils::is_promise(completion)
                           ? static_cast<Promise*>(completion.as_object())
                           : nullptr;
    if (!pending || pending->get_state() != PromiseState::PENDING) {
        // Finished where it stands. A module run from an exec list still
        // settles its own completion, but does not release its ancestors again:
        // the gather that put it there already did.
        async_module_finished(module, /*ok=*/true, Value(), /*wake_parents=*/notify_on_success);
        return;
    }

    module->set_async_evaluating(true);
    if (module->async_order() == 0) module->set_async_order(next_async_order_++);
    ModuleLoader* self = this;
    Module* waiting = module;
    auto on_done = ObjectFactory::create_native_function("",
        [self, waiting](Context&, std::span<const Value>, Value) -> Value {
            self->async_module_finished(waiting, true, Value());
            return Value();
        }, 1);
    auto on_fail = ObjectFactory::create_native_function("",
        [self, waiting](Context&, std::span<const Value> a, Value) -> Value {
            self->async_module_finished(waiting, false, a.empty() ? Value() : a[0]);
            return Value();
        }, 1);
    pending->then(on_done.release(), on_fail.release());
}

// AsyncModuleExecutionFulfilled / Rejected: a module that finishes releases
// everything that was counting on it, and the ones that reach zero run now.
void ModuleLoader::async_module_finished(Module* module, bool ok, const Value& reason,
                                        bool wake_parents) {
    if (!module) return;
    module->set_async_evaluating(false);
    module->mark_evaluated();
    if (!ok && !module->has_thrown_exception()) module->set_thrown_exception(reason);

    // Settled before anything that was waiting on this module runs: whoever
    // asked for the module directly sees it finish first.
    if (Promise* own = AsyncUtils::is_promise(module->completion())
                           ? static_cast<Promise*>(module->completion().as_object())
                           : nullptr) {
        if (own->get_state() == PromiseState::PENDING) {
            if (ok) own->fulfill(Value());
            else own->reject(module->get_thrown_exception());
        }
    }

    if (module->has_thrown_exception()) {
        for (Module* member : module->cycle_members()) {
            if (!member->has_thrown_exception()) {
                member->set_thrown_exception(module->get_thrown_exception());
            }
        }
    }

    if (!ok) {
        // Copied: a parent's own completion can reach back here.
        std::vector<Module*> parents = module->async_parents();
        for (Module* parent : parents) {
            if (!parent || parent->has_thrown_exception()) continue;
            parent->set_thrown_exception(module->get_thrown_exception());
            parent->set_async_evaluating(false);
            async_module_finished(parent, false, module->get_thrown_exception());
        }
        return;
    }

    if (!wake_parents) return;

    // Which ancestors are now free is settled before any of them runs, and
    // they run in the order they became async -- not in the order the walk
    // happens to reach them. A module that suspends again takes its own
    // ancestors with it, which is why the gather stops at one.
    std::vector<Module*> exec_list;
    gather_available_ancestors(module, exec_list);
    std::sort(exec_list.begin(), exec_list.end(), [](Module* a, Module* b) {
        return a->async_order() < b->async_order();
    });
    for (Module* m : exec_list) {
        m->set_async_evaluating(false);
        execute_module_body(m, /*notify_on_success=*/false);
    }
}

void ModuleLoader::gather_available_ancestors(Module* module, std::vector<Module*>& exec_list) {
    for (Module* parent : module->async_parents()) {
        if (!parent || parent->has_thrown_exception()) continue;
        bool seen = false;
        for (Module* m : exec_list) if (m == parent) { seen = true; break; }
        if (seen) continue;
        if (!parent->release_pending_async_dep()) continue;
        exec_list.push_back(parent);
        if (!parent->has_top_level_await()) gather_available_ancestors(parent, exec_list);
    }
}


Module* ModuleLoader::get_module(const std::string& module_id) {
    auto it = modules_.find(module_id);
    return (it != modules_.end()) ? it->second.get() : nullptr;
}

bool ModuleLoader::is_module_loaded(const std::string& module_id) const {
    auto it = modules_.find(module_id);
    return (it != modules_.end()) && it->second->is_loaded();
}

std::string ModuleLoader::resolve_module_path(const std::string& module_id, const std::string& from_path) {
    if (is_relative_path(module_id)) {
        std::string base_path = from_path.empty() ? "./" : std::filesystem::path(from_path).parent_path().string() + "/";
        std::string resolved = join_paths(base_path, module_id);
        
        if (file_exists(resolved)) {
            return resolved;
        }
        if (file_exists(resolved + ".js")) {
            return resolved + ".js";
        }
        if (file_exists(resolved + "/index.js")) {
            return resolved + "/index.js";
        }
    }
    
    if (is_absolute_path(module_id)) {
        if (file_exists(module_id)) {
            return module_id;
        }
        if (file_exists(module_id + ".js")) {
            return module_id + ".js";
        }
    }
    
    for (const auto& search_path : module_search_paths_) {
        std::string candidate = join_paths(search_path, module_id);
        
        if (file_exists(candidate)) {
            return candidate;
        }
        if (file_exists(candidate + ".js")) {
            return candidate + ".js";
        }
        if (file_exists(candidate + "/index.js")) {
            return candidate + "/index.js";
        }
    }
    
    return module_id;
}

void ModuleLoader::add_search_path(const std::string& path) {
    module_search_paths_.push_back(path);
}

// The names a module exports, known from its own declarations rather than
// from what running it happened to record. Instantiation needs them: a module
// that reaches a name it exports itself, and an importer asking whether a name
// resolves at all, both come before any of that module's code has run.
// Every module this one requests, resolved in source order. The spec settles
// the whole graph before any of it runs, and source order is what decides
// which dependency evaluates first; leaving each request to the statement that
// names it puts `export ... from` sources out of order, and one with no
// specifiers at all never gets asked for.
static bool load_requested_modules(Module* module, const Program* program,
                                   ModuleLoader* loader, const std::string& filename) {
    if (!program || !loader) return true;
    for (const auto& stmt : program->get_statements()) {
        if (!stmt) continue;
        std::string request;
        bool deferred = false;
        if (stmt->get_type() == ASTNode::Type::IMPORT_STATEMENT) {
            const auto* im = static_cast<const ImportStatement*>(stmt.get());
            // `import defer` still loads and links what it names -- a module
            // that will not parse, or a name it cannot resolve, is an error
            // before anything runs. Only its evaluation waits.
            deferred = im->is_deferred();
            request = im->get_module_source();
        } else if (stmt->get_type() == ASTNode::Type::EXPORT_STATEMENT) {
            request = static_cast<const ExportStatement*>(stmt.get())->get_source_module();
        }
        if (request.empty()) continue;
        Module* dep = loader->prepare_module(request, filename);
        if (!dep) {
            // A dependency that will not even parse is an error the importer
            // reports, and it reports it before running: the whole graph is
            // parsed during preparation, so an early error anywhere in it stops
            // everything.
            Value reason = loader->has_last_module_exception()
                               ? loader->get_last_module_exception()
                               : Value(Error::create_syntax_error(
                                     "Failed to load module '" + request + "'").release());
            module->set_thrown_exception(reason);
            return false;
        }
        if (deferred) {
            module->add_deferred_request(dep);
            // What suspends cannot be evaluated later from a property read, so
            // it is evaluated now. A deferred module that suspends itself is
            // one of those; one that merely depends on such a module stays
            // deferred while they run.
            if (dep->has_top_level_await()) {
                module->add_requested(dep);
            } else {
                std::vector<Module*> async_roots;
                loader->collect_async_roots(dep, async_roots);
                for (Module* root : async_roots) module->add_requested(root);
            }
        } else {
            module->add_requested(dep);
        }
        // A deferred import defers evaluation, not loading: an error from
        // parsing or linking the dependency still stops this module. An error
        // it recorded while actually evaluating -- which only a module that has
        // already run can carry -- is the deferred namespace's to report.
        if (dep->has_thrown_exception() && (!deferred || !dep->is_evaluated())) {
            module->set_thrown_exception(dep->get_thrown_exception());
            return false;
        }
    }
    return true;
}

static bool declare_module_exports(Module* module, const Program* program,
                                   ModuleLoader* loader, const std::string& filename,
                                   std::string& link_error) {
    if (!module || !program) return true;

    // ParseModule: `import { x } from './m'; export { x };` is not a local
    // export. The name belongs to the import, so the entry points where the
    // import does -- which is what makes two paths to the same binding
    // unambiguous rather than a disagreement between two modules.
    struct ImportedName { std::string request; std::string name; };
    std::unordered_map<std::string, ImportedName> imported;
    for (const auto& stmt : program->get_statements()) {
        if (!stmt || stmt->get_type() != ASTNode::Type::IMPORT_STATEMENT) continue;
        const auto* im = static_cast<const ImportStatement*>(stmt.get());
        // A namespace import re-exported by name is an indirect entry too, and
        // "*" is how the entry says it names the source's namespace rather
        // than one of its exports.
        // A deferred namespace is an object this module owns, not the source's
        // namespace: re-exporting it by name has to hand out that same object,
        // not the eager one the source would resolve to.
        if (im->is_namespace_import() && !im->is_deferred() &&
            !im->get_namespace_alias().empty()) {
            imported[im->get_namespace_alias()] = {im->get_module_source(), "*"};
        }
        if (im->is_default_import() && !im->get_default_alias().empty()) {
            imported[im->get_default_alias()] = {im->get_module_source(), "default"};
        }
        for (const auto& spec : im->get_specifiers()) {
            imported[spec->get_local_name()] = {im->get_module_source(), spec->get_imported_name()};
        }
    }

    for (const auto& stmt : program->get_statements()) {
        if (!stmt || stmt->get_type() != ASTNode::Type::EXPORT_STATEMENT) continue;
        const auto* ex = static_cast<const ExportStatement*>(stmt.get());

        if (ex->is_default_export()) {
            // Which module-scope name the default stands for, decided here so
            // an importer resolves through it rather than through whatever the
            // export record held when the import was made. A named function or
            // class keeps its own name; anything else gets the reserved one
            // ExportStatement::link binds.
            std::string local = "*default*";
            if (const ASTNode* de = ex->get_default_export()) {
                if (de->get_type() == ASTNode::Type::FUNCTION_EXPRESSION) {
                    const auto* fe = static_cast<const FunctionExpression*>(de);
                    if (fe->is_named()) local = fe->get_id()->get_name();
                } else if (de->get_type() == ASTNode::Type::ASYNC_FUNCTION_EXPRESSION) {
                    const auto* afe = static_cast<const AsyncFunctionExpression*>(de);
                    if (afe->get_id()) local = afe->get_id()->get_name();
                } else if (de->get_type() == ASTNode::Type::CLASS_DECLARATION) {
                    const auto* cd = static_cast<const ClassDeclaration*>(de);
                    if (cd->get_id() && !cd->get_id()->get_name().empty()) {
                        local = cd->get_id()->get_name();
                    }
                }
            }
            module->declare_export("default", local);
            continue;
        }

        if (ex->is_declaration_export() && ex->get_declaration()) {
            const ASTNode* decl = ex->get_declaration();
            if (decl->get_type() == ASTNode::Type::FUNCTION_DECLARATION) {
                const auto* fd = static_cast<const FunctionDeclaration*>(decl);
                if (fd->get_id()) module->declare_export(fd->get_id()->get_name(),
                                                         fd->get_id()->get_name());
            } else if (decl->get_type() == ASTNode::Type::CLASS_DECLARATION) {
                const auto* cd = static_cast<const ClassDeclaration*>(decl);
                if (cd->get_id()) module->declare_export(cd->get_id()->get_name(),
                                                         cd->get_id()->get_name());
            } else if (decl->get_type() == ASTNode::Type::VARIABLE_DECLARATION) {
                const auto* vd = static_cast<const VariableDeclaration*>(decl);
                for (const auto& d : vd->get_declarations()) {
                    if (d->get_init() &&
                        d->get_init()->get_type() == ASTNode::Type::DESTRUCTURING_ASSIGNMENT) {
                        std::vector<std::string> bound;
                        static_cast<const DestructuringAssignment*>(d->get_init())
                            ->collect_bound_names(bound);
                        for (const auto& bn : bound) module->declare_export(bn, bn);
                        continue;
                    }
                    if (d->get_id() && !d->get_id()->get_name().empty()) {
                        module->declare_export(d->get_id()->get_name(), d->get_id()->get_name());
                    }
                }
            }
            continue;
        }

        for (const auto& spec : ex->get_specifiers()) {
            const std::string& exported = spec->get_exported_name();
            const std::string& local = spec->get_local_name();
            if (exported == "*") {
                // `export * from './m'` contributes whatever that module has,
                // minus its default. Asking for it links that module, which
                // link time does anyway.
                if (!loader || ex->get_source_module().empty()) continue;
                module->add_star_source(loader->load_module(ex->get_source_module(), filename));
                continue;
            }
            if (exported.empty()) continue;
            if (!ex->is_re_export()) {
                auto from_import = imported.find(local);
                if (loader && from_import != imported.end() &&
                    !from_import->second.request.empty()) {
                    module->declare_export(exported, std::string());
                    module->declare_indirect_export(
                        exported, loader->load_module(from_import->second.request, filename),
                        from_import->second.name);
                    continue;
                }
                module->declare_export(exported, local);
                continue;
            }
            module->declare_export(exported, std::string());
            if (loader && !ex->get_source_module().empty()) {
                module->declare_indirect_export(
                    exported, loader->load_module(ex->get_source_module(), filename), local);
            }
        }
    }
    return true;
}


void DeferredNamespaceObject::ensure_evaluated() {
        if (evaluated_) return;
        Context* reader = Object::current_context_;
        // Reaching into a module that is part way through its own evaluation
        // cannot be answered: there is nothing to give and no way to wait.
        if (Module* pending = loader_->prepare_module(module_source_, from_path_)) {
            if (loader_->subgraph_evaluation_in_progress(pending)) {
                if (reader) {
                    reader->throw_type_error("Cannot access '" + module_source_ +
                                             "' while it is being evaluated");
                }
                return;
            }
        }
        Module* mod = loader_->load_module(module_source_, from_path_);
        if (!mod) return;
        // A module that threw keeps throwing: every later access reports the
        // error its evaluation ended with.
        if (mod->has_thrown_exception()) {
            if (reader) reader->throw_exception(mod->get_thrown_exception(), true);
            return;
        }
        evaluated_ = true;
        // The namespace is observably non-extensible; re-open it only for this
        // internal export copy.
        reopen_extensible();
        for (const auto& name : mod->get_export_names()) {
            // A namespace's exports are writable and enumerable but never
            // configurable, the same as the non-deferred namespace's.
            Object::set_property_default(
                name, mod->get_export(name),
                static_cast<PropertyAttributes>(PropertyAttributes::Writable |
                                                PropertyAttributes::Enumerable));
        }
        prevent_extensions();
    }

bool ModuleNamespaceObject::reading_would_throw(const std::string& key) const {
    if (!module_ || !module_->export_is_uninitialized(key)) return false;
    if (Context* reader = Object::current_context_) {
        reader->throw_reference_error("Cannot access '" + key + "' before initialization");
    }
    return true;
}

Value ImportBindingObject::resolve() const {
    if (!module_) return Value();
    // A re-export can name a binding that is itself an import of the same
    // name, which is a legal cycle to write and an endless one to follow.
    // Whatever asked first gets undefined rather than a second trip round.
    static thread_local std::vector<const ImportBindingObject*> in_progress;
    for (const ImportBindingObject* b : in_progress) if (b == this) return Value();
    in_progress.push_back(this);
    struct Pop {
        std::vector<const ImportBindingObject*>& v;
        ~Pop() { v.pop_back(); }
    } pop{in_progress};
    Value v = module_->get_export(export_name_);
    if (!v.is_undefined()) return v;
    // Undefined here is two different answers. If the export resolves to a
    // binding whose module has not reached the declaration yet, the name is in
    // its temporal dead zone and reading it throws, exactly as it would inside
    // the module that owns it.
    if (module_->export_is_uninitialized(export_name_)) {
        if (Context* reader = Object::current_context_) {
            reader->throw_reference_error("Cannot access '" + export_name_ +
                                          "' before initialization");
        }
        return Value();
    }
    Context* mc = module_->get_context();
    if (!mc) return v;
    // While the module is still running, its record is the `exports` object it
    // is building. Read the module's OWN binding for it rather than walking
    // out: the chain reaches the global, whose `exports` belongs to something
    // else entirely.
    if (Environment* env = mc->get_lexical_environment()) {
        if (env->has_own_binding("exports")) {
            Value exports = env->get_binding("exports");
            if (exports.is_object()) {
                Object* eo = exports.as_object();
                if (eo && eo->has_own_property(export_name_)) {
                    return eo->get_property(export_name_);
                }
            }
        }
    }
    if (module_scope_owner(mc, export_name_)) return mc->get_binding(export_name_);
    return v;
}

Value ModuleLoader::import_from_module(const std::string& module_id, const std::string& import_name, const std::string& from_path) {

    Module* module = load_module(module_id, from_path);
    if (!module) {
        return Value();
    }

    Value result = module->get_export(import_name);
    // Module is partially loaded (circular/self-import): fall back to context bindings
    if (result.is_undefined() && module->is_loading() &&
        module_scope_owner(module->get_context(), import_name)) {
        result = module->get_context()->get_binding(import_name);
    }
    return result;
}

// Whether the module actually provides this export, which is a different
// question from what the export holds: a name bound to undefined resolves,
// and a name that was never exported is a link error the importer has to
// raise before any of its own code runs.
bool ModuleLoader::module_provides_export(const std::string& module_id,
                                          const std::string& import_name,
                                          const std::string& from_path) {
    Module* module = load_module(module_id, from_path);
    if (!module) return false;
    if (module->has_export(import_name)) return true;
    // A module still evaluating has not recorded its exports yet, so the
    // binding its own scope already holds is the answer (a hoisted function,
    // or anything a cycle has reached).
    if (module->is_loading()) {
        return !module->get_context() || module_scope_owner(module->get_context(), import_name);
    }
    // The record is the whole story now: it is written from the module's own
    // declarations before it runs, and a `export * from` contributes the
    // source's names to it at the same time.
    return false;
}

Value ModuleLoader::import_default_from_module(const std::string& module_id, const std::string& from_path) {
    return import_from_module(module_id, "default", from_path);
}

// static
Value ModuleLoader::build_module_namespace(Module* module) {
    if (!module) return Value();
    if (module->has_namespace()) return module->get_namespace();
    auto* ns = new ModuleNamespaceObject(module);
    ns->prevent_extensions();
    Value ns_val(ns);
    module->set_namespace(ns_val);
    return ns_val;
}

Value ModuleLoader::import_namespace_from_module(const std::string& module_id, const std::string& from_path) {
    Module* module = load_module(module_id, from_path);
    if (!module) return Value();
    return build_module_namespace(module);
}

void ModuleLoader::register_builtin_module(const std::string& module_id, std::unique_ptr<Module> module) {
    module->set_loaded(true);
    modules_[module_id] = std::move(module);
}

std::unique_ptr<Module> ModuleLoader::create_module(const std::string& module_id, const std::string& filename) {
    return std::make_unique<Module>(module_id, filename);
}

bool ModuleLoader::prepare_module_file(Module* module, const std::string& filename) {
    std::string source = read_file(filename);
    if (source.empty()) {
        auto err = Error::create_type_error("Failed to fetch dynamically imported module '" + filename + "'");
        last_module_exception_ = Value(err.release());
        return false;
    }
    
    try {
        // A module is not its own realm. A fresh Global context would stand up
        // a second set of intrinsics, so `Error` inside the module and `Error`
        // in the importing script would be different functions and every
        // cross-module instanceof against them would answer false. The module
        // keeps a scope of its own for its top-level declarations, but that
        // scope's outer is the realm's global environment.
        Context* realm = engine_ ? engine_->get_global_context() : nullptr;
        std::unique_ptr<Context> module_context;
        if (realm) {
            module_context = std::make_unique<Context>(engine_, realm, Context::Type::Module);
            Object* module_scope = ObjectFactory::create_object().release();
            auto module_env = std::make_unique<Environment>(module_scope, realm->get_variable_environment());
            module_context->set_lexical_environment(module_env.get());
            module_context->set_variable_environment(module_env.release());
            module_context->set_global_object(realm->get_global_object());
            // A module's top-level `this` is undefined, not the global. The
            // global object is still the realm's, so a sloppy function called
            // from module code resolves its own `this` to it as usual.
            module_context->set_this_value(Value());
        } else {
            module_context = std::make_unique<Context>(engine_);
        }

        // Handed to the collector, not held here: a body that suspends on a
        // top-level await still reaches for `exports` long after this function
        // has returned, and the binding is what keeps them reachable.
        auto module_obj = ObjectFactory::create_object();
        module_context->create_binding("module", Value(module_obj.release()));
        auto exports_obj = ObjectFactory::create_object();
        module_context->create_binding("exports", Value(exports_obj.release()));
        module_context->create_binding("__filename", Value(filename));
        module_context->create_binding("__dirname", Value(std::filesystem::path(filename).parent_path().string()));
        
        Lexer::LexerOptions lex_opts;
        lex_opts.source_type_module = true;
        Lexer lexer(source, lex_opts);
        auto tokens = lexer.tokenize();
        TokenSequence token_sequence{tokens};
        Parser::ParseOptions parse_opts;
        parse_opts.source_type_module = true;
        parse_opts.strict_mode = true;
        Parser parser{token_sequence, parse_opts};
        auto ast = parser.parse_program();
        if (!ast || parser.has_errors()) {
            const auto& errs = parser.get_errors();
            std::string msg = errs.empty() ? "Failed to parse module" : errs[0].message;
            // Strip "SyntaxError: " prefix if present -- Error::create_syntax_error adds it
            if (msg.substr(0, 13) == "SyntaxError: ") msg = msg.substr(13);
            auto err = Error::create_syntax_error(msg);
            last_module_exception_ = Value(err.release());
            return false;
        }
        
        module->set_context(std::move(module_context));
        module->get_context()->set_current_filename(filename);

        // What this module exports is settled before it runs, so a name it
        // reaches through itself (or through a cycle) resolves, and an
        // importer asking whether a name exists gets a real answer.
        // Hoisting first: a dependency evaluated below can call back into this
        // module, and what it finds has to be what InitializeEnvironment would
        // have put there.
        ast->hoist_declarations(*module->get_context());
        if (module->get_context()->has_exception()) {
            module->set_thrown_exception(module->get_context()->get_exception());
            module->get_context()->clear_exception();
            module->set_loading(false);
            module->set_loaded(true);
            return true;
        }

        if (!load_requested_modules(module, ast.get(), this, filename)) {
            module->set_loading(false);
            module->set_loaded(true);
            return true;
        }

        std::string link_error;
        if (!declare_module_exports(module, ast.get(), this, filename, link_error)) {
            auto err = Error::create_syntax_error(link_error);
            module->set_thrown_exception(Value(err.release()));
            module->set_loading(false);
            module->set_loaded(true);
            return true;
        }

        module->set_has_top_level_await(
            BytecodeCompiler::module_body_suspends(ast->get_statements()));
        module->set_program(std::move(ast));
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error preparing module " << filename << ": " << e.what() << std::endl;
        return false;
    }
}

void ModuleLoader::evaluate_module(Module* module) {
    Program* program = static_cast<Program*>(module->program());
    if (!program || !module->get_context()) return;
    try {
        // Any module may suspend: what waits on it counts it as a pending
        // dependency and runs when it finishes.
        program->set_may_suspend(true);
        // Evaluating a module points the engine's current-context pointer at
        // that module and leaves it there. A module evaluated from inside
        // another one -- reaching through a deferred namespace does exactly
        // that -- must not leave the importer looking at its dependency.
        Context* outer_current = Object::current_context_;
        program->evaluate(*module->get_context());
        Object::current_context_ = outer_current;
        module->set_evaluation_promise(program->completion_promise());

        if (module->get_context()->has_exception()) {
            module->set_thrown_exception(module->get_context()->get_exception());
            module->get_context()->clear_exception();
        }

        // Local-name map recorded by ExportStatement::evaluate (export_name -> local
        // module-scope binding name) -- lets Module::get_export return live values.
        Object* local_names = nullptr;
        {
            Value ln = module->get_context()->get_binding("\x01localnames");
            if (ln.is_object()) local_names = ln.as_object();
        }

        Value exports_value = module->get_context()->get_binding("exports");
        if (exports_value.is_object()) {
            auto exports_obj = exports_value.as_object();

            auto keys = exports_obj->get_own_property_keys();

            for (const auto& key : keys) {
                Value prop_value = exports_obj->get_property(key);
                std::string local_name;
                if (local_names) {
                    Value ln = local_names->get_property(key);
                    if (ln.is_string()) local_name = ln.to_string();
                }
                module->add_export(key, prop_value, local_name);
            }
        }
    } catch (const std::exception& e) {
        if (!module->has_thrown_exception()) {
            module->set_thrown_exception(Value(std::string(e.what())));
        }
    }
}

std::string ModuleLoader::normalize_module_id(const std::string& module_id, const std::string& from_path) {
    if (is_relative_path(module_id) && !from_path.empty()) {
        std::string base_path = std::filesystem::path(from_path).parent_path().string();
        return std::filesystem::weakly_canonical(std::filesystem::path(base_path) / module_id).string();
    }
    // A file named two ways is one module. The entry is given as it was typed
    // and a relative import of the same file canonicalises, so without this
    // the two spellings became two module records and the file was evaluated
    // twice -- which a module that imports itself does by definition.
    std::error_code ec;
    if (std::filesystem::exists(std::filesystem::path(module_id), ec) && !ec) {
        std::string canonical = std::filesystem::weakly_canonical(
            std::filesystem::path(module_id), ec).string();
        if (!ec && !canonical.empty()) return canonical;
    }
    return module_id;
}

bool ModuleLoader::is_relative_path(const std::string& path) {
    return (path.length() >= 2 && path.substr(0, 2) == "./") || 
           (path.length() >= 3 && path.substr(0, 3) == "../");
}

bool ModuleLoader::is_absolute_path(const std::string& path) {
    return std::filesystem::path(path).is_absolute();
}

std::string ModuleLoader::join_paths(const std::string& base, const std::string& relative) {
    return (std::filesystem::path(base) / relative).string();
}

bool ModuleLoader::file_exists(const std::string& filename) {
    return std::filesystem::exists(filename) && std::filesystem::is_regular_file(filename);
}

std::string ModuleLoader::read_file(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        return "";
    }
    
    std::string content;
    std::string line;
    while (std::getline(file, line)) {
        content += line + "\n";
    }
    
    return content;
}

}
