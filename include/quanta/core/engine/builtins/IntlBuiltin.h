/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once
#include "quanta/core/engine/Context.h"

namespace Quanta {

void register_intl_builtins(Context& ctx);

// The methods of Number, BigInt, Date, String and Array that format "for the locale" go through Intl when there is one.
// Each says Unavailable when this build has no Intl (the caller then writes its own text), Done with the result, or Threw
// with an exception pending on the context.
namespace IntlHooks {

enum class Outcome { Unavailable, Done, Threw };

Outcome number_to_locale_string(Context& ctx, const Value& number, const Value& locales, const Value& options, std::string& out);
Outcome date_to_locale_string(Context& ctx, double time_value, const Value& locales, const Value& options, const char* required,
                              const char* defaults, std::string& out);
Outcome locale_compare(Context& ctx, const std::string& a, const std::string& b, const Value& locales, const Value& options,
                       double& out);
Outcome locale_case(Context& ctx, const std::string& text, bool upper, const Value& locales, std::string& out);

}  // namespace IntlHooks

} // namespace Quanta
