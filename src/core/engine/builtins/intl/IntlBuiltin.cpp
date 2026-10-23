/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/engine/builtins/IntlBuiltin.h"
#include <span>
#include "quanta/core/engine/Context.h"
#include "quanta/core/engine/Engine.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/parser/AST.h"
#ifdef QUANTA_ICU4X
#include "IntlSupport.h"
#endif

namespace Quanta {

void register_intl_builtins(Context& ctx) {
#ifdef QUANTA_ICU4X
    if (IntlICU::data_present()) {
        auto real_intl = ObjectFactory::create_object();
        IntlImpl::register_all(ctx, real_intl.get());
        ctx.register_built_in_object("Intl", real_intl.release());
        return;
    }
#endif
    (void)ctx;   // no ICU4X data, no Intl: a stand-in would answer wrongly
}

namespace IntlHooks {

#ifdef QUANTA_ICU4X
namespace {
Outcome run(Context& ctx, bool ok) { return ok ? Outcome::Done : (ctx.has_exception() ? Outcome::Threw : Outcome::Unavailable); }
}  // namespace
#endif

Outcome number_to_locale_string(Context& ctx, const Value& number, const Value& locales, const Value& options, std::string& out) {
#ifdef QUANTA_ICU4X
    if (IntlICU::data_present()) return run(ctx, IntlImpl::number_to_locale_string(ctx, number, locales, options, out));
#endif
    (void)ctx; (void)number; (void)locales; (void)options; (void)out;
    return Outcome::Unavailable;
}

Outcome date_to_locale_string(Context& ctx, double time_value, const Value& locales, const Value& options, const char* required,
                              const char* defaults, std::string& out) {
#ifdef QUANTA_ICU4X
    if (IntlICU::data_present()) return run(ctx, IntlImpl::date_to_locale_string(ctx, time_value, locales, options, required, defaults, out));
#endif
    (void)ctx; (void)time_value; (void)locales; (void)options; (void)required; (void)defaults; (void)out;
    return Outcome::Unavailable;
}

Outcome locale_compare(Context& ctx, const std::string& a, const std::string& b, const Value& locales, const Value& options,
                       double& out) {
#ifdef QUANTA_ICU4X
    if (IntlICU::data_present()) return run(ctx, IntlImpl::locale_compare(ctx, a, b, locales, options, out));
#endif
    (void)ctx; (void)a; (void)b; (void)locales; (void)options; (void)out;
    return Outcome::Unavailable;
}

Outcome locale_case(Context& ctx, const std::string& text, bool upper, const Value& locales, std::string& out) {
#ifdef QUANTA_ICU4X
    if (IntlICU::data_present()) return run(ctx, IntlImpl::locale_case(ctx, text, upper, locales, out));
#endif
    (void)ctx; (void)text; (void)upper; (void)locales; (void)out;
    return Outcome::Unavailable;
}

}  // namespace IntlHooks

} // namespace Quanta
