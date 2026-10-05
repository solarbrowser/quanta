/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/engine/TimerHost.h"

namespace Quanta {

namespace {
constinit thread_local int g_nesting_level = 0;
constinit thread_local int64_t g_next_timer_id = 1;
}

int timer_nesting_level() { return g_nesting_level; }
void set_timer_nesting_level(int level) { g_nesting_level = level; }
int64_t next_timer_id() { return g_next_timer_id++; }

}
