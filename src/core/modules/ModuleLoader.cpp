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
#include <fstream>
#include <filesystem>
#include <iostream>
#include <algorithm>

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

void Module::add_star_source(Module* source) {
    if (!source || source == this) return;
    for (Module* m : star_sources_) if (m == source) return;
    star_sources_.push_back(source);
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
    return names;
}

void Module::set_context(std::unique_ptr<Context> context) {
    module_context_ = std::move(context);
}

void Module::gc_trace(Visitor& v) const {
    for (const auto& kv : exports_) v.visit(kv.second);
    v.visit(thrown_exception_);
    v.visit(namespace_);
}

ModuleLoader::ModuleLoader(Engine* engine) : engine_(engine) {
    add_search_path("./");
    add_search_path("./node_modules/");
}

void ModuleLoader::gc_trace(Visitor& v) const {
    for (const auto& kv : modules_) kv.second->gc_trace(v);
    v.visit(last_module_exception_);
}

Module* ModuleLoader::load_module(const std::string& module_id, const std::string& from_path) {
    last_module_exception_ = Value();
    std::string resolved_path = resolve_module_path(module_id, from_path);
    std::string normalized_id = normalize_module_id(module_id, from_path);
    
    auto it = modules_.find(normalized_id);
    if (it != modules_.end()) {
        Module* found = it->second.get();
        // Reaching a module that is still evaluating closes a cycle. Everything
        // that started evaluating since it began is part of that cycle, and the
        // whole cycle reports the one evaluation error recorded on this root.
        if (found->is_loading()) {
            for (size_t i = evaluating_.size(); i-- > 0 && evaluating_[i] != found; ) {
                found->add_cycle_member(evaluating_[i]);
            }
        }
        return found;
    }
    
    if (loading_modules_.find(normalized_id) != loading_modules_.end()) {
        std::cerr << "Circular dependency detected for module: " << normalized_id << std::endl;
        return nullptr;
    }
    
    auto module = create_module(normalized_id, resolved_path);
    if (!module) {
        return nullptr;
    }
    
    Module* module_ptr = module.get();
    modules_[normalized_id] = std::move(module);
    
    loading_modules_.insert(normalized_id);
    module_ptr->set_loading(true);

    evaluating_.push_back(module_ptr);
    bool executed = execute_module_file(module_ptr, resolved_path);
    evaluating_.pop_back();
    if (!executed) {
        loading_modules_.erase(normalized_id);
        modules_.erase(normalized_id);
        return nullptr;
    }

    if (module_ptr->has_thrown_exception()) {
        for (Module* member : module_ptr->cycle_members()) {
            if (!member->has_thrown_exception()) {
                member->set_thrown_exception(module_ptr->get_thrown_exception());
            }
        }
    }
    
    loading_modules_.erase(normalized_id);
    module_ptr->set_loading(false);
    module_ptr->set_loaded(true);
    
    return module_ptr;
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
static bool declare_module_exports(Module* module, const Program* program,
                                   ModuleLoader* loader, const std::string& filename,
                                   std::string& link_error) {
    if (!module || !program) return true;
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
                module->declare_export(exported, local);
                continue;
            }
            module->declare_export(exported, std::string());
            // `export * as ns from` names the source's namespace, not one of
            // its exports, so it stays a value this module owns.
            if (loader && local != "*" && !ex->get_source_module().empty()) {
                module->declare_indirect_export(
                    exported, loader->load_module(ex->get_source_module(), filename), local);
            }
        }
    }
    return true;
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

bool ModuleLoader::execute_module_file(Module* module, const std::string& filename) {
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
            module_context->set_this_value(Value(realm->get_global_object()));
        } else {
            module_context = std::make_unique<Context>(engine_);
        }

        auto module_obj = std::make_shared<Object>();
        module_context->create_binding("module", Value(module_obj.get()));
        auto exports_obj = std::make_shared<Object>();
        module_context->create_binding("exports", Value(exports_obj.get()));
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
        std::string link_error;
        if (!declare_module_exports(module, ast.get(), this, filename, link_error)) {
            auto err = Error::create_syntax_error(link_error);
            module->set_thrown_exception(Value(err.release()));
            module->set_loading(false);
            module->set_loaded(true);
            return true;
        }

        ast->evaluate(*module->get_context());

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
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error executing module " << filename << ": " << e.what() << std::endl;
        return false;
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
