/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_DATE_H
#define QUANTA_DATE_H

#include "Value.h"
#include "Context.h"
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace Quanta {

/**
 * JavaScript Date object implementation
 * Provides date and time functionality
 */
class Date {
private:
    std::chrono::system_clock::time_point time_point_;
    bool is_invalid_;  // True if date is invalid (NaN timestamp)
    
public:
    // Constructors
    Date(); // Current time
    Date(int64_t timestamp); // From timestamp
    Date(int year, int month, int day = 1, int hour = 0, int minute = 0, int second = 0, int millisecond = 0);
    
    // Static methods
    static Value now(Context& ctx, const std::vector<Value>& args);
    static Value parse(Context& ctx, const std::vector<Value>& args);
    static Value UTC(Context& ctx, const std::vector<Value>& args);
    
    // Constructor function for JavaScript
    static Value date_constructor(Context& ctx, const std::vector<Value>& args);
    
    // Instance methods
    static Value getTime(Context& ctx, const std::vector<Value>& args);
    static Value getFullYear(Context& ctx, const std::vector<Value>& args);
    static Value getMonth(Context& ctx, const std::vector<Value>& args);
    static Value getDate(Context& ctx, const std::vector<Value>& args);
    static Value getDay(Context& ctx, const std::vector<Value>& args);
    static Value getHours(Context& ctx, const std::vector<Value>& args);
    static Value getMinutes(Context& ctx, const std::vector<Value>& args);
    static Value getSeconds(Context& ctx, const std::vector<Value>& args);
    static Value getMilliseconds(Context& ctx, const std::vector<Value>& args);
    
    // Setters
    static Value setTime(Context& ctx, const std::vector<Value>& args);
    static Value setFullYear(Context& ctx, const std::vector<Value>& args);
    static Value setMonth(Context& ctx, const std::vector<Value>& args);
    static Value setDate(Context& ctx, const std::vector<Value>& args);
    static Value setHours(Context& ctx, const std::vector<Value>& args);
    static Value setMinutes(Context& ctx, const std::vector<Value>& args);
    static Value setSeconds(Context& ctx, const std::vector<Value>& args);
    static Value setMilliseconds(Context& ctx, const std::vector<Value>& args);

    // Legacy methods (Annex B)
    static Value getYear(Context& ctx, const std::vector<Value>& args);
    static Value setYear(Context& ctx, const std::vector<Value>& args);

    // Timezone
    static Value getTimezoneOffset(Context& ctx, const std::vector<Value>& args);

    // UTC methods
    static Value getUTCDate(Context& ctx, const std::vector<Value>& args);
    static Value getUTCDay(Context& ctx, const std::vector<Value>& args);
    static Value getUTCFullYear(Context& ctx, const std::vector<Value>& args);
    static Value getUTCHours(Context& ctx, const std::vector<Value>& args);
    static Value getUTCMilliseconds(Context& ctx, const std::vector<Value>& args);
    static Value getUTCMinutes(Context& ctx, const std::vector<Value>& args);
    static Value getUTCMonth(Context& ctx, const std::vector<Value>& args);
    static Value getUTCSeconds(Context& ctx, const std::vector<Value>& args);

    // UTC setters
    static Value setUTCFullYear(Context& ctx, const std::vector<Value>& args);
    static Value setUTCMonth(Context& ctx, const std::vector<Value>& args);
    static Value setUTCDate(Context& ctx, const std::vector<Value>& args);
    static Value setUTCHours(Context& ctx, const std::vector<Value>& args);
    static Value setUTCMinutes(Context& ctx, const std::vector<Value>& args);
    static Value setUTCSeconds(Context& ctx, const std::vector<Value>& args);
    static Value setUTCMilliseconds(Context& ctx, const std::vector<Value>& args);

    // String methods
    static Value toString(Context& ctx, const std::vector<Value>& args);
    static Value toISOString(Context& ctx, const std::vector<Value>& args);
    static Value toJSON(Context& ctx, const std::vector<Value>& args);
    
    // Utility methods
    double getTimestamp() const;  // Returns NaN for invalid dates
    std::time_t getTimeT() const;
    std::tm getLocalTime() const;
    std::tm getUTCTime() const;
};

} // namespace Quanta

#endif // QUANTA_DATE_H