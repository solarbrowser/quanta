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

Value Module::get_export(const std::string& name) const {
    // Live binding: if this export is a direct alias for a module-scope binding
    // (the common case -- `export var x`, `export { x }`, `export default function fn(){}`),
    // read the binding's CURRENT value so later reassignments are observable
    // through the module namespace, per ES module live-binding semantics.
    auto local_it = export_local_names_.find(name);
    if (local_it != export_local_names_.end() && module_context_ &&
        module_context_->has_binding(local_it->second)) {
        return module_context_->get_binding(local_it->second);
    }

    auto it = exports_.find(name);
    if (it != exports_.end()) {
        return it->second;
    }
    return Value();
}

void Module::add_star_source(Module* source) {
    if (!source || source == this) return;
    for (Module* m : star_sources_) if (m == source) return;
    star_sources_.push_back(source);
}

// A star source can name this module back, so the walk carries the modules it
// is already inside rather than trusting the graph to be a tree.
static bool module_has_export_through_stars(const Module* module, const std::string& name,
                                            std::vector<const Module*>& seen) {
    for (const Module* m : seen) if (m == module) return false;
    seen.push_back(module);
    if (module->has_own_export(name)) return true;
    for (Module* src : module->star_sources()) {
        if (name == "default") continue;  // `export *` never carries default
        if (module_has_export_through_stars(src, name, seen)) return true;
    }
    return false;
}

bool Module::has_own_export(const std::string& name) const {
    return exports_.find(name) != exports_.end();
}

bool Module::has_export(const std::string& name) const {
    if (exports_.find(name) != exports_.end()) return true;
    if (star_sources_.empty() || name == "default") return false;
    std::vector<const Module*> seen;
    return module_has_export_through_stars(this, name, seen);
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
    names.reserve(exports_.size());
    for (const auto& pair : exports_) {
        names.push_back(pair.first);
    }
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
        return it->second.get();
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

    if (!execute_module_file(module_ptr, resolved_path)) {
        loading_modules_.erase(normalized_id);
        modules_.erase(normalized_id);
        return nullptr;
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
            module->declare_export(exported, ex->is_re_export() ? std::string() : local);
        }
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
    if (mc->has_binding(export_name_)) return mc->get_binding(export_name_);
    return v;
}

Value ModuleLoader::import_from_module(const std::string& module_id, const std::string& import_name, const std::string& from_path) {

    Module* module = load_module(module_id, from_path);
    if (!module) {
        return Value();
    }

    Value result = module->get_export(import_name);
    // Module is partially loaded (circular/self-import): fall back to context bindings
    if (result.is_undefined() && module->is_loading() && module->get_context()) {
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
        return !module->get_context() || module->get_context()->has_binding(import_name);
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
        auto module_context = std::make_unique<Context>(engine_);

        // Share globalThis with the engine's global context so that cross-module
        // globalThis.xxx assignments are visible to the main script and vice versa.
        if (engine_ && engine_->get_global_context()) {
            Object* shared_global = engine_->get_global_context()->get_global_object();
            if (shared_global && module_context->get_global_object()) {
                PropertyDescriptor desc(Value(shared_global),
                    static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
                module_context->get_global_object()->set_property_descriptor("globalThis", desc);
                module_context->get_global_object()->set_property_descriptor("global", desc);
                module_context->get_global_object()->set_property_descriptor("window", desc);

                // A module shares its realm's global object: unqualified `this` inside a
                // sloppy-mode function (e.g. Function('return this;')()) called from module
                // code must resolve to the SAME global object the main script observes,
                // not the module's own isolated pseudo-global. The module's own global
                // object remains the binding object for its top-level var/function
                // declarations (captured by the lexical environment at construction), so
                // this only redirects `this`/Function.prototype.call resolution.
                module_context->set_global_object(shared_global);
            }
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
