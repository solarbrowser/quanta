//! C functions for the formatters icu_capi does not expose. Each takes the locale and the number as text and writes its
//! result into the caller's buffer; the return value is the length of the result, or negative on failure. The data is the
//! same blob the rest of Intl reads: `quanta_icu_init` is given the mapped bytes once.

use core::ffi::{c_char, CStr};
use std::sync::{Mutex, OnceLock};

use fixed_decimal::Decimal;
use icu_locale::LocaleFallbacker;
use icu_locale_core::Locale;
use icu_provider_adapters::fallback::LocaleFallbackProvider;
use icu_provider_blob::BlobDataProvider;
use writeable::Writeable;

// The blob provider counts its owners with an Rc, so it is not Sync by itself; every call takes the lock.
struct Shared(LocaleFallbackProvider<BlobDataProvider>);
unsafe impl Send for Shared {}
static PROVIDER: OnceLock<Mutex<Shared>> = OnceLock::new();

/// Runs `f` on the provider; None when `quanta_icu_init` has not succeeded.
fn with_provider<R>(f: impl FnOnce(&LocaleFallbackProvider<BlobDataProvider>) -> R) -> Option<R> {
    let guard = PROVIDER.get()?.lock().ok()?;
    Some(f(&guard.0))
}

/// # Safety
/// `data` must stay valid and unchanged for the rest of the process.
#[no_mangle]
pub unsafe extern "C" fn quanta_icu_init(data: *const u8, len: usize) -> bool {
    if PROVIDER.get().is_some() {
        return true;
    }
    let bytes: &'static [u8] = core::slice::from_raw_parts(data, len);
    let Ok(blob) = BlobDataProvider::try_new_from_static_blob(bytes) else { return false };
    let Ok(fallbacker) = LocaleFallbacker::try_new_with_buffer_provider(&blob) else { return false };
    let _ = PROVIDER.set(Mutex::new(Shared(LocaleFallbackProvider::new(blob, fallbacker))));
    true
}

unsafe fn text<'a>(p: *const c_char) -> Option<&'a str> {
    if p.is_null() {
        return None;
    }
    CStr::from_ptr(p).to_str().ok()
}

fn parse_locale(s: &str) -> Option<Locale> {
    s.parse().ok()
}

fn parse_decimal(s: &str) -> Option<Decimal> {
    Decimal::try_from_str(s).ok()
}

/// Copies `s` into `out` when it fits; returns its length either way.
unsafe fn emit(s: &str, out: *mut u8, cap: usize) -> isize {
    if s.len() <= cap {
        core::ptr::copy_nonoverlapping(s.as_ptr(), out, s.len());
    }
    s.len() as isize
}

unsafe fn emit_writeable(w: &impl Writeable, out: *mut u8, cap: usize) -> isize {
    emit(&w.write_to_string(), out, cap)
}

/// Percent: `decimal` is the number as written ("12.5"); the result has the percent sign where the locale puts it.
///
/// # Safety
/// All pointers must be valid; `out` must have room for `cap` bytes.
#[no_mangle]
pub unsafe extern "C" fn quanta_icu_format_percent(
    locale: *const c_char,
    decimal: *const c_char,
    out: *mut u8,
    cap: usize,
) -> isize {
    use icu_experimental::dimension::percent::formatter::PercentFormatter;
    let (Some(locale), Some(decimal)) = (text(locale).and_then(parse_locale), text(decimal).and_then(parse_decimal)) else { return -1 };
    with_provider(|provider| {
        let Ok(formatter) = PercentFormatter::try_new_with_buffer_provider(provider, (&locale).into(), Default::default()) else { return -2 };
        let formatted = formatter.format(&decimal);
        emit_writeable(&formatted, out, cap)
    })
    .unwrap_or(-1)
}

/// Currency. `width`: 0 symbol, 1 narrow symbol, 2 code, 3 name. `accounting` picks the accounting pattern.
///
/// # Safety
/// As for `quanta_icu_format_percent`; `code` is the three letter currency code.
#[no_mangle]
pub unsafe extern "C" fn quanta_icu_format_currency(
    locale: *const c_char,
    decimal: *const c_char,
    code: *const c_char,
    width: u8,
    accounting: bool,
    out: *mut u8,
    cap: usize,
) -> isize {
    use icu_experimental::dimension::currency::formatter::CurrencyFormatter;
    use icu_experimental::dimension::currency::options::{CurrencyFormatterOptions, CurrencyUsage};
    use icu_locale_core::preferences::extensions::unicode::keywords::CurrencyType;
    let (Some(locale), Some(decimal), Some(code)) = (text(locale).and_then(parse_locale), text(decimal).and_then(parse_decimal), text(code)) else { return -1 };
    let Ok(currency) = CurrencyType::try_from_str(code) else { return -3 };
    let options = CurrencyFormatterOptions::from(if accounting { CurrencyUsage::Accounting } else { CurrencyUsage::Standard });
    with_provider(|provider| {
        let prefs = (&locale).into();
        let formatter = match width {
            0 => CurrencyFormatter::try_new_symbol_with_buffer_provider(provider, prefs, currency, options),
            1 => CurrencyFormatter::try_new_symbol_narrow_with_buffer_provider(provider, prefs, currency, options),
            2 => CurrencyFormatter::try_new_code_with_buffer_provider(provider, prefs, currency, options),
            _ => CurrencyFormatter::try_new_name_with_buffer_provider(provider, prefs, currency),
        };
        let Ok(formatter) = formatter else { return -2 };
        let formatted = formatter.format_fixed_decimal(&decimal);
        emit_writeable(&formatted, out, cap)
    })
    .unwrap_or(-1)
}

/// Compact notation: the number scaled and given its locale's suffix. `long` picks the long names.
///
/// # Safety
/// As for `quanta_icu_format_percent`.
#[no_mangle]
pub unsafe extern "C" fn quanta_icu_format_compact(
    locale: *const c_char,
    decimal: *const c_char,
    long: bool,
    out: *mut u8,
    cap: usize,
) -> isize {
    use icu_decimal::CompactDecimalFormatter;
    let (Some(locale), Some(decimal)) = (text(locale).and_then(parse_locale), text(decimal).and_then(parse_decimal)) else { return -1 };
    with_provider(|provider| {
        let prefs = (&locale).into();
        let formatter = if long {
            CompactDecimalFormatter::try_new_long_with_buffer_provider(provider, prefs, Default::default())
        } else {
            CompactDecimalFormatter::try_new_short_with_buffer_provider(provider, prefs, Default::default())
        };
        let Ok(formatter) = formatter else { return -2 };
        let formatted = formatter.format(&decimal);
        emit_writeable(&formatted, out, cap)
    })
    .unwrap_or(-1)
}

/// Relative time ("in 3 days", "yesterday"). `unit`: 0 second, 1 minute, 2 hour, 3 day, 4 week, 5 month, 6 quarter,
/// 7 year; `style`: 0 long, 1 short, 2 narrow; `auto` picks the words ("tomorrow") over the numbers where there are some.
/// `decimal` is signed: a negative number is in the past.
///
/// # Safety
/// As for `quanta_icu_format_percent`.
#[no_mangle]
pub unsafe extern "C" fn quanta_icu_format_relative(
    locale: *const c_char,
    decimal: *const c_char,
    unit: u8,
    style: u8,
    auto: bool,
    out: *mut u8,
    cap: usize,
) -> isize {
    use icu_experimental::relativetime::{options::Numeric, RelativeTimeFormatter, RelativeTimeFormatterOptions};
    let (Some(locale), Some(decimal)) = (text(locale).and_then(parse_locale), text(decimal).and_then(parse_decimal)) else { return -1 };
    let options = RelativeTimeFormatterOptions::default().with_numeric(if auto { Numeric::Auto } else { Numeric::Always });
    with_provider(|provider| {
        let prefs = (&locale).into();
        macro_rules! pick {
            ($($style:literal, $unit:literal => $ctor:ident),* $(,)?) => {
                match (style, unit) {
                    $(($style, $unit) => RelativeTimeFormatter::$ctor(provider, prefs, options),)*
                    _ => return -3,
                }
            };
        }
        let formatter = pick!(
            0, 0 => try_new_long_second_with_buffer_provider, 0, 1 => try_new_long_minute_with_buffer_provider,
            0, 2 => try_new_long_hour_with_buffer_provider, 0, 3 => try_new_long_day_with_buffer_provider,
            0, 4 => try_new_long_week_with_buffer_provider, 0, 5 => try_new_long_month_with_buffer_provider,
            0, 6 => try_new_long_quarter_with_buffer_provider, 0, 7 => try_new_long_year_with_buffer_provider,
            1, 0 => try_new_short_second_with_buffer_provider, 1, 1 => try_new_short_minute_with_buffer_provider,
            1, 2 => try_new_short_hour_with_buffer_provider, 1, 3 => try_new_short_day_with_buffer_provider,
            1, 4 => try_new_short_week_with_buffer_provider, 1, 5 => try_new_short_month_with_buffer_provider,
            1, 6 => try_new_short_quarter_with_buffer_provider, 1, 7 => try_new_short_year_with_buffer_provider,
            2, 0 => try_new_narrow_second_with_buffer_provider, 2, 1 => try_new_narrow_minute_with_buffer_provider,
            2, 2 => try_new_narrow_hour_with_buffer_provider, 2, 3 => try_new_narrow_day_with_buffer_provider,
            2, 4 => try_new_narrow_week_with_buffer_provider, 2, 5 => try_new_narrow_month_with_buffer_provider,
            2, 6 => try_new_narrow_quarter_with_buffer_provider, 2, 7 => try_new_narrow_year_with_buffer_provider,
        );
        let Ok(formatter) = formatter else { return -2 };
        let formatted = formatter.format(decimal);
        emit_writeable(&formatted, out, cap)
    })
    .unwrap_or(-1)
}

/// A simple unit with the number written into its locale pattern ("{0} km" with "987" in it). `number` is the already
/// formatted number, `decimal` the same value as a plain decimal (it picks the plural form), `unit` a CLDR unit such as
/// "kilometer" or "kilometer-per-hour" and `width` 0 long, 1 short, 2 narrow. The units the data has names for are the
/// length, mass, volume, area and duration ones; for the rest this fails and the caller has its own.
///
/// # Safety
/// As for `quanta_icu_format_percent`.
#[no_mangle]
pub unsafe extern "C" fn quanta_icu_format_unit(
    locale: *const c_char,
    number: *const c_char,
    decimal: *const c_char,
    unit: *const c_char,
    width: u8,
    out: *mut u8,
    cap: usize,
) -> isize {
    use icu_experimental::dimension::provider::units::categorized_display_names::*;
    use icu_experimental::dimension::provider::units::display_names::UnitsDisplayNames;
    use icu_experimental::dimension::provider::units::essentials::UnitsEssentialsV1;
    use icu_plurals::PluralRules;
    use icu_provider::prelude::*;
    let (Some(locale), Some(number), Some(decimal), Some(unit)) =
        (text(locale).and_then(parse_locale), text(number), text(decimal).and_then(parse_decimal), text(unit))
    else {
        return -1;
    };
    let width_name = ["long", "short", "narrow"][(width as usize).min(2)];
    let data_locale = DataLocale::from(&locale);

    // The plural form first, because every lookup below needs it.
    let rules = with_provider(|provider| PluralRules::try_new_cardinal_with_buffer_provider(provider, (&locale).into()).ok()).flatten();
    let Some(rules) = rules else { return -2 };

    fn pattern_for(
        provider: &icu_provider_adapters::fallback::LocaleFallbackProvider<BlobDataProvider>,
        data_locale: &DataLocale,
        rules: &PluralRules,
        decimal: &Decimal,
        attribute: &str,
        number: &str,
    ) -> Option<String> {
        use icu_provider::prelude::*;
        let provider = provider.as_deserializing();
        let attributes = DataMarkerAttributes::try_from_str(attribute).ok()?;
        macro_rules! try_marker {
            ($($marker:ty),* $(,)?) => {$(
                if let Ok(response) = DataProvider::<$marker>::load(&provider, DataRequest {
                    id: DataIdentifierBorrowed::for_marker_attributes_and_locale(attributes, data_locale),
                    ..Default::default()
                }) {
                    let names: &UnitsDisplayNames = response.payload.get();
                    let mut result = String::new();
                    use writeable::Writeable;
                    let pattern = names.get(decimal.into(), rules).interpolate((number,));
                    pattern.write_to(&mut result).ok()?;
                    return Some(result);
                }
            )*};
        }
        try_marker!(
            UnitsNamesLengthCoreV1, UnitsNamesLengthExtendedV1, UnitsNamesLengthOutlierV1,
            UnitsNamesMassCoreV1, UnitsNamesMassExtendedV1, UnitsNamesMassOutlierV1,
            UnitsNamesVolumeCoreV1, UnitsNamesVolumeExtendedV1, UnitsNamesVolumeOutlierV1,
            UnitsNamesAreaCoreV1, UnitsNamesAreaExtendedV1, UnitsNamesAreaOutlierV1,
            UnitsNamesDurationCoreV1, UnitsNamesDurationExtendedV1, UnitsNamesDurationOutlierV1,
        );
        None
    }

    let result = with_provider(|provider| {
        if let Some((numerator, denominator)) = unit.split_once("-per-") {
            let top = pattern_for(provider, &data_locale, &rules, &decimal, &format!("{width_name}-{numerator}"), number)?;
            // The denominator is the unit's name without a number: its pattern with the placeholder taken out.
            let bottom = pattern_for(provider, &data_locale, &rules, &Decimal::from(1), &format!("{width_name}-{denominator}"), "")?;
            let bottom = bottom.trim().to_string();
            let per = {
                let essentials = DataProvider::<UnitsEssentialsV1>::load(&provider.as_deserializing(), DataRequest {
                    id: DataIdentifierBorrowed::for_marker_attributes_and_locale(
                        DataMarkerAttributes::try_from_str(width_name).ok()?,
                        &data_locale,
                    ),
                    ..Default::default()
                })
                .ok()?;
                essentials.payload.get().per.to_string()
            };
            Some(per.replace("{0}", &top).replace("{1}", &bottom))
        } else {
            pattern_for(provider, &data_locale, &rules, &decimal, &format!("{width_name}-{unit}"), number)
        }
    })
    .flatten();
    match result {
        Some(s) => emit(&s, out, cap),
        None => -3,
    }
}

/// The name of a script in a locale ("Latin"); `short` asks for the short form where there is one.
///
/// # Safety
/// As for `quanta_icu_format_percent`.
#[no_mangle]
pub unsafe extern "C" fn quanta_icu_display_script(
    locale: *const c_char,
    script: *const c_char,
    short: bool,
    out: *mut u8,
    cap: usize,
) -> isize {
    use icu_experimental::displaynames::multi::ScriptDisplayNames;
    use icu_experimental::displaynames::{DisplayNamesOptions, Style};
    use icu_locale_core::subtags::Script;
    let (Some(locale), Some(script)) = (text(locale).and_then(parse_locale), text(script).and_then(|s| s.parse::<Script>().ok())) else { return -1 };
    with_provider(|provider| {
        let mut options = DisplayNamesOptions::default();
        if short {
            options.style = Some(Style::Short);
        }
        let Ok(names) = ScriptDisplayNames::try_new_with_buffer_provider(provider, (&locale).into(), options) else { return -2 };
        match names.of(script) {
            Some(name) => emit(name, out, cap),
            None => -3,
        }
    })
    .unwrap_or(-1)
}

/// The name of a currency in a locale ("US Dollar").
///
/// # Safety
/// As for `quanta_icu_format_percent`.
#[no_mangle]
pub unsafe extern "C" fn quanta_icu_display_currency(
    locale: *const c_char,
    code: *const c_char,
    out: *mut u8,
    cap: usize,
) -> isize {
    use icu_experimental::dimension::provider::currency::displayname::CurrencyDisplaynameV1;
    use icu_provider::prelude::*;
    let (Some(locale), Some(code)) = (text(locale).and_then(parse_locale), text(code)) else { return -1 };
    let data_locale = DataLocale::from(&locale);
    with_provider(|provider| {
        let provider = provider.as_deserializing();
        for candidate in [code.to_string(), code.to_ascii_lowercase()] {
            let Ok(attributes) = DataMarkerAttributes::try_from_str(&candidate) else { continue };
            if let Ok(response) = DataProvider::<CurrencyDisplaynameV1>::load(&provider, DataRequest {
                id: DataIdentifierBorrowed::for_marker_attributes_and_locale(attributes, &data_locale),
                ..Default::default()
            }) {
                let name: &str = response.payload.get();
                return emit(name, out, cap);
            }
        }
        -3
    })
    .unwrap_or(-1)
}

struct PartsCollector {
    text: String,
    parts: Vec<(&'static str, usize, usize)>,
}

impl core::fmt::Write for PartsCollector {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        self.text.push_str(s);
        Ok(())
    }
}

impl writeable::PartsWrite for PartsCollector {
    type SubPartsWrite = Self;
    fn with_part(&mut self, part: writeable::Part, mut f: impl FnMut(&mut Self::SubPartsWrite) -> core::fmt::Result) -> core::fmt::Result {
        let start = self.text.len();
        f(self)?;
        self.parts.push((part.value, start, self.text.len()));
        Ok(())
    }
}

/// Writes the text, a NUL, then the parts as `name,start,end;` triples (byte offsets into the text).
unsafe fn emit_with_parts(c: &PartsCollector, out: *mut u8, cap: usize) -> isize {
    let mut s = c.text.clone();
    s.push('\0');
    for (name, start, end) in &c.parts {
        s.push_str(&format!("{name},{start},{end};"));
    }
    emit(&s, out, cap)
}

/// A date and time (ISO calendar, no zone) written in a locale from a UTS 35 skeleton ("yMMMd", "jm"), with the parts.
/// The locale's `-u-ca-`, `-u-hc-` and `-u-nu-` say which calendar, hour cycle and digits. `zone` is an IANA id (or
/// empty) and `offset_seconds` the offset from UTC at that moment.
///
/// # Safety
/// As for `quanta_icu_format_percent`.
#[no_mangle]
pub unsafe extern "C" fn quanta_icu_format_datetime(
    locale: *const c_char,
    skeleton: *const c_char,
    year: i32,
    month: u8,
    day: u8,
    hour: u8,
    minute: u8,
    second: u8,
    nanosecond: u32,
    zone: *const c_char,
    offset_seconds: i32,
    out: *mut u8,
    cap: usize,
) -> isize {
    use icu_calendar::{Date, Iso};
    use icu_datetime::fieldbag::DateTimeFieldBag;
    use icu_datetime::DateTimeFormatter;
    use icu_time::zone::UtcOffset;
    use icu_time::{DateTime, Time, TimeZone, ZonedDateTime};
    let (Some(locale), Some(skeleton)) = (text(locale).and_then(parse_locale), text(skeleton)) else { return -1 };
    let Ok(bag) = DateTimeFieldBag::try_from_skeleton(skeleton) else { return -4 };
    let field_set = bag.to_composite_field_set();
    let Ok(date) = Date::try_new_iso(year, month, day) else { return -5 };
    let Ok(time) = Time::try_new(hour, minute, second, nanosecond) else { return -5 };
    let zone_name = text(zone).filter(|z| !z.is_empty());
    let offset = UtcOffset::try_from_seconds(offset_seconds).ok();
    with_provider(|provider| {
        let zone_id = match zone_name {
            Some(name) => match icu_time::zone::iana::IanaParser::try_new_with_buffer_provider(provider) {
                Ok(parser) => parser.as_borrowed().parse(name),
                Err(_) => TimeZone::UNKNOWN,
            },
            None => TimeZone::UNKNOWN,
        };
        let zone = zone_id.with_offset(offset).at_date_time(DateTime { date: date.clone(), time });
        let input = ZonedDateTime { date: date.clone(), time, zone };
        let Ok(formatter) = DateTimeFormatter::try_new_with_buffer_provider(provider, (&locale).into(), field_set) else { return -2 };
        let formatted = formatter.format(&input);
        let mut collector = PartsCollector { text: String::new(), parts: Vec::new() };
        if formatted.write_to_parts(&mut collector).is_err() {
            return -6;
        }
        emit_with_parts(&collector, out, cap)
    })
    .unwrap_or(-1)
}
