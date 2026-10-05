/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_ENGINE_TIMER_HOST_H
#define QUANTA_ENGINE_TIMER_HOST_H

#include "quanta/core/runtime/Value.h"
#include <cstdint>
#include <string>
#include <vector>

namespace Quanta {

class Context;
class Function;

// Who keeps the time. With a TimerHost set on its Engine, setTimeout, setInterval and setImmediate hand each
// request to it instead of the engine's own loop, and clearTimeout and friends tell it; it fires the timer when
// it sees fit (throttling, clamping, a task queue of its own).
class TimerHost {
public:
    virtual ~TimerHost() = default;

    struct Request {
        Context* ctx;                 // the realm's global context
        Function* callback;
        std::vector<Value> args;      // the arguments after the delay
        double delay_ms;
        bool repeating;
        int nesting_level;            // HTML "timer nesting level" this timer starts with
        int64_t id;                   // what script was given back, and what clearTimeout names
        const char* source;           // "setTimeout", "setInterval"
    };
    virtual void schedule(Request&& request) = 0;
    virtual void cancel(Context& ctx, int64_t id) = 0;
};

// The nesting level of the timer task that is running, 0 outside any: a timer made inside it starts one deeper.
int timer_nesting_level();
void set_timer_nesting_level(int level);
// Timer ids are unique across the realms of a thread.
int64_t next_timer_id();

}

#endif
