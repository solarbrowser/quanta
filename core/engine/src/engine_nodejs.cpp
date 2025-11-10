/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../include/engine_nodejs.h"
#include "../../include/Engine.h"
#include "../../include/Object.h"
#include "../../include/Value.h"
#include "NodeJS.h"
#include <memory>

namespace Quanta {

// EXTRACTED FROM Engine.cpp - Node.js APIs setup (lines 307-414)

void EngineNodeJS::setup_nodejs_apis(Engine& engine) {
    setup_fs_module(engine);
    setup_path_module(engine);
    setup_os_module(engine);
    setup_process_module(engine);
    setup_crypto_module(engine);
}

//=============================================================================
// File System API (originally lines 308-345)
//=============================================================================

void EngineNodeJS::setup_fs_module(Engine& engine) {
    auto fs_obj = create_module_object();

    // Async functions
    add_function_to_object(fs_obj.get(), "readFile", NodeJS::fs_readFile);
    add_function_to_object(fs_obj.get(), "writeFile", NodeJS::fs_writeFile);
    add_function_to_object(fs_obj.get(), "appendFile", NodeJS::fs_appendFile);
    add_function_to_object(fs_obj.get(), "exists", NodeJS::fs_exists);
    add_function_to_object(fs_obj.get(), "mkdir", NodeJS::fs_mkdir);
    add_function_to_object(fs_obj.get(), "rmdir", NodeJS::fs_rmdir);
    add_function_to_object(fs_obj.get(), "unlink", NodeJS::fs_unlink);
    add_function_to_object(fs_obj.get(), "stat", NodeJS::fs_stat);
    add_function_to_object(fs_obj.get(), "readdir", NodeJS::fs_readdir);

    // Sync functions
    add_function_to_object(fs_obj.get(), "readFileSync", NodeJS::fs_readFileSync);
    add_function_to_object(fs_obj.get(), "writeFileSync", NodeJS::fs_writeFileSync);
    add_function_to_object(fs_obj.get(), "existsSync", NodeJS::fs_existsSync);
    add_function_to_object(fs_obj.get(), "mkdirSync", NodeJS::fs_mkdirSync);
    add_function_to_object(fs_obj.get(), "statSync", NodeJS::fs_statSync);
    add_function_to_object(fs_obj.get(), "readdirSync", NodeJS::fs_readdirSync);

    engine.set_global_property("fs", Value(fs_obj.release()));
}

//=============================================================================
// Path API (originally lines 347-366)
//=============================================================================

void EngineNodeJS::setup_path_module(Engine& engine) {
    auto path_obj = create_module_object();

    add_function_to_object(path_obj.get(), "join", NodeJS::path_join);
    add_function_to_object(path_obj.get(), "resolve", NodeJS::path_resolve);
    add_function_to_object(path_obj.get(), "dirname", NodeJS::path_dirname);
    add_function_to_object(path_obj.get(), "basename", NodeJS::path_basename);
    add_function_to_object(path_obj.get(), "extname", NodeJS::path_extname);
    add_function_to_object(path_obj.get(), "normalize", NodeJS::path_normalize);
    add_function_to_object(path_obj.get(), "isAbsolute", NodeJS::path_isAbsolute);

    engine.set_global_property("path", Value(path_obj.release()));
}

//=============================================================================
// OS API (originally lines 368-385)
//=============================================================================

void EngineNodeJS::setup_os_module(Engine& engine) {
    auto os_obj = create_module_object();

    add_function_to_object(os_obj.get(), "platform", NodeJS::os_platform);
    add_function_to_object(os_obj.get(), "arch", NodeJS::os_arch);
    add_function_to_object(os_obj.get(), "cpus", NodeJS::os_cpus);
    add_function_to_object(os_obj.get(), "hostname", NodeJS::os_hostname);
    add_function_to_object(os_obj.get(), "homedir", NodeJS::os_homedir);
    add_function_to_object(os_obj.get(), "tmpdir", NodeJS::os_tmpdir);

    engine.set_global_property("os", Value(os_obj.release()));
}

//=============================================================================
// Process API (originally lines 387-398)
//=============================================================================

void EngineNodeJS::setup_process_module(Engine& engine) {
    auto process_obj = create_module_object();

    add_function_to_object(process_obj.get(), "exit", NodeJS::process_exit);
    add_function_to_object(process_obj.get(), "cwd", NodeJS::process_cwd);
    add_function_to_object(process_obj.get(), "chdir", NodeJS::process_chdir);

    engine.set_global_property("process", Value(process_obj.release()));
}

//=============================================================================
// Crypto API (originally lines 400-409)
//=============================================================================

void EngineNodeJS::setup_crypto_module(Engine& engine) {
    auto crypto_obj = create_module_object();

    add_function_to_object(crypto_obj.get(), "randomBytes", NodeJS::crypto_randomBytes);
    add_function_to_object(crypto_obj.get(), "createHash", NodeJS::crypto_createHash);

    engine.set_global_property("crypto", Value(crypto_obj.release()));
}

//=============================================================================
// Helper Methods
//=============================================================================

std::unique_ptr<Object> EngineNodeJS::create_module_object() {
    return std::make_unique<Object>();
}

void EngineNodeJS::add_function_to_object(Object* obj, const std::string& name,
                                        Value(*func)(Context&, const std::vector<Value>&)) {
    if (!obj) return;

    auto native_func = ObjectFactory::create_native_function(name, func);
    obj->set_property(name, Value(native_func.release()));
}

} // namespace Quanta