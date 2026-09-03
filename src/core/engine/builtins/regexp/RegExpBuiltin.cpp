/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "quanta/core/engine/builtins/RegExpBuiltin.h"
#include <span>
#include "quanta/core/engine/Context.h"
#include "quanta/core/gc/Collector.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/RegExp.h"
#include "quanta/core/runtime/String.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/core/runtime/Iterator.h"
#include <sstream>
#include "quanta/parser/AST.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace Quanta {

// GetSubstitution: match_start/match_end are UTF-16 offsets into str16 so $`/$'
// slice correctly even when a match boundary falls inside a surrogate pair.
// Returns false when a property get or coercion threw.
static bool regexp_get_substitution(Context& ctx, const std::string& replacement, const std::u16string& str16,
        size_t match_start, size_t match_end, const std::string& matched, const std::vector<std::string>& captures,
        const Value& named_groups, std::string& result) {
    for (size_t i = 0; i < replacement.size(); i++) {
        if (replacement[i] != '$' || i + 1 >= replacement.size()) { result += replacement[i]; continue; }
        char next = replacement[i + 1];
        if (next == '$') { result += '$'; i++; }
        else if (next == '&') { result += matched; i++; }
        else if (next == '`') { result += utf16_to_wtf8(str16.data(), match_start); i++; }
        else if (next == '\'') { result += utf16_to_wtf8(str16.data() + match_end, str16.size() - match_end); i++; }
        else if (next == '<' && !named_groups.is_undefined()) {
            size_t close = replacement.find('>', i + 2);
            if (close != std::string::npos) {
                std::string name = replacement.substr(i + 2, close - i - 2);
                Value val;
                if (named_groups.is_object() || named_groups.is_function()) {
                    Object* ng = named_groups.is_function()
                        ? static_cast<Object*>(named_groups.as_function()) : named_groups.as_object();
                    val = ng->get_property(name);
                    if (ctx.has_exception()) return false;
                } else if (named_groups.is_string()) {
                    // ToObject(string) wrapper emulation: only length and index props exist.
                    std::string gs = named_groups.to_string();
                    if (name == "length") {
                        size_t js_len = 0;
                        for (size_t b = 0; b < gs.size(); ) {
                            unsigned char c = (unsigned char)gs[b];
                            size_t adv = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
                            js_len += (adv == 4) ? 2 : 1;
                            b += adv;
                        }
                        val = Value(static_cast<double>(js_len));
                    }
                }
                if (!val.is_undefined()) {
                    if (val.is_symbol()) { ctx.throw_type_error("Cannot convert Symbol to string"); return false; }
                    if (val.is_object() || val.is_function()) {
                        std::string s = val.to_property_key();
                        if (ctx.has_exception()) return false;
                        result += s;
                    } else {
                        result += val.to_string();
                    }
                }
                i = close;
            } else { result += replacement[i]; }
        }
        else if (next >= '0' && next <= '9') {
            size_t n = next - '0';
            if (i + 2 < replacement.size() && replacement[i+2] >= '0' && replacement[i+2] <= '9') {
                size_t n2 = n * 10 + (replacement[i+2] - '0');
                if (n2 >= 1 && n2 <= captures.size()) { result += captures[n2-1]; i += 2; continue; }
            }
            if (n >= 1 && n <= captures.size()) { result += captures[n-1]; i++; }
            else result += replacement[i];
        } else { result += replacement[i]; }
    }
    return true;
}

// EncodeForRegExpEscape + RegExp.escape main loop over code points. The input is
// WTF-8, so lone surrogates decode naturally and get \uXXXX-escaped.
static std::string regexp_escape_string(const std::string& s) {
    std::string out;
    out.reserve(s.size() * 2);
    static const char* syntax_chars = "^$\\.*+?()[]{}|/";
    static const char* other_punctuators = ",-=<>#&!%:;@~'`\"";
    size_t i = 0;
    bool first = true;
    auto hex2 = [&](uint32_t v) {
        char buf[8];
        snprintf(buf, sizeof(buf), "\\x%02x", v);
        out += buf;
    };
    auto unicode_escape = [&](uint32_t unit) {
        char buf[12];
        snprintf(buf, sizeof(buf), "\\u%04x", unit);
        out += buf;
    };
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        uint32_t cp = len == 1 ? c : len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
        for (size_t k = 1; k < len && i + k < s.size(); k++)
            cp = (cp << 6) | ((unsigned char)s[i+k] & 0x3F);
        i += len;

        if (first && ((cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z'))) {
            hex2(cp);
            first = false;
            continue;
        }
        first = false;
        if (cp < 0x80 && strchr(syntax_chars, (char)cp)) {
            out += '\\';
            out += (char)cp;
            continue;
        }
        switch (cp) {
            case '\t': out += "\\t"; continue;
            case '\n': out += "\\n"; continue;
            case 0x0B: out += "\\v"; continue;
            case '\f': out += "\\f"; continue;
            case '\r': out += "\\r"; continue;
        }
        bool is_other_punct = cp < 0x80 && strchr(other_punctuators, (char)cp);
        bool is_ws = cp == 0x20 || cp == 0xA0 || cp == 0x1680 ||
                     (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 ||
                     cp == 0x202F || cp == 0x205F || cp == 0x3000 || cp == 0xFEFF;
        bool is_surrogate = cp >= 0xD800 && cp <= 0xDFFF;
        if (is_other_punct || is_ws || is_surrogate) {
            if (cp <= 0xFF) hex2(cp);
            else if (cp <= 0xFFFF) unicode_escape(cp);
            else {
                unicode_escape(0xD800 + ((cp - 0x10000) >> 10));
                unicode_escape(0xDC00 + ((cp - 0x10000) & 0x3FF));
            }
            continue;
        }
        // UTF16EncodeCodePoint: append the code point as-is (original UTF-8 bytes).
        out.append(s, i - len, len);
    }
    return out;
}

// RegExpBuiltinExec: the match itself, with lastIndex read and written around
// it. Shared by RegExp.prototype.exec and by the abstract operation below,
// which needs it when a user has replaced exec with something uncallable.
Value regexp_builtin_exec(Context& ctx, Object* r, const std::string& str, const String* cell = nullptr) {
    RegExpObject* reo = RegExpObject::from(r);
    if (!reo || !reo->impl()) {
        ctx.throw_type_error("RegExp.prototype.exec called on incompatible receiver");
        return Value();
    }
    const std::shared_ptr<RegExp>& re = reo->impl();

    Value lastIndex_val = r->get_property("lastIndex");
    double li = lastIndex_val.to_number();
    if (ctx.has_exception()) return Value();
    if (std::isnan(li) || li < 0) li = 0;
    re->set_last_index(li > static_cast<double>(std::numeric_limits<int>::max())
                           ? std::numeric_limits<int>::max() : static_cast<int>(li));

    Value result = re->exec(str, cell);
    // A match that ran out of its budget has no answer, and null would read as
    // one. Every engine has some ceiling here and the spec names none, so the
    // choice is only between a wrong answer and an error.
    if (re->last_match_exhausted()) {
        ctx.throw_range_error("Regular expression match exceeded the step limit");
        return Value();
    }

    int new_last = re->get_last_index();
    if (re->get_global() || re->get_sticky()) {
        bool li_ok = r->set_property("lastIndex", Value(static_cast<double>(new_last)));
        if (!li_ok || ctx.has_exception()) {
            if (!ctx.has_exception()) ctx.throw_type_error("Cannot assign to read only property 'lastIndex'");
            return Value();
        }
    }
    return result;
}

// RegExpBuiltinExec's sibling for callers that only need "did it match".
// Same lastIndex ceremony as regexp_builtin_exec above -- read it, hand it to
// the engine, write it back for a global or sticky regexp, and fail the same
// way if the write is refused -- but RegExp::test does the match without
// building the result array, whose index/input/groups properties and their
// descriptors were the whole cost of RegExp.prototype.test. RegExp::test and
// RegExp::exec move last_index_ identically (both gate on global || sticky),
// so the two paths cannot disagree about where the next match starts.
bool regexp_builtin_test(Context& ctx, Object* r, const std::string& str, bool& ok, const String* cell = nullptr) {
    ok = false;
    RegExpObject* reo = RegExpObject::from(r);
    if (!reo || !reo->impl()) {
        ctx.throw_type_error("RegExp.prototype.test called on incompatible receiver");
        return false;
    }
    const std::shared_ptr<RegExp>& re = reo->impl();

    Value lastIndex_val = r->get_property("lastIndex");
    double li = lastIndex_val.to_number();
    if (ctx.has_exception()) return false;
    if (std::isnan(li) || li < 0) li = 0;
    re->set_last_index(li > static_cast<double>(std::numeric_limits<int>::max())
                           ? std::numeric_limits<int>::max() : static_cast<int>(li));

    bool found = re->test(str, cell);
    if (re->last_match_exhausted()) {
        ctx.throw_range_error("Regular expression match exceeded the step limit");
        return false;
    }

    if (re->get_global() || re->get_sticky()) {
        bool li_ok = r->set_property("lastIndex", Value(static_cast<double>(re->get_last_index())));
        if (!li_ok || ctx.has_exception()) {
            if (!ctx.has_exception()) ctx.throw_type_error("Cannot assign to read only property 'lastIndex'");
            return false;
        }
    }
    ok = true;
    return found;
}

// RegExpExec abstract operation: use a callable "exec" property when present,
// otherwise RegExpBuiltinExec -- replacing RegExp.prototype.exec with a
// non-callable is legal and must not stop a real RegExp from matching.
// An own entry under any of these hides what RegExp.prototype would have
// answered, and the flags getter reads all eight of the flag accessors by
// name -- so an own `global` is enough to make a global pattern behave as a
// non-global one, which the shortcut below would never see. Checked per call,
// not per match, which is what the shortcut is trading away.
static bool regexp_has_shortcut_override(Object* r) {
    static const char* const kNames[] = {
        "exec", "flags", "hasIndices", "global", "ignoreCase",
        "multiline", "dotAll", "unicode", "unicodeSets", "sticky",
    };
    for (const char* n : kNames) {
        if (r->has_own_property(n)) return true;
    }
    return false;
}

// A match result RegExp::exec built (index, then input, then groups added
// in that order; captures live as elements, never shape-resident) carries
// the same shape regardless of which pattern or subject produced it, so the
// slot indices for "index" and "groups" -- read once per match by every
// global replace/matchAll loop -- are worth remembering instead of hashed
// by name every time. "length" needs no lookup at all here: for a genuine,
// unmodified match array it IS the element count.
// Only trusted for that exact shape on a genuine Array with no descriptor
// of its own -- RegExpExec is user-overridable and can hand back any object
// carrying the right property names, which falls through to the caller's
// own get_property path unchanged.
static bool match_result_fast_fields(Object* m, Value& idx_v, Value& grps_v) {
    static thread_local Shape* memo_shape = nullptr;
    static thread_local uint32_t memo_idx_slot = 0;
    static thread_local uint32_t memo_groups_slot = 0;
    if (m->get_type() != Object::ObjectType::Array || m->has_any_descriptor_override()) return false;
    Shape* s = m->get_shape();
    if (!s) return false;
    if (s != memo_shape) {
        int32_t idx_slot = s->find_slot("index");
        int32_t groups_slot = s->find_slot("groups");
        if (idx_slot < 0 || groups_slot < 0 ||
            s->is_accessor_slot("index") || s->is_accessor_slot("groups")) {
            return false;
        }
        memo_shape = s;
        memo_idx_slot = static_cast<uint32_t>(idx_slot);
        memo_groups_slot = static_cast<uint32_t>(groups_slot);
    }
    const Value* iv = m->get_shape_slot_unchecked(memo_idx_slot);
    const Value* gv = m->get_shape_slot_unchecked(memo_groups_slot);
    if (!iv || !gv) return false;
    idx_v = *iv;
    grps_v = *gv;
    return true;
}

// `cell` is the subject's own string cell when the caller has one. It matters
// twice over: a cell rebuilt from the bytes copies the whole subject on every
// call, and the decoded units are remembered per cell -- so rebuilding one
// turned a loop of matches over one subject into a fresh decode of that
// subject for every match.
static bool regexp_exec_abstract(Context& ctx, Object* r, const std::string& str, Value& out,
                                 const String* cell = nullptr) {
    Value exec_fn = r->get_property("exec");
    if (ctx.has_exception()) return false;
    if (!exec_fn.is_function()) {
        if (!RegExpObject::from(r)) {
            ctx.throw_type_error("RegExpExec requires a RegExp or an object with a callable exec");
            return false;
        }
        out = regexp_builtin_exec(ctx, r, str, cell);
        if (ctx.has_exception()) return false;
        return true;
    }
    out = exec_fn.as_function()->call(
        ctx, {cell ? Value(const_cast<String*>(cell)) : Value(str)}, Value(r));
    if (ctx.has_exception()) return false;
    if (!out.is_null() && !out.is_object() && !out.is_function()) {
        ctx.throw_type_error("RegExpExec: exec must return Object or null");
        return false;
    }
    return true;
}

void register_regexp_builtins(Context& ctx) {
    auto regexp_prototype = ObjectFactory::create_object();

    // Annex B compile - delegates to [[compile]] (mirrors exec/[[exec]]) so it actually recompiles.
    auto compile_fn = ObjectFactory::create_native_function("compile",
        [](Context& ctx, std::span<const Value> args, Value receiver) -> Value {
            Object* this_obj = receiver.as_object_or_null();
            if (!RegExpObject::from(this_obj)) {
                ctx.throw_type_error("RegExp.prototype.compile called on incompatible receiver");
                return Value();
            }
            const std::shared_ptr<RegExp>& re = RegExpObject::from(this_obj)->impl();
            if (!re) {
                ctx.throw_type_error("RegExp.prototype.compile called on incompatible receiver");
                return Value();
            }
            std::string pattern = args.size() > 0 ? args[0].to_string() : std::string();
            std::string flags = args.size() > 1 ? args[1].to_string() : std::string();
            if (ctx.has_exception()) return Value();
            re->compile(pattern, flags);
            // The accessors read the implementation, which compile() just
            // rewrote in place, so nothing has to be copied out.
            this_obj->set_property("lastIndex", Value(0.0));
            return Value(this_obj);
        }, 2);
    regexp_prototype->set_property("compile", Value(compile_fn.release()), PropertyAttributes::BuiltinFunction);

    Object* regexp_proto_ptr = regexp_prototype.get();

    auto regexp_constructor = ObjectFactory::create_native_constructor("RegExp",
        [](Context& ctx, std::span<const Value> args, Value receiver) -> Value {
            // ES6: Check IsRegExp(pattern) via Symbol.match
            bool pattern_is_regexp = false;
            std::string pattern = "";
            std::string flags = "";
            if (args.size() > 1 && !args[1].is_undefined()) {
                if (args[1].is_object() || args[1].is_function()) {
                    Object* fo = args[1].is_object() ? args[1].as_object() : static_cast<Object*>(args[1].as_function());
                    Value ts = fo->get_property("toString");
                    if (ctx.has_exception()) return Value();
                    bool ts_ok = false;
                    if (ts.is_function()) {
                        Value r = ts.as_function()->call(ctx, {}, args[1]);
                        if (ctx.has_exception()) return Value();
                        if (!r.is_object() && !r.is_function()) { flags = r.to_string(); ts_ok = true; }
                    }
                    if (!ts_ok) {
                        Value vof = fo->get_property("valueOf");
                        if (ctx.has_exception()) return Value();
                        if (vof.is_function()) {
                            Value r2 = vof.as_function()->call(ctx, {}, args[1]);
                            if (ctx.has_exception()) return Value();
                            if (!r2.is_object() && !r2.is_function()) flags = r2.to_string();
                        }
                    }
                } else {
                    flags = args[1].to_string();
                }
            }

            if (!args.empty() && (args[0].is_object() || args[0].is_function())) {
                Object* pat_obj = args[0].is_object() ? args[0].as_object() : args[0].as_function();
                // IsRegExp: check Symbol.match (fires get trap on Proxy)
                Value sym_match = pat_obj->get_property("Symbol.match");
                if (!sym_match.is_undefined()) {
                    pattern_is_regexp = sym_match.to_boolean();
                } else {
                    // Fallback: the object's own internal slot.
                    pattern_is_regexp = RegExpObject::from(pat_obj) != nullptr;
                }

                if (pattern_is_regexp) {
                    // Get constructor to check if it matches current RegExp
                    Value ctor = pat_obj->get_property("constructor");  // fires get("constructor") on Proxy
                    Value current_regexp = ctx.get_binding("RegExp");
                    bool ctor_matches = false;
                    if (ctor.is_function() && current_regexp.is_function()) {
                        ctor_matches = (ctor.as_function() == current_regexp.as_function());
                    }
                    bool flags_undefined = (args.size() < 2 || args[1].is_undefined());
                    if (ctor_matches && flags_undefined) {
                        // Same constructor and no flags override: return pattern as-is
                        return args[0];
                    }
                    // Different constructor or flags provided: get source and flags
                    Value src = pat_obj->get_property("source");  // fires get("source") on Proxy
                    if (ctx.has_exception()) return Value();
                    pattern = src.is_undefined() ? "" : src.to_string();
                    if (flags_undefined) {
                        Value fl = pat_obj->get_property("flags");  // fires get("flags") on Proxy
                        if (ctx.has_exception()) return Value();
                        flags = fl.is_undefined() ? "" : fl.to_string();
                    }
                } else {
                    // Non-IsRegExp object: ToPrimitive(obj, "string") -- toString then valueOf
                    Value ts = pat_obj->get_property("toString");
                    if (ctx.has_exception()) return Value();
                    bool ts_ok = false;
                    if (ts.is_function()) {
                        Value r = ts.as_function()->call(ctx, {}, args[0]);
                        if (ctx.has_exception()) return Value();
                        if (!r.is_object() && !r.is_function()) { pattern = r.to_string(); ts_ok = true; }
                    }
                    if (!ts_ok) {
                        Value vof = pat_obj->get_property("valueOf");
                        if (ctx.has_exception()) return Value();
                        if (vof.is_function()) {
                            Value r2 = vof.as_function()->call(ctx, {}, args[0]);
                            if (ctx.has_exception()) return Value();
                            if (!r2.is_object() && !r2.is_function()) pattern = r2.to_string();
                        }
                    }
                }
            } else if (!args.empty() && !args[0].is_undefined()) {
                pattern = args[0].to_string();
            }

            if (flags.find('u') != std::string::npos && flags.find('v') != std::string::npos) {
                ctx.throw_syntax_error("Regex flags 'u' and 'v' are mutually exclusive");
                return Value();
            }

            try {
                // ObjectType::RegExp so toString's internal-slot tag and String.prototype's
                // is_native_regexp checks (match/replace/split) see this the same as regex literals.
                auto regexp_impl = std::make_shared<RegExp>(pattern, flags);
                // Every flag used to be copied onto the object under a [[name]]
                // key; the prototype accessors read them off the impl instead,
                // so lastIndex is the one own property an instance has.
                auto regex_obj = std::make_unique<RegExpObject>(regexp_impl);
                regex_obj->set_property("lastIndex", Value(static_cast<double>(regexp_impl->get_last_index())), PropertyAttributes::Writable);
                
                Object* regex_raw = regex_obj.release();
                Value new_target = ctx.get_new_target();
                if (!new_target.is_undefined()) {
                    Object* nt_obj = new_target.is_function()
                        ? static_cast<Object*>(new_target.as_function())
                        : new_target.is_object() ? new_target.as_object() : nullptr;
                    if (nt_obj) {
                        Value nt_proto = nt_obj->get_property("prototype");
                        if (nt_proto.is_object()) regex_raw->set_prototype(nt_proto.as_object());
                    }
                } else {
                    Value regexp_ctor = ctx.get_binding("RegExp");
                    if (regexp_ctor.is_function()) {
                        Value proto = regexp_ctor.as_function()->get_property("prototype");
                        if (proto.is_object()) regex_raw->set_prototype(proto.as_object());
                    }
                }
                return Value(regex_raw);

            } catch (const std::exception& e) {
                ctx.throw_syntax_error(std::string(e.what()));
                return Value();
            }
        });

    // ES6: RegExp.prototype.toString is generic - works on any object with source/flags
    auto regexp_toString = ObjectFactory::create_native_function("toString",
        [](Context& ctx, std::span<const Value> args, Value receiver) -> Value {
            (void)args;
            Object* this_obj = receiver.as_object_or_null();
            if (!this_obj) {
                ctx.throw_type_error("RegExp.prototype.toString called on incompatible receiver");
                return Value();
            }
            Value source_val = this_obj->get_property("source");
            Value flags_val = this_obj->get_property("flags");
            std::string source = source_val.is_undefined() ? "(?:)" : source_val.to_string();
            std::string flags = flags_val.is_undefined() ? "" : flags_val.to_string();
            return Value("/" + source + "/" + flags);
        }, 0);
    regexp_prototype->set_property("toString", Value(regexp_toString.release()), PropertyAttributes::BuiltinFunction);

    PropertyDescriptor regexp_constructor_desc(Value(regexp_constructor.get()),
        PropertyAttributes::BuiltinFunction);
    regexp_prototype->set_property_descriptor("constructor", regexp_constructor_desc);

    // ES2015+: flag properties are ACCESSOR getters on RegExp.prototype (not own data on instances). Each getter throws TypeError for non-RegExp this (including RegExp.prototype itself), else reads flag from [[name]] internal slot on the instance.
    {
        using FlagReader = bool (*)(const RegExp&);
        auto make_flag_getter = [regexp_proto_ptr](const char* getter_name, FlagReader read) {
            return ObjectFactory::create_native_function(getter_name,
                [regexp_proto_ptr, read](Context& ctx, std::span<const Value>, Value receiver) -> Value {
                    Object* self = receiver.as_object_or_null();
                    if (!self) { ctx.throw_type_error("RegExp flag getter requires a RegExp"); return Value(); }
                    if (self == regexp_proto_ptr) return Value(); // spec: return undefined for RegExp.prototype
                    RegExpObject* re = RegExpObject::from(self);
                    if (!re || !re->impl()) { ctx.throw_type_error("RegExp flag getter requires a RegExp"); return Value(); }
                    return Value(read(*re->impl()));
                }, 0);
        };
        auto make_flag_desc = [](std::unique_ptr<Function> getter_fn) {
            PropertyDescriptor d;
            d.set_getter(getter_fn.release());
            d.set_enumerable(false);
            d.set_configurable(true);
            return d;
        };
        regexp_prototype->set_property_descriptor("hasIndices", make_flag_desc(make_flag_getter("get hasIndices", [](const RegExp& r){ return r.get_flags().find('d') != std::string::npos; })));
        regexp_prototype->set_property_descriptor("global",     make_flag_desc(make_flag_getter("get global",     [](const RegExp& r){ return r.get_global(); })));
        regexp_prototype->set_property_descriptor("ignoreCase", make_flag_desc(make_flag_getter("get ignoreCase", [](const RegExp& r){ return r.get_ignore_case(); })));
        regexp_prototype->set_property_descriptor("multiline",  make_flag_desc(make_flag_getter("get multiline",  [](const RegExp& r){ return r.get_multiline(); })));
        regexp_prototype->set_property_descriptor("dotAll",     make_flag_desc(make_flag_getter("get dotAll",     [](const RegExp& r){ return r.get_dotall(); })));
        regexp_prototype->set_property_descriptor("unicode",    make_flag_desc(make_flag_getter("get unicode",    [](const RegExp& r){ return r.get_unicode() && !r.get_unicode_sets(); })));
        regexp_prototype->set_property_descriptor("sticky",     make_flag_desc(make_flag_getter("get sticky",     [](const RegExp& r){ return r.get_sticky(); })));
        // source accessor: empty pattern renders as "(?:)" and slashes in pattern are escaped.
        regexp_prototype->set_property_descriptor("source", make_flag_desc(
            ObjectFactory::create_native_function("get source",
                [regexp_proto_ptr](Context& ctx, std::span<const Value>, Value receiver) -> Value {
                    Object* self = receiver.as_object_or_null();
                    if (!self) { ctx.throw_type_error("get source requires a RegExp"); return Value(); }
                    if (self == regexp_proto_ptr) return Value(std::string("(?:)")); // spec: return "(?:)" for RegExp.prototype
                    RegExpObject* sre = RegExpObject::from(self);
                    if (!sre || !sre->impl()) { ctx.throw_type_error("get source requires a RegExp"); return Value(); }
                    std::string s = sre->impl()->get_source();
                    if (s.empty()) return Value(std::string("(?:)"));
                    // EscapeRegExpPattern: escape '/' and line terminators so the result
                    // round-trips through a regex literal.
                    std::string escaped;
                    for (size_t i = 0; i < s.size(); i++) {
                        if (s[i] == '/' && (i == 0 || s[i-1] != '\\')) { escaped += "\\/"; continue; }
                        if (s[i] == '\n') { escaped += "\\n"; continue; }
                        if (s[i] == '\r') { escaped += "\\r"; continue; }
                        if ((unsigned char)s[i] == 0xE2 && i + 2 < s.size() &&
                            (unsigned char)s[i+1] == 0x80 &&
                            ((unsigned char)s[i+2] == 0xA8 || (unsigned char)s[i+2] == 0xA9)) {
                            escaped += ((unsigned char)s[i+2] == 0xA8) ? "\\u2028" : "\\u2029";
                            i += 2;
                            continue;
                        }
                        escaped += s[i];
                    }
                    return Value(escaped);
                }, 0)));
    }

    // ES2022: RegExp.prototype.flags accessor (reads flag props via get_property for Proxy support)
    {
        auto flags_getter_fn = ObjectFactory::create_native_function("get flags",
            [](Context& ctx, std::span<const Value> args, Value receiver) -> Value {
                (void)args;
                Value raw_this_fl = receiver;
                if (!raw_this_fl.is_object() && !raw_this_fl.is_function()) {
                    ctx.throw_type_error("RegExp.prototype.flags getter called on non-object");
                    return Value();
                }
                Object* this_obj = receiver.as_object_or_null();
                if (!this_obj) {
                    ctx.throw_type_error("RegExp.prototype.flags getter called on incompatible receiver");
                    return Value();
                }
                // Check flags in ES2022 canonical order: d, g, i, m, s, u, v, y. Each flag is an accessor on the prototype that throws TypeError for non-regex `this` -- propagate that if any getter throws.
                std::string result;
                auto add_flag = [&](const char* prop, char ch) -> bool {
                    Value v = this_obj->get_property(prop);
                    if (ctx.has_exception()) return false;
                    if (v.to_boolean()) result += ch;
                    return !ctx.has_exception();
                };
                if (!add_flag("hasIndices", 'd')) return Value();
                if (!add_flag("global",     'g')) return Value();
                if (!add_flag("ignoreCase", 'i')) return Value();
                if (!add_flag("multiline",  'm')) return Value();
                if (!add_flag("dotAll",     's')) return Value();
                if (!add_flag("unicode",    'u')) return Value();
                if (!add_flag("unicodeSets",'v')) return Value();
                if (!add_flag("sticky",     'y')) return Value();
                return Value(result);
            });
        PropertyDescriptor flags_desc;
        flags_desc.set_getter(flags_getter_fn.release());
        flags_desc.set_enumerable(false);
        flags_desc.set_configurable(true);
        regexp_prototype->set_property_descriptor("flags", flags_desc);
    }

    // ES2024: RegExp.prototype.unicodeSets accessor (regexp-v-flag)
    {
        auto unicode_sets_getter_fn = ObjectFactory::create_native_function("get unicodeSets",
            [regexp_proto_ptr](Context& ctx, std::span<const Value> args, Value receiver) -> Value {
                (void)args;
                Object* this_obj = receiver.as_object_or_null();
                if (!this_obj) {
                    ctx.throw_type_error("RegExp.prototype.unicodeSets getter called on incompatible receiver");
                    return Value();
                }
                if (this_obj == regexp_proto_ptr) return Value();
                RegExpObject* ure = RegExpObject::from(this_obj);
                if (!ure || !ure->impl()) {
                    ctx.throw_type_error("RegExp.prototype.unicodeSets getter called on incompatible receiver");
                    return Value();
                }
                return Value(ure->impl()->get_unicode_sets());
            });
        PropertyDescriptor unicode_sets_desc;
        unicode_sets_desc.set_getter(unicode_sets_getter_fn.release());
        unicode_sets_desc.set_enumerable(false);
        unicode_sets_desc.set_configurable(true);
        regexp_prototype->set_property_descriptor("unicodeSets", unicode_sets_desc);
    }

    // RegExp.prototype.exec - delegates to the instance's [[exec]] internal slot
    auto regexp_exec_proto_fn = ObjectFactory::create_native_function("exec",
        [](Context& ctx, std::span<const Value> args, Value receiver) -> Value {
            Object* this_obj = receiver.as_object_or_null();
            if (!RegExpObject::from(this_obj)) {
                ctx.throw_type_error("RegExp.prototype.exec called on incompatible receiver");
                return Value();
            }
            // ToString(string): spec treats no-arg as exec("undefined"), not early null.
            Value arg0 = args.empty() ? Value() : args[0];
            std::string owned;
            // Borrowed when the subject is already a string: a global regex is
            // driven one exec per match, and copying the subject per call costs
            // its whole length every time.
            const String* cell = arg0.is_string() ? arg0.as_string() : nullptr;
            if (arg0.is_symbol()) {
                ctx.throw_type_error("Cannot convert Symbol to string");
                return Value();
            } else if (arg0.is_object() || arg0.is_function()) {
                // ToPrimitive(arg, "string"): go through JS-level toString/valueOf.
                owned = arg0.to_property_key();
                if (ctx.has_exception()) return Value();
            } else if (!cell) {
                owned = arg0.to_string();
            }
            // Not cell->str(): the bytes are only needed when there is no
            // cell to read the subject from, and asking a slice for them
            // flattens it -- the copy the cell is here to avoid.
            static const std::string kNoBytes;
            return regexp_builtin_exec(ctx, this_obj, cell ? kNoBytes : owned, cell);
        }, 1);
    // Kept for identity only, never dereferenced: RegExp.prototype.exec is
    // reachable from the prototype for as long as this realm lives, and test
    // below needs to tell "still the built-in" from "user replaced it".
    Function* builtin_regexp_exec = regexp_exec_proto_fn.get();
    regexp_prototype->set_property("exec", Value(regexp_exec_proto_fn.release()), PropertyAttributes::BuiltinFunction);

    // ES6: RegExp.prototype.test - generic function that calls this.exec
    auto regexp_test_fn = ObjectFactory::create_native_function("test",
        [builtin_regexp_exec](Context& ctx, std::span<const Value> args, Value receiver) -> Value {
            Object* this_obj = receiver.as_object_or_null();
            if (!RegExpObject::from(this_obj)) {
                ctx.throw_type_error("RegExp.prototype.test called on incompatible receiver");
                return Value();
            }
            Value arg0_t = args.empty() ? Value() : args[0];
            std::string owned_t;
            const String* cell_t = arg0_t.is_string() ? arg0_t.as_string() : nullptr;
            if (arg0_t.is_symbol()) {
                ctx.throw_type_error("Cannot convert Symbol to string");
                return Value();
            }
            if (arg0_t.is_object() || arg0_t.is_function()) { owned_t = arg0_t.to_property_key(); if (ctx.has_exception()) return Value(); }
            else if (!cell_t) { owned_t = arg0_t.to_string(); }
            Value exec_fn = this_obj->get_property("exec");
            // Whitelist: a real RegExp still carrying the built-in exec. A
            // replaced exec has to be CALLED, and its result is observable, so
            // everything else keeps the path it always had.
            if (exec_fn.is_function() && exec_fn.as_function() == builtin_regexp_exec) {
                bool ok = false;
                // Not cell_t->str(): the bytes are only needed when there is no
                // cell to read the subject from, and asking a slice for them
                // flattens it -- the copy the cell is here to avoid.
                static const std::string kNoBytes;
                bool found = regexp_builtin_test(ctx, this_obj,
                                                 cell_t ? kNoBytes : owned_t, ok, cell_t);
                if (!ok) return Value();
                return Value(found);
            }
            // A replaced exec is CALLED with the subject, so this path does
            // need the bytes.
            const std::string& str = cell_t ? cell_t->str() : owned_t;
            // Everything else goes through RegExpExec, which calls a replaced
            // exec and otherwise falls back to RegExpBuiltinExec -- returning
            // false for a non-callable exec skipped the match entirely.
            Value result;
            if (!regexp_exec_abstract(ctx, this_obj, str, result, cell_t)) return Value();
            return Value(!result.is_null() && !result.is_undefined());
        }, 1);
    regexp_prototype->set_property("test", Value(regexp_test_fn.release()), PropertyAttributes::BuiltinFunction);

    // ES6: RegExp.prototype[Symbol.match/replace/search/split]
    auto regexp_sym_match = ObjectFactory::create_native_function("[Symbol.match]",
        [](Context& ctx, std::span<const Value> args, Value receiver) -> Value {
            Value raw_this_m = receiver;
            if (!raw_this_m.is_object() && !raw_this_m.is_function()) {
                ctx.throw_type_error("RegExp.prototype[Symbol.match] called on non-object");
                return Value();
            }
            Object* this_obj = receiver.as_object_or_null();
            if (!this_obj) { ctx.throw_type_error("RegExp.prototype[Symbol.match] requires an object this value"); return Value(); }
            Value arg0_m = args.empty() ? Value() : args[0];
            // Borrowed so the matching below runs against the subject's own
            // cell rather than a copy rebuilt from its bytes.
            const String* subject_cell = arg0_m.is_string() ? arg0_m.as_string() : nullptr;
            std::string str;
            if (arg0_m.is_symbol()) { ctx.throw_type_error("Cannot convert Symbol to string"); return Value(); }
            else if (arg0_m.is_object() || arg0_m.is_function()) { str = arg0_m.to_property_key(); if (ctx.has_exception()) return Value(); }
            else { str = arg0_m.to_string(); }
            // Spec step 4: Let flags = ToString(Get(rx, "flags"))
            Value flags_v = this_obj->get_property("flags");
            if (ctx.has_exception()) return Value();
            std::string flags_str;
            if (flags_v.is_object() || flags_v.is_function()) {
                // ToPrimitive(flags, "string")
                Object* fobj = flags_v.is_object() ? flags_v.as_object() : static_cast<Object*>(flags_v.as_function());
                Value tp = fobj->get_property(std::string("Symbol.toPrimitive"));
                if (ctx.has_exception()) return Value();
                if (tp.is_function()) {
                    Value r = tp.as_function()->call(ctx, {Value(std::string("string"))}, flags_v);
                    if (ctx.has_exception()) return Value();
                    flags_str = r.is_object() ? "" : r.to_string();
                } else {
                    Value ts = fobj->get_property("toString");
                    if (ctx.has_exception()) return Value();
                    if (ts.is_function()) {
                        Value r = ts.as_function()->call(ctx, {}, flags_v);
                        if (ctx.has_exception()) return Value();
                        flags_str = r.is_object() ? "" : r.to_string();
                    } else {
                        flags_str = flags_v.to_string();
                    }
                }
            } else {
                flags_str = flags_v.to_string();
            }
            if (ctx.has_exception()) return Value();
            bool is_global = flags_str.find('g') != std::string::npos;
            bool full_unicode_m = flags_str.find('u') != std::string::npos || flags_str.find('v') != std::string::npos;
            if (!is_global) {
                // Non-global: RegExpExec once
                Value res;
                if (!regexp_exec_abstract(ctx, this_obj, str, res)) return Value();
                return res;
            }
            // Global: Set(rx, "lastIndex", 0, true) -- strict
            {
                bool ok = this_obj->set_property("lastIndex", Value(0.0));
                if (ctx.has_exception()) return Value();
                if (!ok) { ctx.throw_type_error("Cannot assign to read only property 'lastIndex'"); return Value(); }
            }
            auto result_array = ObjectFactory::create_array();
            size_t match_count = 0;
            while (true) {
                Value match;
                if (!regexp_exec_abstract(ctx, this_obj, str, match, subject_cell)) return Value();
                if (match.is_null() || match.is_undefined()) break;
                Object* m_obj = match.is_function() ? static_cast<Object*>(match.as_function()) : match.as_object();
                Value matched_v = m_obj->get_element(0);
                if (ctx.has_exception()) return Value();
                std::string matched_str;
                if (matched_v.is_symbol()) { ctx.throw_type_error("Cannot convert Symbol to string"); return Value(); }
                else if (matched_v.is_object() || matched_v.is_function()) { matched_str = matched_v.to_property_key(); if (ctx.has_exception()) return Value(); }
                else { matched_str = matched_v.to_string(); if (ctx.has_exception()) return Value(); }
                result_array->set_element(match_count++, Value(matched_str));
                if (matched_str.empty()) {
                    // Per spec step 5.g.iv.5: thisIndex = ToLength(Get(rx, "lastIndex")).
                    // Must call Get/ToLength for coercion side effects (e.g. coerce-lastindex tests).
                    Value li_v = this_obj->get_property("lastIndex");
                    if (ctx.has_exception()) return Value();
                    li_v.to_number(); // invoke ToLength coercion; throw propagates via ctx
                    if (ctx.has_exception()) return Value();
                    // Use match result index as AdvanceStringIndex base to avoid
                    // double-advance when built-in exec already updated lastIndex.
                    Value idx_v = m_obj->get_property("index");
                    if (ctx.has_exception()) return Value();
                    double idx_d = idx_v.to_number();
                    if (ctx.has_exception()) return Value();
                    if (std::isnan(idx_d) || idx_d < 0) idx_d = 0;
                    size_t match_pos = static_cast<size_t>(idx_d);
                    size_t nextIdx = match_pos + 1;
                    if (full_unicode_m) {
                        std::u16string s16 = wtf8_to_utf16(str);
                        if (match_pos < s16.size() && s16[match_pos] >= 0xD800 && s16[match_pos] <= 0xDBFF)
                            nextIdx = match_pos + 2;
                    }
                    bool ok = this_obj->set_property("lastIndex", Value(static_cast<double>(nextIdx)));
                    if (ctx.has_exception()) return Value();
                    if (!ok) { ctx.throw_type_error("Cannot assign to read only property 'lastIndex'"); return Value(); }
                }
            }
            if (match_count == 0) return Value::null();
            result_array->set_length(static_cast<uint32_t>(match_count));
            return Value(result_array.release());
        }, 1);
    regexp_prototype->set_property("Symbol.match", Value(regexp_sym_match.release()), PropertyAttributes::BuiltinFunction);

    auto regexp_sym_replace = ObjectFactory::create_native_function("[Symbol.replace]",
        [](Context& ctx, std::span<const Value> args, Value receiver) -> Value {
            Value raw_this_r = receiver;
            if (!raw_this_r.is_object() && !raw_this_r.is_function()) {
                ctx.throw_type_error("RegExp.prototype[Symbol.replace] called on non-object");
                return Value();
            }
            Object* this_obj = receiver.as_object_or_null();
            if (!this_obj) { ctx.throw_type_error("RegExp.prototype[Symbol.replace] requires an object this value"); return Value(); }
            // Step 3: ToString(string)
            Value arg0_r = args.size() > 0 ? args[0] : Value();
            // Borrowed so the matching below runs against the subject's own
            // cell rather than a copy rebuilt from its bytes.
            const String* subject_cell = arg0_r.is_string() ? arg0_r.as_string() : nullptr;
            std::string str;
            if (arg0_r.is_symbol()) { ctx.throw_type_error("Cannot convert Symbol to string"); return Value(); }
            else if (arg0_r.is_object() || arg0_r.is_function()) { str = arg0_r.to_property_key(); if (ctx.has_exception()) return Value(); }
            else { str = arg0_r.to_string(); }
            Value replace_val = args.size() > 1 ? args[1] : Value();
            // Step 5: functionalReplace = IsCallable(replaceValue)
            bool is_fn_replace = replace_val.is_function();
            // Step 6: if not functional, ToString(replaceValue)
            std::string replace_str;
            if (!is_fn_replace) {
                if (replace_val.is_symbol()) { ctx.throw_type_error("Cannot convert Symbol to string"); return Value(); }
                else if (replace_val.is_object() || replace_val.is_function()) { replace_str = replace_val.to_property_key(); if (ctx.has_exception()) return Value(); }
                else { replace_str = replace_val.to_string(); }
            }
            // Everything below reads the match back out of an object it just
            // built, one per match, with index/input/groups and their
            // descriptors -- and for a literal replacement none of it is
            // observable. Taking the shortcut needs the whole shape to be
            // ordinary, so the conditions are listed rather than the exceptions
            // ruled out: a real RegExp whose prototype is still the one the
            // engine installed, with exec and flags still on it and no own copy
            // of either, global and not sticky, and a replacement string with
            // no $ substitution in it.
            if (!is_fn_replace && replace_str.find('$') == std::string::npos &&
                Object::regexp_proto_protector_intact() &&
                this_obj->get_prototype() == Object::watched_regexp_prototype() &&
                !regexp_has_shortcut_override(this_obj)) {
                RegExpObject* reo = RegExpObject::from(this_obj);
                if (reo && reo->impl() && reo->impl()->get_global() && !reo->impl()->get_sticky()) {
                    // Step 12 either way: the general path's own exec writes it
                    // back to zero when it finally fails to match.
                    bool li_ok = this_obj->set_property("lastIndex", Value(0.0));
                    if (ctx.has_exception()) return Value();
                    if (!li_ok) {
                        ctx.throw_type_error("Cannot assign to read only property 'lastIndex' of regexp");
                        return Value();
                    }
                    std::string replaced;
                    if (reo->impl()->replace_all_literal(str, replace_str, replaced)) {
                        return Value(replaced);
                    }
                }
            }
            // Steps 7-9: read flags string (not global/unicode directly -- fires flags getter which reads them)
            Value flags_v = this_obj->get_property("flags");
            if (ctx.has_exception()) return Value();
            std::string flags_str;
            if (flags_v.is_object() || flags_v.is_function()) {
                flags_str = flags_v.to_property_key();
                if (ctx.has_exception()) return Value();
            } else {
                flags_str = flags_v.to_string();
            }
            bool is_global = flags_str.find('g') != std::string::npos;
            bool full_unicode = flags_str.find('u') != std::string::npos || flags_str.find('v') != std::string::npos;

            if (!is_global) {
                // Non-global: RegExpExec once
                Value match;
                if (!regexp_exec_abstract(ctx, this_obj, str, match, subject_cell)) return Value();
                if (match.is_null() || match.is_undefined()) return Value(str);
                if (!match.is_object()) return Value(str);
                Object* m = match.as_object();
                // Get matched (element 0), ToString it
                Value matched_v = m->get_element(0);
                if (ctx.has_exception()) return Value();
                std::string matched_str;
                if (matched_v.is_symbol()) { ctx.throw_type_error("Cannot convert Symbol to string"); return Value(); }
                else if (matched_v.is_object() || matched_v.is_function()) { matched_str = matched_v.to_property_key(); if (ctx.has_exception()) return Value(); }
                else { matched_str = matched_v.to_string(); }
                // Get index
                Value idx_v = m->get_property("index");
                if (ctx.has_exception()) return Value();
                double idx_d = idx_v.to_number();
                if (ctx.has_exception()) return Value();
                if (std::isnan(idx_d) || idx_d < 0) idx_d = 0;
                int idx_js = static_cast<int>(idx_d);
                std::u16string str16 = wtf8_to_utf16(str);
                size_t pos = std::min(static_cast<size_t>(idx_js), str16.size());
                size_t end_pos = std::min(pos + utf16_length(matched_str), str16.size());
                // Get nCaptures
                Value len_v = m->get_property("length");
                if (ctx.has_exception()) return Value();
                double len_d = len_v.to_number();
                if (ctx.has_exception()) return Value();
                int nCap = (std::isnan(len_d) || len_d < 1) ? 0 : static_cast<int>(len_d) - 1;
                if (nCap < 0) nCap = 0;
                std::string replacement;
                if (is_fn_replace) {
                    // fn(matchedStr, cap1..., position, S [, groups])
                    std::vector<Value> fn_a;
                    fn_a.push_back(Value(matched_str));
                    for (int ci = 1; ci <= nCap; ci++) {
                        Value cv = m->get_element(ci);
                        if (ctx.has_exception()) return Value();
                        fn_a.push_back(cv);
                    }
                    fn_a.push_back(Value(static_cast<double>(idx_js)));
                    fn_a.push_back(Value(str));
                    Value grps_v = m->get_property("groups");
                    if (ctx.has_exception()) return Value();
                    if (!grps_v.is_undefined()) fn_a.push_back(grps_v);
                    Value r = replace_val.as_function()->call(ctx, fn_a, Value());
                    if (ctx.has_exception()) return Value();
                    if (r.is_symbol()) { ctx.throw_type_error("Cannot convert Symbol to string"); return Value(); }
                    else if (r.is_object() || r.is_function()) { replacement = r.to_property_key(); if (ctx.has_exception()) return Value(); }
                    else { replacement = r.to_string(); if (ctx.has_exception()) return Value(); }
                } else {
                    std::vector<std::string> caps;
                    for (int ci = 1; ci <= nCap; ci++) {
                        Value cv = m->get_element(ci);
                        if (ctx.has_exception()) return Value();
                        if (cv.is_undefined()) caps.push_back("");
                        else if (cv.is_symbol()) caps.push_back("");
                        else if (cv.is_object() || cv.is_function()) { std::string cs = cv.to_property_key(); if (ctx.has_exception()) return Value(); caps.push_back(cs); }
                        else caps.push_back(cv.to_string());
                    }
                    Value grps_v = m->get_property("groups");
                    if (ctx.has_exception()) return Value();
                    if (grps_v.is_null()) { ctx.throw_type_error("Cannot convert null to object"); return Value(); }
                    if (!regexp_get_substitution(ctx, replace_str, str16, pos, end_pos, matched_str, caps, grps_v, replacement))
                        return Value();
                }
                return Value(utf16_to_wtf8(str16.data(), pos) + replacement +
                             utf16_to_wtf8(str16.data() + end_pos, str16.size() - end_pos));
            }
            // Global: Set(rx, "lastIndex", 0, true)
            bool set_ok = this_obj->set_property("lastIndex", Value(0.0));
            if (ctx.has_exception()) return Value();
            if (!set_ok) { ctx.throw_type_error("Cannot assign to read only property 'lastIndex' of regexp"); return Value(); }
            struct MatchRecord { int js_idx; std::string matched; std::vector<Value> captures; Value groups; };
            std::vector<MatchRecord> matches;
            // matches' Values (captures/groups) live inside a vector<MatchRecord>,
            // which ValueVectorRoot can't root directly (it only walks vector<Value>).
            // Mirror every such Value into this flat, rooted vector too, so they
            // survive from collection here through the replacement loop below,
            // which may run long after (and allocate a lot, under GC_STRESS).
            std::vector<Value> matches_values_root_vec;
            ValueVectorRoot matches_values_root(&matches_values_root_vec);
            size_t safety = 0;
            const size_t max_iter = str.length() + 2;
            while (safety++ < max_iter) {
                Value match;
                if (!regexp_exec_abstract(ctx, this_obj, str, match, subject_cell)) return Value();
                if (match.is_null()) break;
                Object* m = match.is_function() ? static_cast<Object*>(match.as_function()) : match.as_object();
                Value idx_v, grps_v;
                int mlen;
                bool fast = match_result_fast_fields(m, idx_v, grps_v);
                if (fast) {
                    mlen = static_cast<int>(m->element_count());
                } else {
                    idx_v = m->get_property("index");
                    if (ctx.has_exception()) return Value();
                }
                double idx_d = idx_v.to_number();
                if (ctx.has_exception()) return Value();
                int idx_js = (std::isnan(idx_d) || idx_d < 0) ? 0 : static_cast<int>(idx_d);
                Value el0 = m->get_element(0);
                if (ctx.has_exception()) return Value();
                std::string matched_s;
                if (el0.is_symbol()) { ctx.throw_type_error("Cannot convert Symbol to string"); return Value(); }
                else if (el0.is_object() || el0.is_function()) { matched_s = el0.to_property_key(); if (ctx.has_exception()) return Value(); }
                else { matched_s = el0.is_undefined() ? "undefined" : el0.to_string(); }
                if (!fast) {
                    Value len_v = m->get_property("length");
                    if (ctx.has_exception()) return Value();
                    double len_d = len_v.to_number();
                    if (ctx.has_exception()) return Value();
                    mlen = (std::isnan(len_d) || len_d < 1) ? 1 : static_cast<int>(len_d);
                }
                std::vector<Value> caps;
                for (int ci = 1; ci < mlen; ci++) {
                    Value cv = m->get_element(ci);
                    if (ctx.has_exception()) return Value();
                    if (!is_fn_replace && !cv.is_undefined()) {
                        // For string replace, convert captures to string
                        if (cv.is_symbol()) cv = Value(std::string(""));
                        else if (cv.is_object() || cv.is_function()) { std::string cs = cv.to_property_key(); if (ctx.has_exception()) return Value(); cv = Value(cs); }
                        else cv = Value(cv.to_string());
                    }
                    caps.push_back(cv);
                }
                if (!fast) {
                    grps_v = m->get_property("groups");
                    if (ctx.has_exception()) return Value();
                }
                matches.push_back({idx_js, matched_s, caps, grps_v});
                for (const auto& c : caps) matches_values_root_vec.push_back(c);
                matches_values_root_vec.push_back(grps_v);
                if (matched_s.empty()) {
                    // ToLength(Get(rx, "lastIndex")) then AdvanceStringIndex
                    Value cur_li = this_obj->get_property("lastIndex");
                    if (ctx.has_exception()) return Value();
                    double thisIdx = cur_li.to_number();
                    if (ctx.has_exception()) return Value();
                    if (std::isnan(thisIdx) || thisIdx < 0) thisIdx = 0;
                    if (thisIdx > 9007199254740991.0) thisIdx = 9007199254740991.0;
                    size_t thisIdxSz = static_cast<size_t>(thisIdx);
                    size_t nextIdx = thisIdxSz + 1;
                    if (full_unicode) {
                        std::u16string s16 = wtf8_to_utf16(str);
                        if (thisIdxSz < s16.size() && s16[thisIdxSz] >= 0xD800 && s16[thisIdxSz] <= 0xDBFF)
                            nextIdx = thisIdxSz + 2;
                    }
                    bool adv_ok = this_obj->set_property("lastIndex", Value(static_cast<double>(nextIdx)));
                    if (ctx.has_exception()) return Value();
                    if (!adv_ok) { ctx.throw_type_error("Cannot assign to read only property 'lastIndex'"); return Value(); }
                }
            }
            if (matches.empty()) return Value(str);
            std::u16string str16 = wtf8_to_utf16(str);
            // Every callback gets the same subject S. Rebuilding it per match
            // allocated a second copy of the whole string each time; when the
            // argument arrived as a string its own cell already is S.
            Value subject_value = arg0_r.is_string() ? arg0_r : Value(str);
            // One argument list for all the matches: cleared and refilled
            // rather than allocated per call, and rooted, which its per-match
            // predecessor was not -- a vector's elements sit in malloc'd
            // storage the conservative stack scan never walks.
            std::vector<Value> fn_a;
            ValueVectorRoot fn_a_root(&fn_a);
            std::string result;
            size_t last_end = 0;
            for (auto& mr : matches) {
                size_t mi = static_cast<size_t>(mr.js_idx >= 0 ? mr.js_idx : 0);
                if (mi > str16.size()) mi = str16.size();
                // Spec: skip match if position moved backwards (position < nextSourcePosition)
                if (mi < last_end) continue;
                // Asked once: the two places below that need where this match
                // ends were each converting the matched text to UTF-16 in full
                // to read a length off it.
                const size_t matched_units = utf16_length(mr.matched);
                if (mi > last_end) result += utf16_to_wtf8(str16.data() + last_end, mi - last_end);
                std::string repl;
                if (is_fn_replace) {
                    fn_a.clear();
                    fn_a.push_back(Value(mr.matched));
                    for (auto& c : mr.captures) fn_a.push_back(c);
                    fn_a.push_back(Value(static_cast<double>(mr.js_idx)));
                    fn_a.push_back(subject_value);
                    if (!mr.groups.is_undefined()) fn_a.push_back(mr.groups);
                    Value r = replace_val.as_function()->call(ctx, fn_a, Value());
                    if (ctx.has_exception()) return Value();
                    if (r.is_symbol()) { ctx.throw_type_error("Cannot convert Symbol to string"); return Value(); }
                    else if (r.is_object() || r.is_function()) { repl = r.to_property_key(); if (ctx.has_exception()) return Value(); }
                    else { repl = r.to_string(); if (ctx.has_exception()) return Value(); }
                } else {
                    std::vector<std::string> caps_str;
                    for (auto& c : mr.captures) caps_str.push_back(c.is_undefined() ? "" : c.to_string());
                    if (mr.groups.is_null()) { ctx.throw_type_error("Cannot convert null to object"); return Value(); }
                    size_t m_end = std::min(mi + matched_units, str16.size());
                    if (!regexp_get_substitution(ctx, replace_str, str16, mi, m_end, mr.matched, caps_str, mr.groups, repl))
                        return Value();
                }
                result += repl;
                last_end = std::min(mi + matched_units, str16.size());
            }
            if (last_end <= str16.size())
                result += utf16_to_wtf8(str16.data() + last_end, str16.size() - last_end);
            return Value(result);
        }, 2);
    regexp_prototype->set_property("Symbol.replace", Value(regexp_sym_replace.release()), PropertyAttributes::BuiltinFunction);

    auto regexp_sym_search = ObjectFactory::create_native_function("[Symbol.search]",
        [](Context& ctx, std::span<const Value> args, Value receiver) -> Value {
            // Step 1: If Type(rx) is not Object, throw TypeError
            Value raw_this_s = receiver;
            if (!raw_this_s.is_object() && !raw_this_s.is_function()) {
                ctx.throw_type_error("RegExp.prototype[Symbol.search] called on non-object");
                return Value();
            }
            Object* this_obj = receiver.as_object_or_null();
            if (!this_obj) { ctx.throw_type_error("RegExp.prototype[Symbol.search] requires an object this value"); return Value(); }
            Value arg0_s = args.empty() ? Value() : args[0];
            // Borrowed so the matching below runs against the subject's own
            // cell rather than a copy rebuilt from its bytes.
            const String* subject_cell = arg0_s.is_string() ? arg0_s.as_string() : nullptr;
            std::string str;
            if (arg0_s.is_symbol()) { ctx.throw_type_error("Cannot convert Symbol to string"); return Value(); }
            else if (arg0_s.is_object() || arg0_s.is_function()) { str = arg0_s.to_property_key(); if (ctx.has_exception()) return Value(); }
            else { str = arg0_s.to_string(); }
            // Step 4: previousLastIndex = Get(rx, "lastIndex")
            Value prev_last_index = this_obj->get_property("lastIndex");
            if (ctx.has_exception()) return Value();
            // Step 5: If SameValue(previousLastIndex, 0) is false, Set(rx, "lastIndex", 0, true)
            auto same_value_zero = [](const Value& v) -> bool {
                if (!v.is_number()) return false;
                double d = v.to_number();
                return d == 0.0 && !std::signbit(d);
            };
            if (!same_value_zero(prev_last_index)) {
                bool ok = this_obj->set_property("lastIndex", Value(0.0));
                if (ctx.has_exception()) return Value();
                if (!ok) { ctx.throw_type_error("Cannot assign to read only property 'lastIndex'"); return Value(); }
            }
            // Step 6: result = RegExpExec(rx, S)
            Value result_val;
            if (!regexp_exec_abstract(ctx, this_obj, str, result_val)) return Value();
            // Step 7: currentLastIndex = Get(rx, "lastIndex")
            Value cur_last_index = this_obj->get_property("lastIndex");
            if (ctx.has_exception()) return Value();
            // Step 8: SameValue(currentLastIndex, previousLastIndex): use signbit for -0 vs +0
            auto same_value = [](const Value& a, const Value& b) -> bool {
                if (a.is_number() && b.is_number()) {
                    double da = a.to_number(), db = b.to_number();
                    if (std::isnan(da) && std::isnan(db)) return true;
                    return da == db && std::signbit(da) == std::signbit(db);
                }
                return false; // simplified for non-numeric case
            };
            if (!same_value(cur_last_index, prev_last_index)) {
                bool ok = this_obj->set_property("lastIndex", prev_last_index);
                if (ctx.has_exception()) return Value();
                if (!ok) { ctx.throw_type_error("Cannot assign to read only property 'lastIndex'"); return Value(); }
            }
            // Steps 9+: return
            if (result_val.is_null() || result_val.is_undefined()) return Value(-1.0);
            if (result_val.is_object() || result_val.is_function()) {
                Object* ro = result_val.is_function() ? static_cast<Object*>(result_val.as_function()) : result_val.as_object();
                Value idx = ro->get_property("index");
                if (ctx.has_exception()) return Value();
                return idx;
            }
            return Value(-1.0);
        }, 1);
    regexp_prototype->set_property("Symbol.search", Value(regexp_sym_search.release()), PropertyAttributes::BuiltinFunction);

    auto regexp_sym_split = ObjectFactory::create_native_function("[Symbol.split]",
        [](Context& ctx, std::span<const Value> args, Value receiver) -> Value {
            Value raw_this_sp = receiver;
            if (!raw_this_sp.is_object() && !raw_this_sp.is_function()) {
                ctx.throw_type_error("RegExp.prototype[Symbol.split] called on non-object");
                return Value();
            }
            Object* this_obj = receiver.as_object_or_null();
            if (!this_obj) { ctx.throw_type_error("RegExp.prototype[Symbol.split] requires an object this value"); return Value(); }
            Value arg0_sp = args.size() > 0 ? args[0] : Value();
            // Borrowed so the matching below runs against the subject's own
            // cell rather than a copy rebuilt from its bytes.
            const String* subject_cell = arg0_sp.is_string() ? arg0_sp.as_string() : nullptr;
            std::string str;
            if (arg0_sp.is_symbol()) { ctx.throw_type_error("Cannot convert Symbol to string"); return Value(); }
            else if (arg0_sp.is_object() || arg0_sp.is_function()) { str = arg0_sp.to_property_key(); if (ctx.has_exception()) return Value(); }
            else { str = arg0_sp.to_string(); }
            // Step 5: flags = ToString(Get(rx, "flags"))
            Value flags_v = this_obj->get_property("flags");
            if (ctx.has_exception()) return Value();
            std::string flags;
            if (flags_v.is_symbol()) { ctx.throw_type_error("Cannot convert Symbol to string"); return Value(); }
            else if (flags_v.is_object() || flags_v.is_function()) {
                flags = flags_v.to_property_key();
                if (ctx.has_exception()) return Value();
            } else {
                flags = flags_v.is_undefined() ? "" : flags_v.to_string();
            }
            bool unicode_matching = flags.find('u') != std::string::npos || flags.find('v') != std::string::npos;
            // Steps 7-8: newFlags = flags + "y" if absent
            std::string new_flags = flags;
            if (new_flags.find('y') == std::string::npos) new_flags += 'y';
            // Steps 3-4 + 9: C = SpeciesConstructor(rx, %RegExp%), splitter = Construct(C, «rx, newFlags»)
            Value ctor_val = this_obj->get_property("constructor");
            if (ctx.has_exception()) return Value();
            if (!ctor_val.is_undefined() && !ctor_val.is_object() && !ctor_val.is_function()) {
                ctx.throw_type_error("constructor property is not an object");
                return Value();
            }
            Function* species_ctor = nullptr;
            if (ctor_val.is_object() || ctor_val.is_function()) {
                Object* ctor_obj = ctor_val.is_function() ? static_cast<Object*>(ctor_val.as_function()) : ctor_val.as_object();
                Symbol* species_sym = Symbol::get_well_known(Symbol::SPECIES);
                if (species_sym) {
                    Value species_val = ctor_obj->get_property(species_sym->to_property_key());
                    if (ctx.has_exception()) return Value();
                    if (!species_val.is_null() && !species_val.is_undefined()) {
                        if (!species_val.is_function()) { ctx.throw_type_error("Species constructor is not a constructor"); return Value(); }
                        species_ctor = species_val.as_function();
                    }
                }
            }
            if (!species_ctor) {
                Object* re_ctor = ctx.get_built_in_object("RegExp");
                if (re_ctor && re_ctor->is_function()) species_ctor = static_cast<Function*>(re_ctor);
            }
            Object* splitter = nullptr;
            if (species_ctor) {
                Value sv = species_ctor->construct(ctx, { Value(this_obj), Value(new_flags) });
                if (ctx.has_exception()) return Value();
                if (sv.is_object()) splitter = sv.as_object();
                else if (sv.is_function()) splitter = static_cast<Object*>(sv.as_function());
            }
            if (!splitter) { ctx.throw_type_error("could not construct splitter"); return Value(); }

            // Step 13: lim = limit undefined ? 2^32-1 : ToUint32(limit)
            Value limit_v = args.size() > 1 ? args[1] : Value();
            uint32_t lim;
            if (limit_v.is_undefined()) {
                lim = 0xFFFFFFFFu;
            } else {
                double lim_d = limit_v.to_number();
                if (ctx.has_exception()) return Value();
                if (std::isnan(lim_d) || std::isinf(lim_d)) {
                    lim = 0;
                } else {
                    double m = std::fmod(std::trunc(lim_d), 4294967296.0);
                    if (m < 0) m += 4294967296.0;
                    lim = static_cast<uint32_t>(m);
                }
            }
            auto result = ObjectFactory::create_array();
            uint32_t length_a = 0;
            if (lim == 0) { result->set_length(0); return Value(result.release()); }

            auto to_object_ptr = [](const Value& v) -> Object* {
                return v.is_function() ? static_cast<Object*>(v.as_function()) : v.as_object();
            };

            // Step 15: size = length of S in UTF-16 code units
            std::u16string str16 = wtf8_to_utf16(str);
            size_t size = str16.size();

            // Step 16: empty string: single exec decides between [] and [""]
            if (size == 0) {
                Value z;
                if (!regexp_exec_abstract(ctx, splitter, str, z, subject_cell)) return Value();
                if (!z.is_null()) { result->set_length(0); return Value(result.release()); }
                result->set_element(0, Value(std::string("")));
                result->set_length(1);
                return Value(result.release());
            }

            auto advance_index = [&](size_t q) -> size_t {
                if (!unicode_matching) return q + 1;
                if (q < str16.size() && str16[q] >= 0xD800 && str16[q] <= 0xDBFF) return q + 2;
                return q + 1;
            };
            auto substring_js = [&](size_t from, size_t to) -> std::string {
                return utf16_to_wtf8(str16.data() + from, to - from);
            };

            // Steps 17-19: p = q = 0; loop while q < size
            size_t p = 0, q = 0;
            while (q < size) {
                bool ok = splitter->set_property("lastIndex", Value(static_cast<double>(q)));
                if (ctx.has_exception()) return Value();
                if (!ok) { ctx.throw_type_error("Cannot assign to read only property 'lastIndex'"); return Value(); }
                Value z;
                if (!regexp_exec_abstract(ctx, splitter, str, z, subject_cell)) return Value();
                if (z.is_null()) {
                    q = advance_index(q);
                    continue;
                }
                Object* m = to_object_ptr(z);
                // e = min(ToLength(Get(splitter, "lastIndex")), size)
                Value li_v = splitter->get_property("lastIndex");
                if (ctx.has_exception()) return Value();
                double e_d = li_v.to_number();
                if (ctx.has_exception()) return Value();
                if (std::isnan(e_d) || e_d < 0) e_d = 0;
                size_t e = e_d >= static_cast<double>(size) ? size : static_cast<size_t>(e_d);
                if (e == p) {
                    q = advance_index(q);
                    continue;
                }
                result->set_element(length_a++, Value(substring_js(p, q)));
                if (length_a == lim) { result->set_length(length_a); return Value(result.release()); }
                p = e;
                Value len_v = m->get_property("length");
                if (ctx.has_exception()) return Value();
                double len_d = len_v.to_number();
                if (ctx.has_exception()) return Value();
                int64_t ncap = (std::isnan(len_d) || len_d < 1) ? 0 : static_cast<int64_t>(len_d) - 1;
                for (int64_t ci = 1; ci <= ncap; ci++) {
                    Value cv = m->get_element(static_cast<uint32_t>(ci));
                    if (ctx.has_exception()) return Value();
                    result->set_element(length_a++, cv);
                    if (length_a == lim) { result->set_length(length_a); return Value(result.release()); }
                }
                q = p;
            }
            // Step 20-22: trailing substring
            result->set_element(length_a++, Value(substring_js(p, size)));
            result->set_length(length_a);
            return Value(result.release());
        }, 2);
    regexp_prototype->set_property("Symbol.split", Value(regexp_sym_split.release()), PropertyAttributes::BuiltinFunction);

    // %RegExpStringIteratorPrototype%: shared prototype for RegExp.prototype[Symbol.matchAll] results.
    // Per-instance state lives in "[[RegExpStringIterator*]]" own properties (checked by has_own_property
    // in `next` so Object.create(iter) -- which inherits but doesn't own them -- correctly TypeErrors).
    Object* regexp_string_iter_proto = nullptr;
    {
        auto proto = ObjectFactory::create_object();
        proto->set_prototype(Iterator::s_iterator_prototype_);
        Symbol* tag_sym = Symbol::get_well_known(Symbol::TO_STRING_TAG);
        if (tag_sym) {
            PropertyDescriptor tag_desc(Value(std::string("RegExp String Iterator")),
                static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
            proto->set_property_descriptor(tag_sym->to_property_key(), tag_desc);
        }
        auto next_fn = ObjectFactory::create_native_function("next",
            [](Context& ctx, std::span<const Value>, Value receiver) -> Value {
                Object* self = receiver.as_object_or_null();
                if (!self || !self->has_own_property("[[RegExpStringIteratorRegExp]]")) {
                    ctx.throw_type_error("%RegExpStringIteratorPrototype%.next called on incompatible receiver");
                    return Value();
                }
                auto make_result = [](const Value& value, bool done) {
                    auto result = ObjectFactory::create_object();
                    result->set_property("value", value);
                    result->set_property("done", Value(done));
                    return Value(result.release());
                };
                if (self->get_own_property("[[RegExpStringIteratorDone]]").to_boolean()) {
                    return make_result(Value(), true);
                }
                Value r_v = self->get_own_property("[[RegExpStringIteratorRegExp]]");
                Object* r_obj = r_v.is_function() ? static_cast<Object*>(r_v.as_function()) : r_v.as_object();
                std::string s = self->get_own_property("[[RegExpStringIteratorString]]").to_string();
                bool global = self->get_own_property("[[RegExpStringIteratorGlobal]]").to_boolean();
                bool full_unicode = self->get_own_property("[[RegExpStringIteratorUnicode]]").to_boolean();

                Value match;
                if (!regexp_exec_abstract(ctx, r_obj, s, match)) return Value();
                if (match.is_null() || match.is_undefined()) {
                    self->set_property("[[RegExpStringIteratorDone]]", Value(true));
                    return make_result(Value(), true);
                }
                if (!global) {
                    self->set_property("[[RegExpStringIteratorDone]]", Value(true));
                    return make_result(match, false);
                }
                Object* m_obj = match.is_function() ? static_cast<Object*>(match.as_function()) : match.as_object();
                Value m0 = m_obj->get_property("0");
                if (ctx.has_exception()) return Value();
                std::string match_str;
                if (m0.is_symbol()) { ctx.throw_type_error("Cannot convert Symbol to string"); return Value(); }
                else if (m0.is_object() || m0.is_function()) { match_str = m0.to_property_key(); if (ctx.has_exception()) return Value(); }
                else { match_str = m0.to_string(); }
                if (match_str.empty()) {
                    Value li_v = r_obj->get_property("lastIndex");
                    if (ctx.has_exception()) return Value();
                    double this_idx = li_v.to_number();
                    if (ctx.has_exception()) return Value();
                    if (std::isnan(this_idx) || this_idx < 0) this_idx = 0;
                    size_t idx_sz = static_cast<size_t>(this_idx);
                    size_t next_idx = idx_sz + 1;
                    if (full_unicode) {
                        std::u16string s16 = wtf8_to_utf16(s);
                        if (idx_sz < s16.size() && s16[idx_sz] >= 0xD800 && s16[idx_sz] <= 0xDBFF) next_idx = idx_sz + 2;
                    }
                    bool ok = r_obj->set_property("lastIndex", Value(static_cast<double>(next_idx)));
                    if (ctx.has_exception()) return Value();
                    if (!ok) { ctx.throw_type_error("Cannot assign to read only property 'lastIndex'"); return Value(); }
                }
                return make_result(match, false);
            }, 0);
        proto->set_property("next", Value(next_fn.release()), PropertyAttributes::BuiltinFunction);
        regexp_string_iter_proto = proto.release();
        // regexp_string_iter_proto is otherwise reachable only through the raw
        // pointer captured by [Symbol.matchAll]'s closure below (invisible to
        // the collector) until some live iterator instance's own prototype_
        // field keeps it reachable transitively -- but nothing creates such an
        // instance until user code actually calls matchAll, so it can be swept
        // between here and then. Pin it onto RegExp.prototype (definitely
        // GC-reachable via the RegExp global) since, per spec, this prototype
        // is intentionally not exposed via any other named property.
        regexp_proto_ptr->set_property("[[StringIteratorProto]]", Value(regexp_string_iter_proto), PropertyAttributes::None);
    }

    // RegExp.prototype[Symbol.matchAll]: per-spec, creates a matcher via SpeciesConstructor then returns iterator.
    {
        auto regexp_sym_matchAll = ObjectFactory::create_native_function("[Symbol.matchAll]",
            [regexp_string_iter_proto](Context& ctx, std::span<const Value> args, Value receiver) -> Value {
                // Step 1: If Type(R) is not Object, throw TypeError.
                Value raw_this_ma = receiver;
                if (!raw_this_ma.is_object() && !raw_this_ma.is_function()) {
                    ctx.throw_type_error("RegExp.prototype[Symbol.matchAll] called on non-object");
                    return Value();
                }
                Object* this_obj = receiver.as_object_or_null();
                if (!this_obj) { ctx.throw_type_error("RegExp.prototype[Symbol.matchAll] requires an object this value"); return Value(); }
                // Step 2: S = ToString(string)
                Value arg0_ma = args.empty() ? Value() : args[0];
                std::string str;
                if (arg0_ma.is_symbol()) { ctx.throw_type_error("Cannot convert Symbol to string"); return Value(); }
                else if (arg0_ma.is_object() || arg0_ma.is_function()) { str = arg0_ma.to_property_key(); if (ctx.has_exception()) return Value(); }
                else { str = arg0_ma.to_string(); }
                // Step 3: C = SpeciesConstructor(rx, %RegExp%)
                Object* spec_ctor = nullptr;
                Value ctor_v = this_obj->get_property("constructor");
                if (ctx.has_exception()) return Value();
                // SpeciesConstructor step 3: if C is not undefined and not an Object, throw TypeError
                if (!ctor_v.is_undefined() && !ctor_v.is_object() && !ctor_v.is_function()) {
                    ctx.throw_type_error("constructor property is not an object");
                    return Value();
                }
                if (ctor_v.is_object() || ctor_v.is_function()) {
                    Object* ctor_obj = ctor_v.is_function() ? static_cast<Object*>(ctor_v.as_function()) : ctor_v.as_object();
                    Symbol* species_sym = Symbol::get_well_known(Symbol::SPECIES);
                    if (species_sym) {
                        Value sp = ctor_obj->get_property(species_sym->to_property_key());
                        if (ctx.has_exception()) return Value();
                        if (sp.is_function()) spec_ctor = static_cast<Object*>(sp.as_function());
                        else if (!sp.is_null() && !sp.is_undefined()) { ctx.throw_type_error("Species is not a constructor"); return Value(); }
                    }
                }
                // Step 4: flags = ToString(Get(rx, "flags"))
                Value flags_v = this_obj->get_property("flags");
                if (ctx.has_exception()) return Value();
                std::string flags;
                if (flags_v.is_object() || flags_v.is_function()) {
                    flags = flags_v.to_property_key();
                    if (ctx.has_exception()) return Value();
                } else {
                    flags = flags_v.is_undefined() ? "" : flags_v.to_string();
                }
                // Step 5: matcher = Construct(C, [rx, flags])
                Value matcher_val;
                if (spec_ctor) {
                    std::vector<Value> ctor_args = { Value(this_obj), Value(flags) };
                    matcher_val = static_cast<Function*>(spec_ctor)->construct(ctx, ctor_args);
                    if (ctx.has_exception()) return Value();
                } else {
                    // Default: %RegExp%(rx, flags) -- creates new regexp with the flags string
                    Object* default_re_ctor = ctx.get_built_in_object("RegExp");
                    if (default_re_ctor && default_re_ctor->is_function()) {
                        std::vector<Value> ctor_args = { Value(this_obj), Value(flags) };
                        matcher_val = static_cast<Function*>(default_re_ctor)->construct(ctx, ctor_args);
                        if (ctx.has_exception()) return Value();
                    } else {
                        matcher_val = Value(this_obj);
                    }
                }
                Value matcher_obj_v = matcher_val;
                // Step 6-7: global, fullUnicode from flags string (not from matcher properties)
                bool global_ma = flags.find('g') != std::string::npos;
                bool full_unicode_ma = flags.find('u') != std::string::npos || flags.find('v') != std::string::npos;
                // Step 8: Set(matcher, "lastIndex", ToLength(Get(rx, "lastIndex")), true)
                Value li_v = this_obj->get_property("lastIndex");
                if (ctx.has_exception()) return Value();
                double li_d = li_v.to_number();
                if (ctx.has_exception()) return Value();
                if (std::isnan(li_d) || li_d < 0) li_d = 0;
                if (matcher_obj_v.is_object() || matcher_obj_v.is_function()) {
                    Object* m_obj = matcher_obj_v.is_function() ? static_cast<Object*>(matcher_obj_v.as_function()) : matcher_obj_v.as_object();
                    m_obj->set_property("lastIndex", Value(li_d));
                    if (ctx.has_exception()) return Value();
                }

                auto iterator = ObjectFactory::create_object();
                iterator->set_prototype(regexp_string_iter_proto);
                iterator->set_property_descriptor("[[RegExpStringIteratorRegExp]]", PropertyDescriptor(matcher_obj_v, PropertyAttributes::None));
                iterator->set_property_descriptor("[[RegExpStringIteratorString]]", PropertyDescriptor(Value(str), PropertyAttributes::None));
                iterator->set_property_descriptor("[[RegExpStringIteratorGlobal]]", PropertyDescriptor(Value(global_ma), PropertyAttributes::None));
                iterator->set_property_descriptor("[[RegExpStringIteratorUnicode]]", PropertyDescriptor(Value(full_unicode_ma), PropertyAttributes::None));
                iterator->set_property_descriptor("[[RegExpStringIteratorDone]]", PropertyDescriptor(Value(false), PropertyAttributes::Writable));
                return Value(iterator.release());
            }, 1);
        Symbol* matchAll_sym = Symbol::get_well_known(Symbol::MATCH_ALL);
        regexp_prototype->set_property(matchAll_sym ? matchAll_sym->to_property_key() : std::string("Symbol.matchAll"),
            Value(regexp_sym_matchAll.release()), PropertyAttributes::BuiltinFunction);
    }

    // Armed only now: every set_property above would otherwise have cleared it
    // on the way in. See Object::regexp_proto_protector_intact.
    Object::watch_regexp_prototype(regexp_prototype.get());
    regexp_constructor->set_property("prototype", Value(regexp_prototype.release()), PropertyAttributes::None);
    {
        PropertyDescriptor len_desc(Value(2.0), PropertyAttributes::Configurable);
        len_desc.set_enumerable(false);
        len_desc.set_writable(false);
        regexp_constructor->set_property_descriptor("length", len_desc);
    }

    // ES2025 RegExp.escape: only accepts strings, no coercion.
    {
        auto escape_fn = ObjectFactory::create_native_function("escape",
            [](Context& ctx, std::span<const Value> args, Value receiver) -> Value {
                Value arg = args.empty() ? Value() : args[0];
                if (!arg.is_string()) {
                    ctx.throw_type_error("RegExp.escape requires a string argument");
                    return Value();
                }
                return Value(regexp_escape_string(arg.to_string()));
            }, 1);
        regexp_constructor->set_property("escape", Value(escape_fn.release()), PropertyAttributes::BuiltinFunction);
    }

    {
        Symbol* species_sym = Symbol::get_well_known(Symbol::SPECIES);
        if (species_sym) {
            auto regexp_species_getter = ObjectFactory::create_native_function("get [Symbol.species]",
                [](Context& ctx, std::span<const Value> args, Value receiver) -> Value {
                    (void)args;
                    Object* self = receiver.as_object_or_null();
                    return self ? Value(self) : Value();
                });
            PropertyDescriptor regexp_species_desc;
            regexp_species_desc.set_getter(regexp_species_getter.release());
            regexp_species_desc.set_enumerable(false);
            regexp_species_desc.set_configurable(true);
            regexp_constructor->set_property_descriptor(species_sym->to_property_key(), regexp_species_desc);
        }
    }

    ctx.register_built_in_object("RegExp", regexp_constructor.release());
}

} // namespace Quanta
