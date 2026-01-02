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
#include <fstream>
#include <filesystem>
#include <iostream>

namespace Quanta {

Module::Module(const std::string& id, const std::string& filename)
    : id_(id), filename_(filename), loaded_(false), loading_(false) {
}

void Module::add_export(const std::string& name, const Value& value) {
    exports_[name] = value;
}

Value Module::get_export(const std::string& name) const {
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
    return result;
}

Value ModuleLoader::import_default_from_module(const std::string& module_id, const std::string& from_path) {
    return import_from_module(module_id, "default", from_path);
}

Value ModuleLoader::import_namespace_from_module(const std::string& module_id, const std::string& from_path) {
    Module* module = load_module(module_id, from_path);
    if (!module) {
        return Value();
    }
    
    auto exports_obj = std::make_shared<Object>();
    for (const auto& name : module->get_export_names()) {
        Value export_value = module->get_export(name);
        std::cout << "Namespace import: Adding '" << name << "' (type: " << (export_value.is_function() ? "function" : "other") << ")" << std::endl;
        exports_obj->set_property(name, export_value);
        
        Value retrieved_value = exports_obj->get_property(name);
        std::cout << "Namespace import: Retrieved '" << name << "' (type: " << (retrieved_value.is_function() ? "function" : "other") << ")" << std::endl;
    }
    
    return Value(exports_obj.get());
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
        std::cerr << "Failed to read module file: " << filename << std::endl;
        return false;
    }
    
    try {
        auto module_context = std::make_unique<Context>(engine_);
        
        auto module_obj = std::make_shared<Object>();
        module_context->create_binding("module", Value(module_obj.get()));
        auto exports_obj = std::make_shared<Object>();
        module_context->create_binding("exports", Value(exports_obj.get()));
        module_context->create_binding("__filename", Value(filename));
        module_context->create_binding("__dirname", Value(std::filesystem::path(filename).parent_path().string()));
        
        Lexer lexer(source);
        auto tokens = lexer.tokenize();
        TokenSequence token_sequence{tokens};
        Parser parser{token_sequence};
        auto ast = parser.parse_program();
        if (!ast) {
            std::cerr << "Failed to parse module: " << filename << std::endl;
            return false;
        }
        
        module->set_context(std::move(module_context));
        
        ast->evaluate(*module->get_context());
        
        Value exports_value = module->get_context()->get_binding("exports");
        if (exports_value.is_object()) {
            auto exports_obj = exports_value.as_object();
            
            auto keys = exports_obj->get_own_property_keys();

            for (const auto& key : keys) {
                Value prop_value = exports_obj->get_property(key);
                module->add_export(key, prop_value);
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
