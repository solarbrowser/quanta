/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/modules/ModuleLoader.h"
#include "quanta/core/engine/Engine.h"
#include "quanta/core/engine/Context.h"
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

// ES2022 10.4.6: Module Namespace Exotic Object.
// Reads every property live from the module's binding environment so that
// mutations to exported variables after import are observable (live bindings).
class ModuleNamespaceObject : public Object {
    Module* module_;

    static std::string tag_key() {
        Symbol* s = Symbol::get_well_known(Symbol::TO_STRING_TAG);
        return s ? s->to_property_key() : std::string();
    }

public:
    explicit ModuleNamespaceObject(Module* module)
        : Object(ObjectType::Custom), module_(module) {
        set_prototype(nullptr);
    }

    // [[Get]]: live binding for exports; "Module" for @@toStringTag
    Value get_property(const std::string& key) const override {
        std::string tk = tag_key();
        if (!tk.empty() && key == tk) return Value(std::string("Module"));
        if (module_ && module_->has_export(key)) return module_->get_export(key);
        return Value();
    }

    // [[HasProperty]] / [[GetOwnProperty]] presence check
    bool has_own_property(const std::string& key) const override {
        std::string tk = tag_key();
        if (!tk.empty() && key == tk) return true;
        if (module_) {
            for (auto& n : module_->get_export_names())
                if (n == key) return true;
        }
        return false;
    }

    // [[OwnPropertyKeys]]: sorted string export names, then @@toStringTag
    std::vector<std::string> get_own_property_keys() const override {
        std::vector<std::string> keys;
        if (module_) {
            keys = module_->get_export_names();
            std::sort(keys.begin(), keys.end());
        }
        std::string tk = tag_key();
        if (!tk.empty()) keys.push_back(tk);
        return keys;
    }

    // Enumerable keys: only the sorted string exports (@@toStringTag is non-enumerable)
    std::vector<std::string> get_enumerable_keys() const override {
        std::vector<std::string> keys;
        if (module_) {
            keys = module_->get_export_names();
            std::sort(keys.begin(), keys.end());
        }
        return keys;
    }

    // [[Set]]: always false per ES2022 10.4.6.5
    bool set_property(const std::string& /*key*/, const Value& /*value*/,
                      PropertyAttributes /*attrs*/ = PropertyAttributes::Default) override {
        return false;
    }

    // [[DefineOwnProperty]]: always false per ES2022 10.4.6.4
    bool set_property_descriptor(const std::string& /*key*/, const PropertyDescriptor& /*desc*/) override {
        return false;
    }

    // [[Delete]]: false for any own property (all non-configurable), true otherwise
    bool delete_property(const std::string& key) override {
        if (has_own_property(key)) return false;
        return true;
    }

    // [[GetOwnProperty]]: proper non-writable non-configurable descriptors
    PropertyDescriptor get_property_descriptor(const std::string& key) const override {
        std::string tk = tag_key();
        if (!tk.empty() && key == tk) {
            // @@toStringTag: non-writable, non-enumerable, non-configurable
            return PropertyDescriptor(Value(std::string("Module")), PropertyAttributes::None);
        }
        if (module_ && module_->has_export(key)) {
            // Spec 10.4.6.8: exports are {writable: true, enumerable: true, configurable: false}
            return PropertyDescriptor(module_->get_export(key), static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Enumerable));
        }
        return PropertyDescriptor();
    }
};

Module::Module(const std::string& id, const std::string& filename)
    : id_(id), filename_(filename), loaded_(false), loading_(false) {
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

bool Module::has_export(const std::string& name) const {
    return exports_.find(name) != exports_.end();
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

ModuleLoader::ModuleLoader(Engine* engine) : engine_(engine) {
    add_search_path("./");
    add_search_path("./node_modules/");
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
