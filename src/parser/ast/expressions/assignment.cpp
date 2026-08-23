/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/parser/AST.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/engine/Engine.h"
#include "quanta/core/engine/CallStack.h"
#include "quanta/core/gc/Collector.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/RegExp.h"
#include "quanta/core/runtime/Async.h"
#include "quanta/core/runtime/BigInt.h"
#include "quanta/core/runtime/Iterator.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/core/runtime/Generator.h"
#include "quanta/core/runtime/ProxyReflect.h"
#include "quanta/core/runtime/String.h"
#include "quanta/core/runtime/Math.h"
#include "quanta/core/runtime/TypedArray.h"
#include "../ast_internal.h"
#include <sstream>
#include <set>
#include <cmath>
#include <climits>
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <unordered_map>
#include <map>
#include <cstdlib>
#include <cstdio>

#ifdef __GNUC__
    #define LIKELY(x) __builtin_expect(!!(x), 1)
    #define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
    #define LIKELY(x) (x)
    #define UNLIKELY(x) (x)
#endif

namespace Quanta {

// Defined in member.cpp (GetSuperBase / super [[Set]]).
Object* resolve_super_base(Context&);
void super_set_on(Context&, Object*, const std::string&, const Value&);

static bool is_anonymous_function_def(const ASTNode* node);

// NamedEvaluation for a pattern default: an anonymous class has to carry its
// inferred name before it is evaluated, because a static initializer can read
// it. Everything else can be renamed afterwards, once the value exists.
static void stamp_pattern_default_class(ASTNode* rhs, const ASTNode* target) {
    if (!rhs || rhs->get_type() != ASTNode::Type::CLASS_DECLARATION) return;
    if (!target || target->get_type() != ASTNode::Type::IDENTIFIER) return;
    auto* cd = static_cast<ClassDeclaration*>(rhs);
    if (cd->is_expression() && cd->get_id() && cd->get_id()->get_name().empty()) {
        cd->set_inferred_name(static_cast<const Identifier*>(target)->get_name());
    }
}



// Helper: recursively perform destructuring assignment from an ObjectLiteral or ArrayLiteral pattern
// A getter run while destructuring throws into Object::current_context_, which
// is not the context we are binding in whenever the two differ -- inside a
// generator's own fiber context, notably. Adopting it here is what stops the
// rest loop below from spinning forever on a throwing `value` getter.
static void adopt_foreign_exception(Context& ctx) {
    if (!ctx.has_exception() && Object::current_context_ && Object::current_context_ != &ctx
            && Object::current_context_->has_exception()) {
        ctx.throw_exception(Object::current_context_->get_exception(), true);
        Object::current_context_->clear_exception();
    }
}

// CopyDataProperties for an object pattern's `...rest`: every own enumerable
// property of the source the pattern did not already take. Shared with
// Op::CopyRestProperties so both paths build the same object.
Value build_rest_object(Context& ctx, const Value& source_value, Object* source_obj,
                        const std::vector<std::string>& assigned_keys) {
auto rest_obj = ObjectFactory::create_object();
if (source_value.is_string()) {
    // For strings, create indexed char properties (spec 12.15.5.2).
    const std::string& raw = source_value.as_string()->str();
    uint32_t char_idx = 0;
    size_t pos = 0;
    while (pos < raw.size()) {
        unsigned char c = static_cast<unsigned char>(raw[pos]);
        size_t cl = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
        if (pos + cl > raw.size()) cl = 1;
        std::string char_key = std::to_string(char_idx);
        bool already_assigned = false;
        for (const auto& ak : assigned_keys) {
            if (ak == char_key) { already_assigned = true; break; }
        }
        if (!already_assigned) {
            PropertyDescriptor cdesc(Value(raw.substr(pos, cl)),
                static_cast<PropertyAttributes>(PropertyAttributes::Writable |
                    PropertyAttributes::Enumerable | PropertyAttributes::Configurable));
            rest_obj->set_property_descriptor(char_key, cdesc);
        }
        pos += cl;
        char_idx++;
    }
} else if (!source_obj) {
    // A number, boolean or symbol source boxes to a wrapper with no own
    // enumerable properties of its own, so the rest object is simply empty.
    // Only null and undefined are an error, and CheckObjectCoercible has
    // already refused those before this runs.
} else if (source_obj->get_type() == Object::ObjectType::Proxy) {
    // get_enumerable_keys()/get_property() don't know about Proxy traps, so go through ownKeys/getOwnPropertyDescriptor/get directly per spec.
    Proxy* proxy = static_cast<Proxy*>(source_obj);
    for (const auto& k : proxy->own_keys_trap()) {
        bool already_assigned = false;
        for (const auto& ak : assigned_keys) {
            if (ak == k) { already_assigned = true; break; }
        }
        if (already_assigned) continue;
        // own_keys_trap() returns symbol keys as their "@@sym:" string encoding; decode back to the real Symbol so traps receive the original key, not its string form.
        Symbol* sym = Symbol::find_by_property_key(k);
        Value key_value = sym ? Value(sym) : Value(k);
        PropertyDescriptor kdesc = proxy->get_own_property_descriptor_trap(key_value);
        if (!kdesc.is_data_descriptor() && !kdesc.is_accessor_descriptor()) continue;
        if (!kdesc.is_enumerable()) continue;
        Value val = proxy->get_trap(key_value);
        if (ctx.has_exception()) return Value();
        PropertyDescriptor rdesc(val,
            static_cast<PropertyAttributes>(PropertyAttributes::Writable |
                PropertyAttributes::Enumerable | PropertyAttributes::Configurable));
        rest_obj->set_property_descriptor(k, rdesc);
    }
} else {
    // For objects: use enumerable keys only (spec excludes non-enumerable).
    auto keys = source_obj->get_enumerable_keys();
    for (const auto& k : keys) {
        bool already_assigned = false;
        for (const auto& ak : assigned_keys) {
            if (ak == k) { already_assigned = true; break; }
        }
        if (!already_assigned) {
            Value val = source_obj->get_property(k);
            if (ctx.has_exception()) return Value();
            PropertyDescriptor rdesc(val,
                static_cast<PropertyAttributes>(PropertyAttributes::Writable |
                    PropertyAttributes::Enumerable | PropertyAttributes::Configurable));
            rest_obj->set_property_descriptor(k, rdesc);
        }
    }
}
    return Value(rest_obj.release());
}

void AssignmentExpression::destructuring_assign(Context& ctx, ASTNode* pattern, const Value& source_value,
                                                DestructureMode mode) {
    if (pattern->get_type() == ASTNode::Type::OBJECT_LITERAL) {
        if (source_value.is_null() || source_value.is_undefined()) {
            ctx.throw_type_error("Cannot destructure " + std::string(source_value.is_null() ? "null" : "undefined"));
            return;
        }
        Object* source_obj = nullptr;
        if (source_value.is_object()) source_obj = source_value.as_object();
        else if (source_value.is_function()) source_obj = static_cast<Object*>(source_value.as_function());
        else if (source_value.is_string()) {
            // ES6: Box string with proper prototype chain
            auto wrapper = ObjectFactory::create_string(source_value.as_string()->str());
            Value ctor = ctx.get_binding("String");
            if (ctor.is_function()) {
                Value proto_val = ctor.as_function()->get_property("prototype");
                if (proto_val.is_object()) {
                    wrapper->set_prototype(proto_val.as_object());
                }
            }
            source_obj = wrapper.release();
        } else if (source_value.is_number() || source_value.is_boolean()) {
            // ES6: Box number/boolean with proper prototype chain
            std::string ctor_name = source_value.is_number() ? "Number" : "Boolean";
            Value ctor = ctx.get_binding(ctor_name);
            if (ctor.is_function()) {
                Value proto_val = ctor.as_function()->get_property("prototype");
                if (proto_val.is_object()) {
                    auto wrapper = ObjectFactory::create_object();
                    wrapper->set_prototype(proto_val.as_object());
                    source_obj = wrapper.release();
                }
            }
            if (!source_obj) {
                auto* wrapper = ObjectFactory::create_object().release();
                source_obj = wrapper;
            }
        } else if (source_value.is_symbol()) {
            // Symbols are object-coercible; ToObject(symbol) succeeds
            auto sym_wrapper = ObjectFactory::create_object();
            source_obj = sym_wrapper.release();
        } else if (source_value.is_bigint()) {
            // BigInts are object-coercible; ToObject(bigint) succeeds
            auto bigint_wrapper = ObjectFactory::create_object();
            Value ctor = ctx.get_binding("BigInt");
            if (ctor.is_function()) {
                Value proto_val = ctor.as_function()->get_property("prototype");
                if (proto_val.is_object()) bigint_wrapper->set_prototype(proto_val.as_object());
            }
            source_obj = bigint_wrapper.release();
        }
        if (!source_obj) {
            ctx.throw_type_error("Cannot destructure non-object value");
            return;
        }

        auto* obj_lit = static_cast<ObjectLiteral*>(pattern);
        std::vector<std::string> assigned_keys;

        for (const auto& prop : obj_lit->get_properties()) {
            // Handle rest element: {...rest}
            if (prop->type == ObjectLiteral::PropertyType::Value &&
                prop->value && prop->value->get_type() == ASTNode::Type::SPREAD_ELEMENT) {
                auto* spread = static_cast<SpreadElement*>(prop->value.get());
                ASTNode* rest_target = spread->get_argument();
                // Create object with remaining enumerable own properties.
                // Use set_property_descriptor with explicit WEC attrs so that numeric keys
                // whose getter returns undefined still appear in Object.keys (they'd be lost
                // as "holes" if stored via set_property/set_element with undefined value).
                Value rest_value = build_rest_object(ctx, source_value, source_obj, assigned_keys);
                if (ctx.has_exception()) return;
                assign_to_target(ctx, rest_target, rest_value, nullptr, nullptr, mode);
                if (ctx.has_exception()) return;
                continue;
            }

            // Get property name from key
            std::string prop_name;
            if (prop->computed) {
                Value key_val = prop->key->evaluate_compiled(ctx);
                if (ctx.has_exception()) return;
                // ToPropertyKey, not a raw stringify: an object key's own
                // @@toPrimitive/toString has to run.
                prop_name = key_val.to_property_key();
                if (ctx.has_exception()) return;
            } else if (prop->key->get_type() == ASTNode::Type::IDENTIFIER) {
                prop_name = static_cast<Identifier*>(prop->key.get())->get_name();
            } else if (prop->key->get_type() == ASTNode::Type::STRING_LITERAL) {
                prop_name = static_cast<StringLiteral*>(prop->key.get())->get_value();
            } else if (prop->key->get_type() == ASTNode::Type::NUMBER_LITERAL) {
                double kv = static_cast<NumberLiteral*>(prop->key.get())->get_value();
                if (kv == std::floor(kv) && kv >= static_cast<double>(LLONG_MIN) && kv <= static_cast<double>(LLONG_MAX)) {
                    prop_name = std::to_string(static_cast<long long>(kv));
                } else {
                    std::ostringstream koss;
                    koss << kv;
                    prop_name = koss.str();
                }
            }
            assigned_keys.push_back(prop_name);

            // Determine assignment target. Resolved before the property is read:
            // KeyedDestructuringAssignmentEvaluation evaluates the target's
            // reference (step 1) ahead of GetV (step 2), so a member target's
            // object and computed key run before the source's getter does. The
            // array path already works this way.
            ASTNode* target = prop->shorthand ? prop->key.get() : prop->value.get();
            AssignmentExpression* default_assign = nullptr;
            if (prop->shorthand && prop->value &&
                prop->value->get_type() == ASTNode::Type::ASSIGNMENT_EXPRESSION) {
                default_assign = static_cast<AssignmentExpression*>(prop->value.get());
                target = default_assign->left_.get();
            } else if (!prop->shorthand && target &&
                       target->get_type() == ASTNode::Type::ASSIGNMENT_EXPRESSION) {
                default_assign = static_cast<AssignmentExpression*>(target);
                target = default_assign->left_.get();
            }

            Value member_obj, member_key;
            bool has_member_ref = false;
            if (mode == DestructureMode::Assign && target &&
                target->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
                auto* mem = static_cast<MemberExpression*>(target);
                member_obj = mem->get_object()->evaluate_compiled(ctx);
                if (ctx.has_exception()) return;
                if (mem->is_computed()) {
                    member_key = mem->get_property()->evaluate_compiled(ctx);
                    if (ctx.has_exception()) return;
                }
                has_member_ref = true;
            }

            Value prop_value = source_obj->get_property(prop_name);
            // Getter may throw into Object::current_context_ rather than ctx
            if (!ctx.has_exception() && Object::current_context_ && Object::current_context_ != &ctx
                    && Object::current_context_->has_exception()) {
                ctx.throw_exception(Object::current_context_->get_exception(), true);
                Object::current_context_->clear_exception();
            }
            if (ctx.has_exception()) return;

            // `{a = d}` and `{key: target = d}` both reach here with the
            // default's node kept from the unwrapping above.
            if (default_assign && prop_value.is_undefined()) {
                stamp_pattern_default_class(default_assign->right_.get(), target);
                prop_value = default_assign->right_->evaluate_compiled(ctx);
                if (ctx.has_exception()) return;
                if (prop_value.is_function() &&
                        is_anonymous_function_def(default_assign->right_.get()) &&
                        target && target->get_type() == ASTNode::Type::IDENTIFIER) {
                    Function* fn = prop_value.as_function();
                    if (fn->get_name().empty() || fn->get_name() == "<arrow>") {
                        fn->set_name(static_cast<Identifier*>(target)->get_name());
                    }
                }
            }

            assign_to_target(ctx, target, prop_value,
                             has_member_ref ? &member_obj : nullptr,
                             has_member_ref ? &member_key : nullptr, mode);
            if (ctx.has_exception()) return;
        }
    } else if (pattern->get_type() == ASTNode::Type::ARRAY_LITERAL) {
        if (source_value.is_null() || source_value.is_undefined()) {
            ctx.throw_type_error("Cannot destructure " + std::string(source_value.is_null() ? "null" : "undefined"));
            return;
        }
        Object* source_arr = nullptr;
        uint32_t source_len = 0;
        bool is_string_source = false;
        std::string str_source;
        // Deferred IteratorClose: must happen AFTER default value evaluation (not before).
        Value deferred_iter_close_obj;
        bool deferred_iter_close_needed = false;

        // MemberExpression targets get their reference evaluated once up front, before iterator.next() -- cache those values per element index so the assignment below reuses them instead of re-evaluating.
        size_t elem_count_for_precompute = static_cast<ArrayLiteral*>(pattern)->get_elements().size();
        std::vector<Value> precomputed_member_obj(elem_count_for_precompute);
        std::vector<Value> precomputed_member_key(elem_count_for_precompute);
        std::vector<bool> has_precomputed_member(elem_count_for_precompute, false);

        // For codepoint-aware string destructuring
        std::vector<std::string> str_codepoints;

        if (source_value.is_string()) {
            is_string_source = true;
            str_source = source_value.as_string()->str();
            // Split into UTF-8 codepoints
            size_t pos = 0;
            while (pos < str_source.length()) {
                unsigned char ch = static_cast<unsigned char>(str_source[pos]);
                size_t cl = 1;
                if (ch >= 0xF0) cl = 4;
                else if (ch >= 0xE0) cl = 3;
                else if (ch >= 0xC0) cl = 2;
                if (pos + cl > str_source.length()) cl = 1;
                str_codepoints.push_back(str_source.substr(pos, cl));
                pos += cl;
            }
            source_len = static_cast<uint32_t>(str_codepoints.size());
        } else if (source_value.is_number() || source_value.is_boolean() || source_value.is_symbol()) {
            // Primitives (number/boolean/symbol) are not iterable -- throw TypeError
            ctx.throw_type_error("Cannot destructure a non-iterable value");
            return;
        } else if (source_value.is_object() || source_value.is_function()) {
            source_arr = source_value.is_function()
                ? static_cast<Object*>(source_value.as_function())
                : source_value.as_object();
            // For arrays: verify Symbol.iterator is callable (deleted iterator -> TypeError)
            if (source_arr && source_arr->is_array()) {
                Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
                if (iter_sym) {
                    Value iter_method = source_arr->get_property(iter_sym->to_property_key());
                    if (!iter_method.is_function()) {
                        ctx.throw_type_error("Cannot destructure: Symbol.iterator is not callable");
                        return;
                    }
                }
            }
            // Everything that is not a plain Array goes through the real iterator
            // protocol -- and so does an Array once anything has replaced
            // @@iterator or %ArrayIteratorPrototype%.next, which is what the
            // protector tracks. Skipping it there would ignore a patched
            // iterator, the same deviation the spread path had.
            if (source_arr && (!source_arr->is_array() || !Object::array_iterator_protector_intact())) {
                Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
                if (iter_sym) {
                    Value iter_method = source_arr->get_property(iter_sym->to_property_key());
                    // Array destructuring is defined in terms of GetIterator, so a
                    // source without a callable @@iterator is simply not iterable --
                    // an array-like shape does not stand in for one.
                    if (!iter_method.is_function()) {
                        ctx.throw_type_error("Cannot destructure a non-iterable value");
                        return;
                    }
                    if (iter_method.is_function()) {
                        Value iter_obj = iter_method.as_function()->call(ctx, {}, source_value);
                        if (!ctx.has_exception() && iter_obj.is_object()) {
                            Value next_fn = iter_obj.as_object()->get_property("next");
                            {
                                // next() may not be callable (e.g. a custom iterator object that only implements return()) 
                                // per spec, that's only discovered (and throws) when next() is actually invoked, not before. 
                                // Target reference evaluation for each element still happens first.
                                auto call_next = [&]() -> Value {
                                    if (!next_fn.is_function()) {
                                        ctx.throw_type_error("Iterator's next() is not callable");
                                        return Value();
                                    }
                                    return next_fn.as_function()->call(ctx, {}, iter_obj);
                                };
                                // Determine how many elements we need from pattern
                                auto* arr_lit_check = static_cast<ArrayLiteral*>(pattern);
                                const auto& elems_check = arr_lit_check->get_elements();
                                bool has_rest = false;
                                size_t needed = elems_check.size();
                                for (size_t ei = 0; ei < elems_check.size(); ei++) {
                                    if (elems_check[ei] && elems_check[ei]->get_type() == ASTNode::Type::SPREAD_ELEMENT) {
                                        has_rest = true;
                                        break;
                                    }
                                }
                                // IteratorClose per spec 7.4.6:
                                // - If there was a pending exception (throw completion): call return(),
                                //   discard its result, restore original exception
                                // - If no pending exception (normal completion): call return(),
                                //   propagate its errors, TypeError if result is not Object
                                auto close_iter = [&]() {
                                    bool had_exception = ctx.has_exception();
                                    Value saved_exc = ctx.get_exception();
                                    ctx.clear_exception();
                                    Value ret_m = iter_obj.as_object()->get_property("return");
                                    if (!ret_m.is_function()) {
                                        if (had_exception) ctx.throw_exception(saved_exc, true);
                                        return;
                                    }
                                    Value inner = ret_m.as_function()->call(ctx, {}, iter_obj);
                                    if (had_exception) {
                                        ctx.clear_exception();
                                        ctx.throw_exception(saved_exc, true);
                                    } else if (!ctx.has_exception() && !inner.is_object()) {
                                        ctx.throw_type_error("Iterator return() must return an Object");
                                    }
                                };

                                // Spec 7.4.6 IteratorClose with a return completion (the generator being destructured into was itself resumed via .return() while
                                // call iter_obj's return(), propagate its throw or a TypeError for a non-Object result, otherwise rethrow the original GeneratorReturnException.
                                auto close_iter_for_generator_return = [&]() {
                                    Value ret_m = iter_obj.as_object()->get_property("return");
                                    if (ret_m.is_function()) {
                                        Value inner = ret_m.as_function()->call(ctx, {}, iter_obj);
                                        if (ctx.has_exception()) return;
                                        if (!inner.is_object()) {
                                            ctx.throw_type_error("Iterator return() result must be an Object");
                                            return;
                                        }
                                    }
                                    throw;
                                };

                                if (has_rest) {
                                    const auto& elems_r = arr_lit_check->get_elements();
                                    auto temp = ObjectFactory::create_array(0);
                                    uint32_t cnt = 0;
                                    bool iter_done = false;
                                    ASTNode* rest_target = nullptr;

                                    // Spec: leading (non-rest) elements consume the iterator first, each evaluating its target reference before calling next()
                                    // same ordering as the non-rest loop below.
                                    // Only once all of them are done do we reach the rest element itself.
                                    for (size_t ri = 0; ri < elems_r.size() && !iter_done; ri++) {
                                        if (elems_r[ri] && elems_r[ri]->get_type() == ASTNode::Type::SPREAD_ELEMENT) {
                                            rest_target = static_cast<SpreadElement*>(elems_r[ri].get())->get_argument();
                                            break;
                                        }
                                        ASTNode* tgt = elems_r[ri] ? elems_r[ri].get() : nullptr;
                                        if (tgt && tgt->get_type() == ASTNode::Type::ASSIGNMENT_EXPRESSION) {
                                            tgt = static_cast<AssignmentExpression*>(tgt)->left_.get();
                                        }
                                        if (tgt && tgt->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
                                            auto* mem = static_cast<MemberExpression*>(tgt);
                                            try {
                                                mem->get_object()->evaluate_compiled(ctx);
                                            } catch (const GeneratorReturnException&) {
                                                close_iter_for_generator_return();
                                                return;
                                            }
                                            if (ctx.has_exception()) { close_iter(); return; }
                                            if (mem->is_computed()) {
                                                try {
                                                    mem->get_property()->evaluate_compiled(ctx);
                                                } catch (const GeneratorReturnException&) {
                                                    close_iter_for_generator_return();
                                                    return;
                                                }
                                                if (ctx.has_exception()) { close_iter(); return; }
                                            }
                                        }
                                        // Per spec, if next() throws, do NOT close the iterator
                                        // (no IteratorClose on abrupt next).
                                        Value res = call_next();
                                        if (ctx.has_exception()) return;
                                        if (!res.is_object()) { iter_done = true; break; }
                                        Value done_v = res.as_object()->get_property("done");
                                        adopt_foreign_exception(ctx);
                                        if (ctx.has_exception()) return;
                                        if (done_v.to_boolean()) { iter_done = true; break; }
                                        Value val_v = res.as_object()->get_property("value");
                                        adopt_foreign_exception(ctx);
                                        if (ctx.has_exception()) return;
                                        temp->set_element(cnt++, val_v);
                                    }

                                    // Spec AssignmentRestElement step 1
                                    // for a MemberExpression target,evaluate the reference (object + key) BEFORE consuming the rest of the iterator.
                                    // ReturnIfAbrupt: if evaluation throws, close.
                                    if (!iter_done && rest_target && rest_target->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
                                        auto* mem = static_cast<MemberExpression*>(rest_target);
                                        try {
                                            mem->get_object()->evaluate_compiled(ctx);
                                        } catch (const GeneratorReturnException&) {
                                            close_iter_for_generator_return();
                                            return;
                                        }
                                        if (ctx.has_exception()) { close_iter(); return; }
                                        if (mem->is_computed()) {
                                            try {
                                                mem->get_property()->evaluate_compiled(ctx);
                                            } catch (const GeneratorReturnException&) {
                                                close_iter_for_generator_return();
                                                return;
                                            }
                                            if (ctx.has_exception()) { close_iter(); return; }
                                        }
                                    }

                                    // Rest: collect all remaining into temp array
                                    if (!iter_done) {
                                        for (;;) {
                                            Collector::safepoint();
                                            // Per spec, if next() throws, do NOT close the iterator
                                            // (no IteratorClose on abrupt next).
                                            Value res = call_next();
                                            if (ctx.has_exception()) return;
                                            if (!res.is_object()) { iter_done = true; break; }
                                            Value done_v = res.as_object()->get_property("done");
                                            adopt_foreign_exception(ctx);
                                            if (ctx.has_exception()) return;
                                            if (done_v.to_boolean()) { iter_done = true; break; }
                                            Value val_v = res.as_object()->get_property("value");
                                            adopt_foreign_exception(ctx);
                                            if (ctx.has_exception()) return;
                                            temp->set_element(cnt++, val_v);
                                        }
                                    }
                                    if (!iter_done) { close_iter(); }
                                    temp->set_length(cnt);
                                    source_arr = temp.release();
                                } else {
                                    // Spec: for each non-rest element, evaluate target ref FIRST,
                                    // then call next(). This allows member expression key errors
                                    // to occur before iterator.next() is called.
                                    auto temp = ObjectFactory::create_array(0);
                                    uint32_t cnt = 0;
                                    bool iter_done = false;
                                    const auto& elems_c = arr_lit_check->get_elements();

                                    for (size_t ei = 0; ei < needed && !iter_done; ei++) {
                                        const auto& elem_c = elems_c[ei];
                                        // Get actual target (skip elisions)
                                        ASTNode* tgt = elem_c ? elem_c.get() : nullptr;
                                        // Unwrap default
                                        if (tgt && tgt->get_type() == ASTNode::Type::ASSIGNMENT_EXPRESSION) {
                                            tgt = static_cast<AssignmentExpression*>(tgt)->left_.get();
                                        }
                                        // If target is MemberExpression: evaluate base+key NOW (before next())
                                        // so that errors in key evaluation prevent next() from being called
                                        if (tgt && tgt->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
                                            auto* mem = static_cast<MemberExpression*>(tgt);
                                            try {
                                                precomputed_member_obj[ei] = mem->get_object()->evaluate_compiled(ctx);
                                            } catch (const GeneratorReturnException&) {
                                                close_iter_for_generator_return();
                                                return;
                                            }
                                            if (ctx.has_exception()) { close_iter(); return; }
                                            has_precomputed_member[ei] = true;
                                            if (mem->is_computed()) {
                                                try {
                                                    precomputed_member_key[ei] = mem->get_property()->evaluate_compiled(ctx);
                                                } catch (const GeneratorReturnException&) {
                                                    close_iter_for_generator_return();
                                                    return;
                                                }
                                                if (ctx.has_exception()) { close_iter(); return; }
                                            }
                                        }
                                        // Now call next(). Per spec, if next()/done/value throw,
                                        // do NOT close the iterator (no IteratorClose on abrupt next).
                                        Value res = call_next();
                                        if (ctx.has_exception()) return;
                                        if (!res.is_object()) { iter_done = true; break; }
                                        Value done_v = res.as_object()->get_property("done");
                                        adopt_foreign_exception(ctx);
                                        if (ctx.has_exception()) return;
                                        if (done_v.to_boolean()) { iter_done = true; break; }
                                        Value val_v = res.as_object()->get_property("value");
                                        adopt_foreign_exception(ctx);
                                        if (ctx.has_exception()) return;
                                        temp->set_element(cnt++, val_v);
                                    }
                                    if (!iter_done) {
                                        // Defer IteratorClose until after default value evaluation.
                                        deferred_iter_close_needed = true;
                                        deferred_iter_close_obj = iter_obj;
                                    }
                                    temp->set_length(cnt);
                                    source_arr = temp.release();
                                }
                            }
                        }
                    }
                }
            }
            source_len = source_arr->get_length();
        }

        auto* arr_lit = static_cast<ArrayLiteral*>(pattern);
        const auto& elements = arr_lit->get_elements();

        for (size_t i = 0; i < elements.size(); i++) {
            const auto& elem = elements[i];
            if (!elem) continue; // hole/elision

            // Handle rest element: [...rest]
            if (elem->get_type() == ASTNode::Type::SPREAD_ELEMENT) {
                auto* spread = static_cast<SpreadElement*>(elem.get());
                ASTNode* rest_target = spread->get_argument();
                auto rest_arr = ObjectFactory::create_array(0);
                uint32_t rest_idx = 0;
                for (uint32_t j = static_cast<uint32_t>(i); j < source_len; j++) {
                    Value val;
                    if (is_string_source) {
                        val = (j < str_codepoints.size()) ? Value(str_codepoints[j]) : Value();
                    } else {
                        val = source_arr->get_element(j);
                    }
                    rest_arr->set_element(rest_idx++, val);
                }
                rest_arr->set_length(rest_idx);
                assign_to_target(ctx, rest_target, Value(rest_arr.release()), nullptr, nullptr, mode);
                break;
            }

            Value elem_value;
            if (is_string_source) {
                elem_value = (i < str_codepoints.size()) ? Value(str_codepoints[i]) : Value();
            } else if (source_arr) {
                elem_value = (i < source_len) ? source_arr->get_element(static_cast<uint32_t>(i)) : Value();
            }

            ASTNode* target = elem.get();

            // Check for default: element is AssignmentExpression like (a = default)
            if (target->get_type() == ASTNode::Type::ASSIGNMENT_EXPRESSION) {
                auto* assign = static_cast<AssignmentExpression*>(target);
                ASTNode* lhs = assign->left_.get();
                if (elem_value.is_undefined()) {
                    stamp_pattern_default_class(assign->right_.get(), lhs);
                    // Default may yield/return. On GeneratorReturnException, close iter first.
                    if (deferred_iter_close_needed) {
                        try {
                            elem_value = assign->right_->evaluate_compiled(ctx);
                        } catch (const GeneratorReturnException&) {
                            // Spec 7.4.6 IteratorClose with return completion:
                            // Call iterator.return(); propagate its throw or TypeError for non-Object.
                            // If it succeeds and returns Object, propagate the original return.
                            Value ret_m = deferred_iter_close_obj.as_object()->get_property("return");
                            if (ret_m.is_function()) {
                                Value inner = ret_m.as_function()->call(ctx, {}, deferred_iter_close_obj);
                                if (ctx.has_exception()) {
                                    // iterator.return() threw -- propagate that throw.
                                    return;
                                }
                                if (!inner.is_object()) {
                                    ctx.throw_type_error("Iterator return() result must be an Object");
                                    return;
                                }
                            }
                            throw; // iterator.return() ok -- propagate original return
                        }
                    } else {
                        elem_value = assign->right_->evaluate_compiled(ctx);
                    }
                    // Break (not return) so the deferred IteratorClose below still runs -- it preserves this exception over any of its own.
                    if (ctx.has_exception()) break;
                    if (elem_value.is_function() && is_anonymous_function_def(assign->right_.get()) &&
                            lhs && lhs->get_type() == ASTNode::Type::IDENTIFIER) {
                        Function* fn = elem_value.as_function();
                        if (fn->get_name().empty() || fn->get_name() == "<arrow>") {
                            fn->set_name(static_cast<Identifier*>(lhs)->get_name());
                        }
                    }
                }
                target = lhs;
            }

            if (i < has_precomputed_member.size() && has_precomputed_member[i]) {
                assign_to_target(ctx, target, elem_value, &precomputed_member_obj[i], &precomputed_member_key[i], mode);
            } else {
                assign_to_target(ctx, target, elem_value, nullptr, nullptr, mode);
            }
            if (ctx.has_exception()) break;
        }
        // Spec ArrayAssignmentPattern step 5: IteratorClose if iterator not exhausted.
        if (deferred_iter_close_needed && deferred_iter_close_obj.is_object()) {
            bool had_exc = ctx.has_exception();
            Value saved_exc = had_exc ? ctx.get_exception() : Value();
            if (had_exc) ctx.clear_exception();
            Value ret_m = deferred_iter_close_obj.as_object()->get_property("return");
            if (ret_m.is_function()) {
                Value inner = ret_m.as_function()->call(ctx, {}, deferred_iter_close_obj);
                if (had_exc) {
                    // Throw completion: suppress inner errors, restore original.
                    ctx.clear_exception();
                    ctx.throw_exception(saved_exc, true);
                } else if (!ctx.has_exception() && !inner.is_object()) {
                    ctx.throw_type_error("Iterator return() result must be an Object");
                }
            } else if (had_exc) {
                ctx.throw_exception(saved_exc, true);
            }
        }
    }
}

// Helper: assign a value to a target node (Identifier, MemberExpression, or nested pattern)
void AssignmentExpression::assign_to_target(Context& ctx, ASTNode* target, const Value& value,
                                              const Value* precomputed_obj, const Value* precomputed_key,
                                              DestructureMode mode) {
    if (!target) return;

    if (target->get_type() == ASTNode::Type::IDENTIFIER) {
        std::string name = static_cast<Identifier*>(target)->get_name();
        // A declaration binds a fresh name; only the assignment form writes
        // through an existing reference.
        if (mode != DestructureMode::Assign) {
            if (mode == DestructureMode::Let) {
                ctx.create_lexical_binding(name, value, true);
            } else if (mode == DestructureMode::Const) {
                ctx.create_lexical_binding(name, value, false);
            } else if (!ctx.has_binding(name)) {
                ctx.create_binding(name, value, true);
            } else if (!ctx.set_binding(name, value) && ctx.is_strict_const(name)) {
                ctx.throw_type_error("Assignment to constant variable '" + name + "'");
            }
            return;
        }
        // Resolve once and ask that environment. This used to gate on
        // has_binding and then walk again for the dead zone, which both cost a
        // second HasBinding on any object environment along the way and got
        // the answer wrong: a `with` object binding the name ends the search,
        // so an outer `let` it shadows is not consulted.
        Environment* ref_env = ctx.find_binding_env(name);
        if (ctx.has_exception()) return;
        if (ref_env) {
            if (ref_env->binding_in_tdz(name)) {
                ctx.throw_reference_error("Cannot access '" + name + "' before initialization");
                return;
            }
            bool ok = ctx.set_binding(name, value);
            if (!ok) {
                if (ctx.is_strict_mode() || ctx.is_strict_const(name)) {
                    ctx.throw_type_error("Assignment to constant variable '" + name + "'");
                }
            }
        } else {
            if (ctx.is_strict_mode()) {
                ctx.throw_reference_error("'" + name + "' is not defined");
                return;
            }
            // ES5 8.7.2: PutValue on unresolvable reference -- always sets a property
            // on the global object (creating an implicit global var), regardless of
            // which scope the assignment executes in.
            Object* global = ctx.get_global_object();
            if (global) global->set_property(name, value);
        }
    } else if (target->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
        auto* member = static_cast<MemberExpression*>(target);
        // Reuse a reference already evaluated earlier (e.g. destructuring's evaluate-target-before-next() ordering) instead of re-evaluating and re-triggering side effects.
        Value obj_val = precomputed_obj ? *precomputed_obj : member->get_object()->evaluate_compiled(ctx);
        if (ctx.has_exception()) return;
        if (obj_val.is_null() || obj_val.is_undefined()) {
            ctx.throw_type_error(std::string("Cannot set properties of ") +
                (obj_val.is_null() ? "null" : "undefined"));
            return;
        }
        if (obj_val.is_object_like()) {
            Object* obj = obj_val.is_object() ? obj_val.as_object()
                                              : static_cast<Object*>(obj_val.as_function());
            std::string prop_name;
            if (member->is_computed()) {
                Value key_val = precomputed_key ? *precomputed_key : member->get_property()->evaluate_compiled(ctx);
                if (ctx.has_exception()) return;
                prop_name = key_val.to_property_key();
                if (ctx.has_exception()) return;
            } else if (member->get_property()->get_type() == ASTNode::Type::IDENTIFIER) {
                prop_name = static_cast<Identifier*>(member->get_property())->get_name();
            }
            // Fields are stored under a qualified key (see resolve_private_storage_key); fall back to the bare key for methods/getters/setters, which live unqualified on the prototype.
            // Only the literal `.#name` syntax is a private reference.
            if (!member->is_computed() && !prop_name.empty() && prop_name[0] == '#') {
                // require_exists=true: unlike the normal assignment path (binary.cpp/
                // assignment.cpp's identifier-LHS branch), this destructuring target
                // has no separate follow-up "does the slot actually exist" check of
                // its own, so private_brand_check must do that check itself here --
                // for fields specifically that's the *only* real check (no brand).
                if (!private_brand_check(ctx, obj, prop_name, true)) {
                    ctx.throw_type_error("Cannot write private member " + prop_name + " to an object whose class did not declare it");
                    return;
                }
                std::string qualified = resolve_private_storage_key(prop_name, obj);
                if (obj->has_private_slot(qualified)) {
                    // Raw write: private state bypasses [[Set]] (Proxy traps,
                    // exotic overrides) entirely.
                    obj->set_private_slot_value(qualified, value);
                    return;
                }
            }
            obj->ordinary_set(prop_name, value);
        }
    } else if (target->get_type() == ASTNode::Type::OBJECT_LITERAL ||
               target->get_type() == ASTNode::Type::ARRAY_LITERAL) {
        // Nested destructuring
        destructuring_assign(ctx, target, value, mode);
    }
}

std::string AssignmentExpression::to_string() const {
    std::string op_str;
    switch (operator_) {
        case Operator::ASSIGN: op_str = " = "; break;
        case Operator::PLUS_ASSIGN: op_str = " += "; break;
        case Operator::MINUS_ASSIGN: op_str = " -= "; break;
        case Operator::MUL_ASSIGN: op_str = " *= "; break;
        case Operator::DIV_ASSIGN: op_str = " /= "; break;
        case Operator::MOD_ASSIGN: op_str = " %= "; break;
        case Operator::LOGICAL_AND_ASSIGN: op_str = " &&= "; break;
        case Operator::LOGICAL_OR_ASSIGN: op_str = " ||= "; break;
        case Operator::NULLISH_ASSIGN: op_str = " ??= "; break;
        default: op_str = " op= "; break;
    }
    return left_->to_string() + op_str + right_->to_string();
}

std::unique_ptr<ASTNode> AssignmentExpression::clone() const {
    return std::make_unique<AssignmentExpression>(
        left_->clone(), operator_, right_->clone(), start_, end_, lhs_is_paren_
    );
}


static bool is_anonymous_function_def(const ASTNode* node) {
    if (!node) return false;
    auto t = node->get_type();
    return t == ASTNode::Type::FUNCTION_EXPRESSION ||
           t == ASTNode::Type::ARROW_FUNCTION_EXPRESSION ||
           t == ASTNode::Type::ASYNC_FUNCTION_EXPRESSION ||
           t == ASTNode::Type::CLASS_DECLARATION;
}

// Walks the pattern literal, which is the same shape AssignmentExpression's
// own destructuring walks: an ObjectLiteral property's value is the target
// (an Identifier, a nested literal, or an AssignmentExpression carrying a
// default), an ArrayLiteral element likewise, with SpreadElement for rest.
static void walk_pattern_targets(const ASTNode* pattern,
                                 const std::function<void(const ASTNode*)>& on_target,
                                 const std::function<void(const ASTNode*)>& on_expression) {
    if (!pattern) return;
    auto visit_target = [&](const ASTNode* t) {
        if (!t) return;
        if (t->get_type() == ASTNode::Type::ASSIGNMENT_EXPRESSION) {
            const auto* ae = static_cast<const AssignmentExpression*>(t);
            if (on_expression && ae->get_right()) on_expression(ae->get_right());
            walk_pattern_targets(ae->get_left(), on_target, on_expression);
            if (ae->get_left() && ae->get_left()->get_type() != ASTNode::Type::OBJECT_LITERAL &&
                ae->get_left()->get_type() != ASTNode::Type::ARRAY_LITERAL) {
                on_target(ae->get_left());
            }
            return;
        }
        if (t->get_type() == ASTNode::Type::OBJECT_LITERAL ||
            t->get_type() == ASTNode::Type::ARRAY_LITERAL) {
            walk_pattern_targets(t, on_target, on_expression);
            return;
        }
        if (t->get_type() == ASTNode::Type::SPREAD_ELEMENT) {
            walk_pattern_targets(static_cast<const SpreadElement*>(t)->get_argument(),
                                 on_target, on_expression);
            on_target(static_cast<const SpreadElement*>(t)->get_argument());
            return;
        }
        on_target(t);
    };

    if (pattern->get_type() == ASTNode::Type::OBJECT_LITERAL) {
        for (const auto& prop : static_cast<const ObjectLiteral*>(pattern)->get_properties()) {
            if (prop->computed && prop->key && on_expression) on_expression(prop->key.get());
            visit_target(prop->value ? prop->value.get() : nullptr);
        }
    } else if (pattern->get_type() == ASTNode::Type::ARRAY_LITERAL) {
        for (const auto& el : static_cast<const ArrayLiteral*>(pattern)->get_elements()) {
            if (el) visit_target(el.get());
        }
    }
}

void DestructuringAssignment::collect_bound_names(std::vector<std::string>& out) const {
    walk_pattern_targets(pattern_literal_.get(),
        [&](const ASTNode* t) {
            if (t && t->get_type() == ASTNode::Type::IDENTIFIER) {
                const std::string& n = static_cast<const Identifier*>(t)->get_name();
                if (!n.empty()) out.push_back(n);
            }
        },
        nullptr);
}

void DestructuringAssignment::for_each_expression(const std::function<void(const ASTNode*)>& fn) const {
    walk_pattern_targets(pattern_literal_.get(),
        [&](const ASTNode* t) {
            // A member-expression target (`[o.x] = v`) is itself an expression.
            if (t && t->get_type() != ASTNode::Type::IDENTIFIER) fn(t);
        },
        fn);
}

Value DestructuringAssignment::evaluate_with_value(Context& ctx, const Value& source_value,
                                                   bool as_lexical, bool is_const) {
    // One binder for both destructuring forms: the pattern literal is exactly
    // the shape AssignmentExpression::destructuring_assign already walks, so a
    // declaration differs from an assignment only in how leaves are bound.
    AssignmentExpression::DestructureMode mode = !as_lexical ? AssignmentExpression::DestructureMode::Var
                         : (is_const ? AssignmentExpression::DestructureMode::Const
                                     : AssignmentExpression::DestructureMode::Let);
    if (pattern_literal_) {
        AssignmentExpression::destructuring_assign(ctx, pattern_literal_.get(), source_value, mode);
    }
    return Value();
}



std::string DestructuringAssignment::to_string() const {
    return pattern_literal_ ? pattern_literal_->to_string() : std::string("[destructuring]");
}

std::unique_ptr<ASTNode> DestructuringAssignment::clone() const {
    auto cloned = std::make_unique<DestructuringAssignment>(
        std::vector<std::unique_ptr<Identifier>>{}, source_ ? source_->clone() : nullptr,
        type_, start_, end_);
    if (pattern_literal_) cloned->set_pattern_literal(pattern_literal_->clone());
    return cloned;
}


} // namespace Quanta
