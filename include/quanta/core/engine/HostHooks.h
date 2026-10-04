/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_ENGINE_HOST_HOOKS_H
#define QUANTA_ENGINE_HOST_HOOKS_H

#include "quanta/core/runtime/Value.h"
#include <functional>

namespace Quanta {

class Context;
class Promise;

// Where the engine tells its host about things that happen with no script left to tell:
// an exception nobody caught, a promise rejected with nobody to handle it. One set per
// thread, like the heap and the event loop they belong to. With none installed the engine
// behaves as the CLI always has: it prints and goes on.
struct HostHooks {
    // `origin` says where the exception surfaced: "timer", "queueMicrotask",
    // "FinalizationRegistry cleanup". `exception` is whatever was thrown.
    using UncaughtFn = std::function<void(Context& ctx, const Value& exception, const char* origin)>;
    // HostPromiseRejectionTracker: `handled` false when a promise is rejected with no handler,
    // true when one is attached to such a promise afterwards.
    using RejectionFn = std::function<void(Promise* promise, bool handled)>;

    static void set_uncaught_handler(UncaughtFn fn);
    static void set_rejection_tracker(RejectionFn fn);

    // Hands the exception to the handler, or prints it as "Uncaught (in <origin>) ..." when
    // there is none.
    static void report_uncaught(Context& ctx, const Value& exception, const char* origin);
    static bool has_rejection_tracker();
    static void note_rejection(Promise* promise, bool handled);

    // How many scripts are running (the top level of a script, an eval, a module). With the
    // call stack, what the spec's "JavaScript execution context stack is empty" asks about.
    struct ScriptEntry {
        ScriptEntry();
        ~ScriptEntry();
        ScriptEntry(const ScriptEntry&) = delete;
        ScriptEntry& operator=(const ScriptEntry&) = delete;
    };
    static bool js_stack_empty();
};

}

#endif
