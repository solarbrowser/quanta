/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_MODULE_LOADER_H
#define QUANTA_MODULE_LOADER_H

#include <string>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include "quanta/core/runtime/Value.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/Symbol.h"

namespace Quanta {

class Engine;
class Context;
class ASTNode;
class Visitor;


class Module {
private:
    std::string id_;
    std::string filename_;
    std::unordered_map<std::string, Value> exports_;
    std::unordered_map<std::string, std::string> export_local_names_;
    std::vector<Module*> star_sources_;
    // `export { x as y } from './m'`: the export name and where it forwards to.
    // ResolveExport follows these; the module holds no binding of its own for
    // them, which is what lets a chain run back through a cycle.
    std::unordered_map<std::string, std::pair<Module*, std::string>> indirect_exports_;
    std::vector<Module*> cycle_members_;
    // Every module this one requests, in source order: the graph's edges.
    std::vector<Module*> requested_;
    // Requested with `import defer`: part of the graph that gets linked, but
    // not part of what evaluates when this module does.
    std::vector<Module*> deferred_requests_;
    // Where the evaluation walk has got to. "Evaluating" means on the walk's
    // own stack, which is what separates a module in the cycle being climbed
    // out of from one that already belongs to a finished component.
    enum class EvalStatus : uint8_t { NotStarted, Evaluating, EvaluatingAsync, Evaluated };
    EvalStatus status_ = EvalStatus::NotStarted;
    // A module is async while its own body is suspended on a top-level await,
    // or while it is still waiting for a dependency that is. Its parents wait
    // on it by counting; it wakes them when it finishes.
    bool async_evaluating_ = false;
    int pending_async_deps_ = 0;
    std::vector<Module*> async_parents_;
    // The order modules became async in, which is the order they run in once
    // what they were waiting for arrives.
    int async_order_ = 0;
    bool has_tla_ = false;
    // Tarjan bookkeeping for the evaluation walk. A cycle's members share one
    // root, and that root is what an outsider waits on and what records the
    // error the whole cycle reports.
    int dfs_index_ = 0;
    int dfs_ancestor_index_ = 0;
    Module* cycle_root_ = nullptr;
    std::unique_ptr<Context> module_context_;
    std::unique_ptr<ASTNode> program_;
    Value evaluation_promise_;
    Value completion_;
    bool loaded_;
    bool loading_;
    Value thrown_exception_;

public:
    Module(const std::string& id, const std::string& filename);
    ~Module();

    // The module keeps its own parse tree. A body that suspends on a top-level
    // await is still running after the loader has returned, and the chunk it
    // runs holds the export statements it has to link.
    void set_program(std::unique_ptr<ASTNode> program);
    ASTNode* program() const { return program_.get(); }

    // Visits exports_/thrown_exception_/namespace_ -- the collector's own
    // roots have no reach into modules at all otherwise. module_context_ is
    // deliberately NOT visited here: it needs revisit_context's force-
    // retrace-every-slice treatment, which only exists on the concrete
    // MarkVisitor (not the abstract Visitor this method takes), so the
    // caller (Collector.cpp) visits get_context() itself.
    void gc_trace(Visitor& v) const;

    const std::string& get_id() const { return id_; }
    const std::string& get_filename() const { return filename_; }
    bool is_loaded() const { return loaded_; }
    bool is_loading() const { return loading_; }

    void add_export(const std::string& name, const Value& value, const std::string& local_name = "");
    // The name is exported; what it holds is decided when the module runs.
    void declare_export(const std::string& name, const std::string& local_name = "");
    // `export * from './m'`: what that module exports is this module's too,
    // minus its default. Kept as a link rather than a copy -- the source's own
    // record is still growing while a cycle is being instantiated.
    void add_star_source(Module* source);
    const std::vector<Module*>& star_sources() const { return star_sources_; }

    // Modules that reached back into this one while it was still evaluating,
    // i.e. the rest of the strongly connected component this module roots.
    // They share its evaluation error.
    void add_cycle_member(Module* member);

    void declare_indirect_export(const std::string& export_name, Module* source,
                                 const std::string& import_name);

    // ResolveExport: which module actually owns a name, following both
    // indirect re-exports and star sources. A name reached twice on one walk
    // is circular and resolves to nothing; two star sources arriving at
    // different owners make it ambiguous.
    struct Resolution {
        const Module* module = nullptr;
        std::string name;
        bool ambiguous = false;
        explicit operator bool() const { return module != nullptr; }
    };
    Resolution resolve_export(const std::string& name) const;

    // Whether the binding this export names exists but has not run yet.
    // Reading it through an import has to fail the way reading the name in
    // its own module would, not answer undefined.
    bool export_is_uninitialized(const std::string& name) const;

    const std::pair<Module*, std::string>* indirect_export(const std::string& name) const {
        auto it = indirect_exports_.find(name);
        return it == indirect_exports_.end() ? nullptr : &it->second;
    }
    const std::vector<Module*>& cycle_members() const { return cycle_members_; }

    void add_requested(Module* dep);
    const std::vector<Module*>& requested() const { return requested_; }
    void add_deferred_request(Module* dep);
    const std::vector<Module*>& deferred_requests() const { return deferred_requests_; }
    bool is_evaluated() const { return status_ != EvalStatus::NotStarted; }
    bool is_on_eval_stack() const { return status_ == EvalStatus::Evaluating; }
    // Started but not finished: on the walk's stack, or suspended partway.
    bool evaluation_in_progress() const {
        return status_ == EvalStatus::Evaluating || status_ == EvalStatus::EvaluatingAsync;
    }
    void mark_evaluating() { status_ = EvalStatus::Evaluating; }
    void mark_evaluated() { status_ = EvalStatus::Evaluated; }
    void mark_evaluating_async() { status_ = EvalStatus::EvaluatingAsync; }

    bool is_async_evaluating() const { return async_evaluating_; }
    void set_async_evaluating(bool v) { async_evaluating_ = v; }
    int pending_async_deps() const { return pending_async_deps_; }
    void add_pending_async_dep() { ++pending_async_deps_; }
    bool release_pending_async_dep() { return --pending_async_deps_ <= 0; }
    void add_async_parent(Module* parent);
    const std::vector<Module*>& async_parents() const { return async_parents_; }
    int async_order() const { return async_order_; }
    void set_async_order(int n) { async_order_ = n; }
    bool has_top_level_await() const { return has_tla_; }
    void set_has_top_level_await(bool v) { has_tla_ = v; }

    int dfs_index() const { return dfs_index_; }
    void set_dfs_index(int n) { dfs_index_ = n; }
    int dfs_ancestor_index() const { return dfs_ancestor_index_; }
    void set_dfs_ancestor_index(int n) { dfs_ancestor_index_ = n; }
    Module* cycle_root() { return cycle_root_ ? cycle_root_ : this; }
    void set_cycle_root(Module* m) { cycle_root_ = m; }
    Value get_export(const std::string& name) const;
    bool has_export(const std::string& name) const;
    bool has_own_export(const std::string& name) const;
    std::vector<std::string> get_export_names() const;
    std::vector<std::string> own_export_names() const;

    void set_context(std::unique_ptr<Context> context);
    Context* get_context() const { return module_context_.get(); }

    void set_loaded(bool loaded) { loaded_ = loaded; }
    void set_loading(bool loading) { loading_ = loading; }
    // A body with a top-level await is still running when the loader returns.
    // This is what settles when it finishes, and what an importer waits on.
    void set_evaluation_promise(const Value& v) { evaluation_promise_ = v; }
    const Value& get_evaluation_promise() const { return evaluation_promise_; }

    // What an outsider waits on. The module settles this itself, before it
    // wakes anything that was waiting on it as a dependency -- the module's own
    // completion is observed first.
    const Value& completion() const { return completion_; }
    void set_completion(const Value& v) { completion_ = v; }

    void set_thrown_exception(const Value& v) { thrown_exception_ = v; }
    const Value& get_thrown_exception() const { return thrown_exception_; }
    bool has_thrown_exception() const { return !thrown_exception_.is_undefined(); }

    // Deferring the same module twice names the same object, so the loader
    // keeps it here rather than making a new one per import statement.
    const Value& get_deferred_namespace() const { return deferred_namespace_; }
    void set_deferred_namespace(const Value& v) { deferred_namespace_ = v; }

    // Cached namespace object -- spec requires same namespace for same module
    const Value& get_namespace() const { return namespace_; }
    bool has_namespace() const { return !namespace_.is_undefined(); }
    void set_namespace(const Value& ns) { namespace_ = ns; }

private:
    Value namespace_;
    Value deferred_namespace_;
};

// ES2022 10.4.6: Module Namespace Exotic Object.
// Reads every property live from the module's binding environment so that
// mutations to exported variables after import are observable (live bindings).
// Moved here (out of ModuleLoader.cpp) so CustomObjectBase's own switch
// (Object.cpp) can name this class directly for its ObjectType::Custom /
// CustomKind::ModuleNamespace case.
// One imported name, resolved where it is read rather than where it was
// imported. An import is an alias for the exporting module's binding, so a
// value the exporting module has not produced yet (a class it declares later,
// a name a cycle reaches back for) is still seen once it exists.
class ImportBindingObject : public CustomObjectBase {
    Module* module_;
    std::string export_name_;

public:
    ImportBindingObject(Module* module, std::string export_name)
        : CustomObjectBase(ObjectType::Custom), module_(module),
          export_name_(std::move(export_name)) {
        set_custom_kind(CustomKind::ImportBinding);
        set_prototype(nullptr);
    }

    Module* module() const { return module_; }
    const std::string& export_name() const { return export_name_; }
    Value resolve() const;
};

class ModuleNamespaceObject : public CustomObjectBase {
    Module* module_;

    static std::string tag_key() {
        Symbol* s = Symbol::get_well_known(Symbol::TO_STRING_TAG);
        return s ? s->to_property_key() : std::string();
    }

public:
    explicit ModuleNamespaceObject(Module* module)
        : CustomObjectBase(ObjectType::Custom), module_(module) {
        set_custom_kind(CustomKind::ModuleNamespace);
        set_prototype(nullptr);
    }

    // A binding the module declared but has not reached yet is in its dead
    // zone, and reading it through the namespace throws exactly as reading the
    // name inside the module would. Every namespace read that produces a value
    // goes through here, [[GetOwnProperty]] included, since that one reads the
    // value to build its descriptor.
    bool reading_would_throw(const std::string& key) const;

    // [[Get]]: live binding for exports; "Module" for @@toStringTag
    Value get_property(const std::string& key) const {
        std::string tk = tag_key();
        if (!tk.empty() && key == tk) return Value(std::string("Module"));
        if (module_ && module_->has_export(key)) {
            if (reading_would_throw(key)) return Value();
            return module_->get_export(key);
        }
        return Value();
    }

    // Whether the name is one of this namespace's own properties, asked
    // without reading anything. [[Delete]] and the export listings want this;
    // [[GetOwnProperty]] wants the reading version below.
    bool exports_include(const std::string& key) const {
        std::string tk = tag_key();
        if (!tk.empty() && key == tk) return true;
        if (module_) {
            for (auto& n : module_->get_export_names())
                if (n == key) return true;
        }
        return false;
    }

    // [[HasProperty]] (10.4.6.4) answers from the export list alone. It reads
    // no value, so a binding still in its dead zone is present, not an error --
    // unlike [[GetOwnProperty]] below, which has to build a descriptor.
    bool has_property(const std::string& key) const { return exports_include(key); }

    // [[GetOwnProperty]] presence check. It builds a descriptor, which means
    // reading the value, so a binding still in its dead zone throws here too.
    bool has_own_property(const std::string& key) const {
        if (!exports_include(key)) return false;
        return !reading_would_throw(key);
    }

    // [[OwnPropertyKeys]]: sorted string export names, then @@toStringTag
    std::vector<std::string> get_own_property_keys() const {
        std::vector<std::string> keys;
        if (module_) {
            keys = module_->get_export_names();
            std::sort(keys.begin(), keys.end());
        }
        std::string tk = tag_key();
        if (!tk.empty()) keys.push_back(tk);
        return keys;
    }

    // Enumerable keys: only the sorted string exports (@@toStringTag is
    // non-enumerable). Deciding a name is enumerable means asking for its
    // descriptor, so a binding in its dead zone stops the enumeration.
    std::vector<std::string> get_enumerable_keys() const {
        std::vector<std::string> keys;
        if (module_) {
            keys = module_->get_export_names();
            std::sort(keys.begin(), keys.end());
            for (const auto& k : keys) {
                if (reading_would_throw(k)) return {};
            }
        }
        return keys;
    }

    // [[Set]]: always false per ES2022 10.4.6.5
    bool set_property(const std::string& /*key*/, const Value& /*value*/,
                      PropertyAttributes /*attrs*/ = PropertyAttributes::Default) {
        return false;
    }

    // [[DefineOwnProperty]]: always false per ES2022 10.4.6.4
    // [[DefineOwnProperty]] (ES2024 10.4.6.6). A namespace's exports are fixed,
    // so nothing can be added or altered -- but a descriptor that ASKS for
    // nothing the property does not already have is a no-op, and a no-op
    // succeeds. `Object.seal` and friends lean on that.
    bool set_property_descriptor(const std::string& key, const PropertyDescriptor& desc) {
        if (!has_own_property(key)) return false;
        if (desc.is_accessor_descriptor()) return false;
        if (desc.has_configurable() && desc.is_configurable()) return false;
        // @@toStringTag is the one own property that is not enumerable and not
        // writable; the exports are both.
        const bool is_tag = (key == tag_key());
        if (desc.has_enumerable() && desc.is_enumerable() == is_tag) return false;
        if (desc.has_writable() && desc.is_writable() == is_tag) return false;
        if (desc.has_value()) {
            PropertyDescriptor current = get_property_descriptor(key);
            return desc.get_value().same_value(current.get_value());
        }
        return true;
    }

    // [[Delete]]: false for any own property (all non-configurable), true otherwise
    bool delete_property(const std::string& key) {
        if (exports_include(key)) return false;
        return true;
    }

    // [[GetOwnProperty]]: proper non-writable non-configurable descriptors
    PropertyDescriptor get_property_descriptor(const std::string& key) const {
        std::string tk = tag_key();
        if (!tk.empty() && key == tk) {
            // @@toStringTag: non-writable, non-enumerable, non-configurable
            return PropertyDescriptor(Value(std::string("Module")), PropertyAttributes::None);
        }
        if (module_ && module_->has_export(key)) {
            // Spec 10.4.6.8: exports are {writable: true, enumerable: true, configurable: false}
            if (reading_would_throw(key)) return PropertyDescriptor();
            return PropertyDescriptor(module_->get_export(key), static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Enumerable));
        }
        return PropertyDescriptor();
    }
};

/**
 * Manages module loading, resolution, and dependency tracking
 */
class ModuleLoader {
private:
    Engine* engine_;
    std::unordered_map<std::string, std::unique_ptr<Module>> modules_;
    std::unordered_set<std::string> loading_modules_;
    // Modules currently being evaluated, innermost last: a cycle is closed when
    // an import reaches one of these.
    std::vector<Module*> evaluating_;
    // Modules being prepared, innermost last. A request reaching one of these
    // closes a cycle in the graph.
    std::vector<Module*> preparing_;
    std::vector<std::string> module_search_paths_;
    Value last_module_exception_;

public:
    explicit ModuleLoader(Engine* engine);
    ~ModuleLoader() = default;

    Module* load_module(const std::string& module_id, const std::string& from_path = "");
    // Parses a module, stands up its scope and settles its export table --
    // and does the same for everything it requests -- without running any of
    // it. load_module is this plus linking and evaluating the graph.
    Module* prepare_module(const std::string& module_id, const std::string& from_path);

    // Whether anything in this module's subgraph suspends on a top-level
    // await. Deferring such a module is not possible: reaching through the
    // namespace later would have to wait, and a property read cannot.
    bool subgraph_has_top_level_await(Module* module);

    // Whether anything in this module's subgraph is part way through its own
    // evaluation. Evaluating the module now would mean waiting on one of
    // those, and a property read cannot wait.
    bool subgraph_evaluation_in_progress(Module* module);

    // The modules inside a deferred subgraph that cannot wait to be evaluated:
    // the ones that suspend. They run with everything else, while the module
    // that was deferred stays deferred.
    void collect_async_roots(Module* module, std::vector<Module*>& out);

    // Links and evaluates a module that is already prepared.
    void evaluate_module_graph(Module* module);
    Module* get_module(const std::string& module_id);
    bool is_module_loaded(const std::string& module_id) const;
    const Value& get_last_module_exception() const { return last_module_exception_; }
    bool has_last_module_exception() const { return !last_module_exception_.is_undefined(); }

    // Visits every loaded module's exports_/thrown_exception_/namespace_ and
    // last_module_exception_. Modules are otherwise entirely outside the
    // collector's root set (see Collector.cpp's engine loops, which also
    // separately visit each module's Context via get_context() below).
    void gc_trace(Visitor& v) const;
    const std::unordered_map<std::string, std::unique_ptr<Module>>& modules() const { return modules_; }

    std::string resolve_module_path(const std::string& module_id, const std::string& from_path = "");
    void add_search_path(const std::string& path);

    Value import_from_module(const std::string& module_id, const std::string& import_name, const std::string& from_path = "");
    // Whether the module provides this export at all, which is a different
    // question from what it holds: a name bound to undefined resolves, a name
    // never exported is a link error.
    bool module_provides_export(const std::string& module_id, const std::string& import_name,
                                const std::string& from_path = "");
    Value import_default_from_module(const std::string& module_id, const std::string& from_path = "");
    Value import_namespace_from_module(const std::string& module_id, const std::string& from_path = "");

    // Build a live-binding module namespace object for `module` (used by both
    // static namespace imports and dynamic import() resolution).
    static Value build_module_namespace(Module* module);

    void register_builtin_module(const std::string& module_id, std::unique_ptr<Module> module);

private:
    std::unique_ptr<Module> create_module(const std::string& module_id, const std::string& filename);
    // Linking and evaluation are separate passes over the graph. Preparing a
    // module parses it, stands up its scope, hoists its declarations and
    // settles its export table, and prepares everything it requests -- without
    // running a single statement of any of it. Only once the whole graph is
    // prepared and linked does anything evaluate.
    bool prepare_module_file(Module* module, const std::string& filename);
    bool link_graph(Module* root);
    void evaluate_graph(Module* root);
    // InnerModuleEvaluation: post-order over the graph, assigning each module
    // the cycle it belongs to as it comes back up.
    int inner_evaluate(Module* module, std::vector<Module*>& stack, int index);
    void evaluate_module(Module* module);
    // Runs a module's body now that nothing it depends on is still pending,
    // and settles what waits on it when it is done.
    void execute_module_body(Module* module, bool notify_on_success);
    void async_module_finished(Module* module, bool ok, const Value& reason,
                               bool wake_parents = true);
    void gather_available_ancestors(Module* module, std::vector<Module*>& exec_list);
    int next_async_order_ = 1;
    std::string normalize_module_id(const std::string& module_id, const std::string& from_path);
    bool is_relative_path(const std::string& path);
    bool is_absolute_path(const std::string& path);
    std::string join_paths(const std::string& base, const std::string& relative);
    bool file_exists(const std::string& filename);
    std::string read_file(const std::string& filename);
};

// Namespace object for a deferred (`import defer * as ns from ...`) module:
// the module doesn't actually load/evaluate until its first property access
// (spec: import-defer proposal). Moved here from language.cpp (originally
// .cpp-local) for the same reason as ModuleNamespaceObject above --
// CustomObjectBase's switch dispatch (Object.cpp) needs to name it directly.
class DeferredNamespaceObject : public CustomObjectBase {
    static std::string tag_key() {
        Symbol* s = Symbol::get_well_known(Symbol::TO_STRING_TAG);
        return s ? s->to_property_key() : std::string();
    }

    ModuleLoader* loader_;
    std::string module_source_;
    std::string from_path_;
    bool evaluated_ = false;

    void ensure_evaluated();

    static bool is_symbol_like(const std::string& key) {
        // Per spec: Symbol keys and "then" do not trigger deferred evaluation.
        // Symbol keys in this engine are stored as "@@sym:N" or "Symbol.xxx".
        if (key == "then") return true;
        if (key.size() >= 5 && key.substr(0, 5) == "@@sym") return true;
        if (key.size() >= 7 && key.substr(0, 7) == "Symbol.") return true;
        return false;
    }

public:
    DeferredNamespaceObject(ModuleLoader* loader, const std::string& src, const std::string& from)
        // Explicit ObjectType::Custom (matching ModuleNamespaceObject, its
        // non-deferred sibling) -- previously fell through to the implicit
        // default (ObjectType::Ordinary), which the GC sweep's destructor
        // dispatch cannot tell apart from a genuinely plain Object.
        : CustomObjectBase(ObjectType::Custom), loader_(loader), module_source_(src), from_path_(from) {
        set_custom_kind(CustomKind::DeferredNamespace);
        // Stamped before the object closes: reading it must not be what makes
        // the module evaluate.
        std::string tag = tag_key();
        if (!tag.empty()) {
            Object::set_property_default(tag, Value(std::string("Deferred Module")),
                                         PropertyAttributes::None);
        }
        // Namespace objects are never extensible (spec 10.4.6): PrivateFieldAdd
        // on one throws TypeError without triggering deferred evaluation.
        prevent_extensions();
    }

    Value get_property(const std::string& key) const {
        if (!is_symbol_like(key))
            const_cast<DeferredNamespaceObject*>(this)->ensure_evaluated();
        return Object::get_property_default(key);
    }

    bool has_own_property(const std::string& key) const {
        if (!is_symbol_like(key))
            const_cast<DeferredNamespaceObject*>(this)->ensure_evaluated();
        return Object::has_own_property_default(key);
    }

    bool has_property(const std::string& key) const {
        if (!is_symbol_like(key))
            const_cast<DeferredNamespaceObject*>(this)->ensure_evaluated();
        return Object::has_property_default(key);
    }

    bool set_property(const std::string& key, const Value& value, PropertyAttributes attrs = PropertyAttributes::Default) {
        // Spec: [[Set]] on a namespace object always returns false without triggering evaluation.
        return false;
    }

    bool set_property_descriptor(const std::string& key, const PropertyDescriptor& desc) {
        // Spec: [[DefineOwnProperty]] on a namespace object triggers evaluation for non-symbol-like keys.
        if (!is_symbol_like(key))
            ensure_evaluated();
        return Object::set_property_descriptor_default(key, desc);
    }

    PropertyDescriptor get_property_descriptor(const std::string& key) const {
        // Spec: [[GetOwnProperty]] on a deferred namespace object triggers evaluation for non-symbol-like keys.
        if (!is_symbol_like(key))
            const_cast<DeferredNamespaceObject*>(this)->ensure_evaluated();
        return Object::get_property_descriptor_default(key);
    }

    bool delete_property(const std::string& key) {
        if (!is_symbol_like(key))
            ensure_evaluated();
        return Object::delete_property_default(key);
    }

    // @@toStringTag is an own property of a namespace, deferred or not, and it
    // comes after the exports: the string keys are sorted, the symbol is last.
    std::vector<std::string> get_own_property_keys() const {
        const_cast<DeferredNamespaceObject*>(this)->ensure_evaluated();
        std::vector<std::string> keys;
        std::string tag = tag_key();
        for (const auto& k : Object::get_own_property_keys_default()) {
            if (k != tag) keys.push_back(k);
        }
        std::sort(keys.begin(), keys.end());
        if (!tag.empty()) keys.push_back(tag);
        return keys;
    }

    std::vector<std::string> get_enumerable_keys() const {
        const_cast<DeferredNamespaceObject*>(this)->ensure_evaluated();
        std::vector<std::string> keys;
        std::string tag = tag_key();
        for (const auto& k : Object::get_enumerable_keys_default()) {
            if (k != tag) keys.push_back(k);
        }
        std::sort(keys.begin(), keys.end());
        return keys;
    }
};

}

#endif
