/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifdef QUANTA_ICU4X

#include "IntlSupport.h"

#include "icu4x/CaseMapper.hpp"
#include "icu4x/Locale.hpp"

namespace Quanta::IntlImpl {

// String.prototype.toLocaleLowerCase / toLocaleUpperCase: the first requested locale picks the special casing
// (Turkish dotted i, Lithuanian, ...).
bool locale_case(Context& ctx, const std::string& text, bool upper, const Value& locales, std::string& out) {
    std::vector<std::string> requested;
    if (!canonicalize_locale_list(ctx, locales, requested)) return false;
    std::string tag = requested.empty() ? IntlICU::default_locale() : requested[0];
    LocaleId id = IntlICU::split_tag(tag);
    // Only the language decides: the case rules have no use for the rest, and the mapper takes what it knows.
    std::unique_ptr<icu4x::Locale> locale = IntlICU::to_icu(id.language);
    if (!locale) locale = IntlICU::to_icu("und");
    auto mapper = icu4x::CaseMapper::create_with_provider(IntlICU::provider());
    if (!mapper.is_ok() || !locale) { out = text; return true; }
    std::unique_ptr<icu4x::CaseMapper> instance = std::move(mapper).ok().value();
    auto result = upper ? instance->uppercase(text, *locale) : instance->lowercase(text, *locale);
    out = result.is_ok() ? std::move(result).ok().value() : text;
    return true;
}

}  // namespace Quanta::IntlImpl

#endif  // QUANTA_ICU4X
