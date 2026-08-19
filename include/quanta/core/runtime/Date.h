/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_DATE_H
#define QUANTA_DATE_H

#include "quanta/core/runtime/Value.h"
#include <span>
#include "quanta/core/engine/Context.h"
#include <string>
#include <vector>

namespace Quanta {

class Date {
public:
    static Value date_constructor(Context& ctx, std::span<const Value> args);

    static Value now(Context& ctx, std::span<const Value> args);
    static Value parse(Context& ctx, std::span<const Value> args);
    static Value UTC(Context& ctx, std::span<const Value> args);

    static Value getTime(Context& ctx, std::span<const Value> args);
    static Value getFullYear(Context& ctx, std::span<const Value> args);
    static Value getMonth(Context& ctx, std::span<const Value> args);
    static Value getDate(Context& ctx, std::span<const Value> args);
    static Value getDay(Context& ctx, std::span<const Value> args);
    static Value getHours(Context& ctx, std::span<const Value> args);
    static Value getMinutes(Context& ctx, std::span<const Value> args);
    static Value getSeconds(Context& ctx, std::span<const Value> args);
    static Value getMilliseconds(Context& ctx, std::span<const Value> args);
    static Value getTimezoneOffset(Context& ctx, std::span<const Value> args);
    static Value getYear(Context& ctx, std::span<const Value> args);

    static Value getUTCFullYear(Context& ctx, std::span<const Value> args);
    static Value getUTCMonth(Context& ctx, std::span<const Value> args);
    static Value getUTCDate(Context& ctx, std::span<const Value> args);
    static Value getUTCDay(Context& ctx, std::span<const Value> args);
    static Value getUTCHours(Context& ctx, std::span<const Value> args);
    static Value getUTCMinutes(Context& ctx, std::span<const Value> args);
    static Value getUTCSeconds(Context& ctx, std::span<const Value> args);
    static Value getUTCMilliseconds(Context& ctx, std::span<const Value> args);

    static Value setTime(Context& ctx, std::span<const Value> args);
    static Value setFullYear(Context& ctx, std::span<const Value> args);
    static Value setMonth(Context& ctx, std::span<const Value> args);
    static Value setDate(Context& ctx, std::span<const Value> args);
    static Value setHours(Context& ctx, std::span<const Value> args);
    static Value setMinutes(Context& ctx, std::span<const Value> args);
    static Value setSeconds(Context& ctx, std::span<const Value> args);
    static Value setMilliseconds(Context& ctx, std::span<const Value> args);
    static Value setYear(Context& ctx, std::span<const Value> args);

    static Value setUTCFullYear(Context& ctx, std::span<const Value> args);
    static Value setUTCMonth(Context& ctx, std::span<const Value> args);
    static Value setUTCDate(Context& ctx, std::span<const Value> args);
    static Value setUTCHours(Context& ctx, std::span<const Value> args);
    static Value setUTCMinutes(Context& ctx, std::span<const Value> args);
    static Value setUTCSeconds(Context& ctx, std::span<const Value> args);
    static Value setUTCMilliseconds(Context& ctx, std::span<const Value> args);

    static Value toString(Context& ctx, std::span<const Value> args);
    static Value toDateString(Context& ctx, std::span<const Value> args);
    static Value toTimeString(Context& ctx, std::span<const Value> args);
    static Value toISOString(Context& ctx, std::span<const Value> args);
    static Value toUTCString(Context& ctx, std::span<const Value> args);
    static Value toGMTString(Context& ctx, std::span<const Value> args);
    static Value toJSON(Context& ctx, std::span<const Value> args);
    static Value toLocaleString(Context& ctx, std::span<const Value> args);
    static Value toLocaleDateString(Context& ctx, std::span<const Value> args);
    static Value toLocaleTimeString(Context& ctx, std::span<const Value> args);
    static Value valueOf(Context& ctx, std::span<const Value> args);
    static Value symbol_to_primitive(Context& ctx, std::span<const Value> args);

    static double current_time_ms();
    static std::string to_date_string(double tv);
};

}

#endif
