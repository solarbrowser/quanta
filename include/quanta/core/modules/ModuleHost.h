/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_MODULES_MODULE_HOST_H
#define QUANTA_MODULES_MODULE_HOST_H

#include "quanta/core/runtime/Value.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Quanta {

class Context;
class Object;
class Promise;

// What a host answers a module fetch with.
struct ModuleContent {
    enum class Kind {
        Script,    // source text of an ES module
        Json,      // text the engine parses; the default export is the result
        Text,      // text; the default export is that string
        Bytes,     // an immutable Uint8Array over these bytes is the default export
        Default,   // a value the host made (a CSSStyleSheet, say): the default export as it is
        Failure    // the fetch failed
    };
    Kind kind = Kind::Failure;
    std::string text;               // Script, Json, Text
    std::vector<uint8_t> bytes;     // Bytes
    Value value;                    // Default
    std::string error;              // Failure: the message of the TypeError the import rejects with
    Value error_value;              // Failure: or the exact thing to reject with, when the host has one
};

// How a realm's modules reach the outside world: the places the HTML and ECMAScript module
// machinery calls the host. With none set a realm loads modules from the file system, as the CLI does.
// The engine owns the module map and the graph walk; the host only names, fetches and decorates.
class ModuleHost {
public:
    virtual ~ModuleHost() = default;

    // HostResolveImportedModuleSpecifier: an absolute URL for `specifier` as written in the module
    // at `referrer` (empty for a request that has no referrer), or false with the message of the
    // TypeError the import fails with. Bare specifiers, import maps, data: and blob: are the host's.
    virtual bool resolve(Context& ctx, const std::string& specifier, const std::string& referrer,
                         std::string& resolved, std::string& error) = 0;

    // HostLoadImportedModule's fetch: the content of `url` for an import with `type` (empty, "json",
    // "css", ...). `done` may be called at once or later, from the thread the realm belongs to; the
    // engine continues the graph walk inside it.
    using Completion = std::function<void(ModuleContent)>;
    virtual void fetch(Context& ctx, const std::string& url, const std::string& type, Completion done) = 0;

    // HostGetImportMetaProperties / HostFinalizeImportMeta: `meta` is a fresh null-prototype object for
    // the module at `url`.
    virtual void init_import_meta(Context& ctx, Object* meta, const std::string& url) = 0;

    // `import()` evaluated in the module or script at `referrer`. True when the host took the request and
    // will settle `result` itself; false to let the engine do it with resolve and fetch.
    virtual bool dynamic_import(Context& ctx, const std::string& specifier, const std::string& referrer,
                                const std::string& type, Promise* result) = 0;
};

}

#endif
