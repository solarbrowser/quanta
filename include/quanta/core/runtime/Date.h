/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_DATE_H
#define QUANTA_DATE_H

#include "quanta/core/runtime/Value.h"
#include "quanta/core/engine/Context.h"
#include <string>
#include <vector>

namespace Quanta {

class Date {
public:
    static Value date_constructor(Context& ctx, const std::vector<Value>& args);

    static Value now(Context& ctx, const std::vector<Value>& args);
    static Value parse(Context& ctx, const std::vector<Value>& args);
    static Value UTC(Context& ctx, const std::vector<Value>& args);

    static Value getTime(Context& ctx, const std::vector<Value>& args);
    static Value getFullYear(Context& ctx, const std::vector<Value>& args);
    static Value getMonth(Context& ctx, const std::vector<Value>& args);
    static Value getDate(Context& ctx, const std::vector<Value>& args);
    static Value getDay(Context& ctx, const std::vector<Value>& args);
    static Value getHours(Context& ctx, const std::vector<Value>& args);
    static Value getMinutes(Context& ctx, const std::vector<Value>& args);
    static Value getSeconds(Context& ctx, const std::vector<Value>& args);
    static Value getMilliseconds(Context& ctx, const std::vector<Value>& args);
    static Value getTimezoneOffset(Context& ctx, const std::vector<Value>& args);
    static Value getYear(Context& ctx, const std::vector<Value>& args);

    static Value getUTCFullYear(Context& ctx, const std::vector<Value>& args);
    static Value getUTCMonth(Context& ctx, const std::vector<Value>& args);
    static Value getUTCDate(Context& ctx, const std::vector<Value>& args);
    static Value getUTCDay(Context& ctx, const std::vector<Value>& args);
    static Value getUTCHours(Context& ctx, const std::vector<Value>& args);
    static Value getUTCMinutes(Context& ctx, const std::vector<Value>& args);
    static Value getUTCSeconds(Context& ctx, const std::vector<Value>& args);
    static Value getUTCMilliseconds(Context& ctx, const std::vector<Value>& args);

    static Value setTime(Context& ctx, const std::vector<Value>& args);
    static Value setFullYear(Context& ctx, const std::vector<Value>& args);
    static Value setMonth(Context& ctx, const std::vector<Value>& args);
    static Value setDate(Context& ctx, const std::vector<Value>& args);
    static Value setHours(Context& ctx, const std::vector<Value>& args);
    static Value setMinutes(Context& ctx, const std::vector<Value>& args);
    static Value setSeconds(Context& ctx, const std::vector<Value>& args);
    static Value setMilliseconds(Context& ctx, const std::vector<Value>& args);
    static Value setYear(Context& ctx, const std::vector<Value>& args);

    static Value setUTCFullYear(Context& ctx, const std::vector<Value>& args);
    static Value setUTCMonth(Context& ctx, const std::vector<Value>& args);
    static Value setUTCDate(Context& ctx, const std::vector<Value>& args);
    static Value setUTCHours(Context& ctx, const std::vector<Value>& args);
    static Value setUTCMinutes(Context& ctx, const std::vector<Value>& args);
    static Value setUTCSeconds(Context& ctx, const std::vector<Value>& args);
    static Value setUTCMilliseconds(Context& ctx, const std::vector<Value>& args);

    static Value toString(Context& ctx, const std::vector<Value>& args);
    static Value toDateString(Context& ctx, const std::vector<Value>& args);
    static Value toTimeString(Context& ctx, const std::vector<Value>& args);
    static Value toISOString(Context& ctx, const std::vector<Value>& args);
    static Value toUTCString(Context& ctx, const std::vector<Value>& args);
    static Value toGMTString(Context& ctx, const std::vector<Value>& args);
    static Value toJSON(Context& ctx, const std::vector<Value>& args);
    static Value toLocaleString(Context& ctx, const std::vector<Value>& args);
    static Value toLocaleDateString(Context& ctx, const std::vector<Value>& args);
    static Value toLocaleTimeString(Context& ctx, const std::vector<Value>& args);
    static Value valueOf(Context& ctx, const std::vector<Value>& args);
    static Value symbol_to_primitive(Context& ctx, const std::vector<Value>& args);

    static double current_time_ms();
    static std::string to_date_string(double tv);
};

}

#endif
