/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "../../include/Engine.h"
#include "../../include/Object.h"
#include <memory>

namespace Quanta {

/**
 * Engine NodeJS APIs - Node.js compatibility layer
 * EXTRACTED FROM Engine.cpp - Node.js APIs setup (lines 307-414)
 */
class EngineNodeJS {
public:
    // Main setup method
    static void setup_nodejs_apis(Engine& engine);

private:
    // Individual module setup
    static void setup_fs_module(Engine& engine);
    static void setup_path_module(Engine& engine);
    static void setup_os_module(Engine& engine);
    static void setup_process_module(Engine& engine);
    static void setup_crypto_module(Engine& engine);

    // Helper methods
    static std::unique_ptr<Object> create_module_object();
    static void add_function_to_object(Object* obj, const std::string& name,
                                     Value(*func)(Context&, const std::vector<Value>&));
};

} // namespace Quanta