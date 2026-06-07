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
#include "quanta/core/runtime/Value.h"

namespace Quanta {

class Engine;
class Context;
class ASTNode;


class Module {
private:
    std::string id_;
    std::string filename_;
    std::unordered_map<std::string, Value> exports_;
    std::unordered_map<std::string, std::string> export_local_names_;
    std::unique_ptr<Context> module_context_;
    bool loaded_;
    bool loading_;
    Value thrown_exception_; 

public:
    Module(const std::string& id, const std::string& filename);
    ~Module() = default;

    const std::string& get_id() const { return id_; }
    const std::string& get_filename() const { return filename_; }
    bool is_loaded() const { return loaded_; }
    bool is_loading() const { return loading_; }

    void add_export(const std::string& name, const Value& value, const std::string& local_name = "");
    Value get_export(const std::string& name) const;
    bool has_export(const std::string& name) const;
    std::vector<std::string> get_export_names() const;

    void set_context(std::unique_ptr<Context> context);
    Context* get_context() const { return module_context_.get(); }

    void set_loaded(bool loaded) { loaded_ = loaded; }
    void set_loading(bool loading) { loading_ = loading; }
    void set_thrown_exception(const Value& v) { thrown_exception_ = v; }
    const Value& get_thrown_exception() const { return thrown_exception_; }
    bool has_thrown_exception() const { return !thrown_exception_.is_undefined(); }

    // Cached namespace object -- spec requires same namespace for same module
    const Value& get_namespace() const { return namespace_; }
    bool has_namespace() const { return !namespace_.is_undefined(); }
    void set_namespace(const Value& ns) { namespace_ = ns; }

private:
    Value namespace_;
};

/**
 * Manages module loading, resolution, and dependency tracking
 */
class ModuleLoader {
private:
    Engine* engine_;
    std::unordered_map<std::string, std::unique_ptr<Module>> modules_;
    std::unordered_set<std::string> loading_modules_;
    std::vector<std::string> module_search_paths_;
    Value last_module_exception_;

public:
    explicit ModuleLoader(Engine* engine);
    ~ModuleLoader() = default;

    Module* load_module(const std::string& module_id, const std::string& from_path = "");
    Module* get_module(const std::string& module_id);
    bool is_module_loaded(const std::string& module_id) const;
    const Value& get_last_module_exception() const { return last_module_exception_; }
    bool has_last_module_exception() const { return !last_module_exception_.is_undefined(); }

    std::string resolve_module_path(const std::string& module_id, const std::string& from_path = "");
    void add_search_path(const std::string& path);

    Value import_from_module(const std::string& module_id, const std::string& import_name, const std::string& from_path = "");
    Value import_default_from_module(const std::string& module_id, const std::string& from_path = "");
    Value import_namespace_from_module(const std::string& module_id, const std::string& from_path = "");

    void register_builtin_module(const std::string& module_id, std::unique_ptr<Module> module);

private:
    std::unique_ptr<Module> create_module(const std::string& module_id, const std::string& filename);
    bool execute_module_file(Module* module, const std::string& filename);
    std::string normalize_module_id(const std::string& module_id, const std::string& from_path);
    bool is_relative_path(const std::string& path);
    bool is_absolute_path(const std::string& path);
    std::string join_paths(const std::string& base, const std::string& relative);
    bool file_exists(const std::string& filename);
    std::string read_file(const std::string& filename);
};

}

#endif
