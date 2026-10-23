/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

// Intl.DateTimeFormat's object, shared with the Date methods that format through it. Compiled only with ICU4X.
#ifdef QUANTA_ICU4X

#include "IntlNumber.h"

namespace Quanta::IntlImpl {

struct DateTimePart {
    std::string type, value;
    std::string source;   // formatRangeToParts
};

struct DateTimeFormatObject : DOMObject {
    std::string locale, data_locale, icu_locale, calendar, numbering_system, time_zone, hour_cycle;
    std::string weekday, era, year, month, day, day_period, hour, minute, second, time_zone_name, date_style, time_style;
    int fractional_digits = 0;
    TracedValue bound_format;

    // The UTS 35 skeleton the options ask for.
    std::string skeleton() const;
    // The time value `t` written out, with its parts. False when the data cannot do it.
    bool format(double t, std::vector<DateTimePart>& parts) const;
};

// CreateDateTimeFormat after ToDateTimeOptions: `required` is "date", "time" or "any", `defaults` "date", "time" or "all".
// Null with an exception pending.
DateTimeFormatObject* create_date_time_format(Context& ctx, const Value& locales, const Value& options, const char* required,
                                              const char* defaults, Object* prototype);

}  // namespace Quanta::IntlImpl

#endif  // QUANTA_ICU4X
