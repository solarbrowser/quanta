/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "quanta/core/engine/builtins/DisposableBuiltin.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/parser/AST.h"

namespace Quanta {

void register_disposable_builtins(Context& ctx) {
    auto disposablestack_constructor = ObjectFactory::create_native_constructor("DisposableStack",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* constructor = ctx.get_this_binding();
            auto stack_obj = ObjectFactory::create_object();

            if (constructor && constructor->is_function()) {
                Value prototype_val = constructor->get_property("prototype");
                if (prototype_val.is_object()) {
                    stack_obj->set_prototype(prototype_val.as_object());
                }
            }

            stack_obj->set_property("_stack", Value(ObjectFactory::create_array(0).release()));
            stack_obj->set_property("_disposed", Value(false));

            return Value(stack_obj.release());
        }, 0);

    auto disposablestack_prototype = ObjectFactory::create_object();

    auto ds_use_fn = ObjectFactory::create_native_function("use",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value();

            Value disposed = this_obj->get_property("_disposed");
            if (disposed.to_boolean()) {
                ctx.throw_reference_error("DisposableStack already disposed");
                return Value();
            }

            if (args.size() > 0) {
                Value stack_val = this_obj->get_property("_stack");
                if (stack_val.is_object()) {
                    Object* stack = stack_val.as_object();
                    stack->push(args[0]);
                }
                return args[0];
            }
            return Value();
        }, 1);
    disposablestack_prototype->set_property("use", Value(ds_use_fn.release()), PropertyAttributes::BuiltinFunction);

    auto ds_dispose_fn = ObjectFactory::create_native_function("dispose",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value();

            Value disposed = this_obj->get_property("_disposed");
            if (disposed.to_boolean()) {
                return Value();
            }

            this_obj->set_property("_disposed", Value(true));

            Value stack_val = this_obj->get_property("_stack");
            if (stack_val.is_object()) {
                Object* stack = stack_val.as_object();
                uint32_t length = stack->get_length();

                for (int32_t i = length - 1; i >= 0; i--) {
                    Value resource = stack->get_element(static_cast<uint32_t>(i));
                    if (resource.is_object()) {
                        Object* res_obj = resource.as_object();
                        Value dispose_method = res_obj->get_property("dispose");
                        if (dispose_method.is_function()) {
                            Function* dispose_fn_inner = dispose_method.as_function();
                            std::vector<Value> no_args;
                            dispose_fn_inner->call(ctx, no_args, resource);
                        }
                    }
                }
            }
            return Value();
        }, 0);
    disposablestack_prototype->set_property("dispose", Value(ds_dispose_fn.release()), PropertyAttributes::BuiltinFunction);

    auto ds_adopt_fn = ObjectFactory::create_native_function("adopt",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value();

            Value disposed = this_obj->get_property("_disposed");
            if (disposed.to_boolean()) {
                ctx.throw_reference_error("DisposableStack already disposed");
                return Value();
            }

            if (args.size() < 2) return Value();

            Value value = args[0];
            Value onDispose = args[1];

            if (!onDispose.is_function()) {
                ctx.throw_type_error("onDispose must be a function");
                return Value();
            }

            auto wrapper = ObjectFactory::create_object();
            wrapper->set_property("_value", value);
            wrapper->set_property("_onDispose", onDispose);

            auto wrapper_dispose = ObjectFactory::create_native_function("dispose",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)args;
                    Object* wrapper_obj = ctx.get_this_binding();
                    if (!wrapper_obj) return Value();

                    Value val = wrapper_obj->get_property("_value");
                    Value on_dispose = wrapper_obj->get_property("_onDispose");

                    if (on_dispose.is_function()) {
                        Function* dispose_callback = on_dispose.as_function();
                        std::vector<Value> callback_args = {val};
                        dispose_callback->call(ctx, callback_args);
                    }
                    return Value();
                }, 0);
            wrapper->set_property("dispose", Value(wrapper_dispose.release()));

            Value stack_val = this_obj->get_property("_stack");
            if (stack_val.is_object()) {
                Object* stack = stack_val.as_object();
                stack->push(Value(wrapper.release()));
            }

            return value;
        }, 2);
    disposablestack_prototype->set_property("adopt", Value(ds_adopt_fn.release()), PropertyAttributes::BuiltinFunction);

    auto ds_defer_fn = ObjectFactory::create_native_function("defer",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value();

            Value disposed = this_obj->get_property("_disposed");
            if (disposed.to_boolean()) {
                ctx.throw_reference_error("DisposableStack already disposed");
                return Value();
            }

            if (args.empty() || !args[0].is_function()) {
                ctx.throw_type_error("defer requires a function argument");
                return Value();
            }

            auto wrapper = ObjectFactory::create_object();
            wrapper->set_property("_onDispose", args[0]);

            auto wrapper_dispose = ObjectFactory::create_native_function("dispose",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)args;
                    Object* wrapper_obj = ctx.get_this_binding();
                    if (!wrapper_obj) return Value();

                    Value on_dispose = wrapper_obj->get_property("_onDispose");
                    if (on_dispose.is_function()) {
                        Function* dispose_callback = on_dispose.as_function();
                        std::vector<Value> no_args;
                        dispose_callback->call(ctx, no_args);
                    }
                    return Value();
                }, 0);
            wrapper->set_property("dispose", Value(wrapper_dispose.release()));

            Value stack_val = this_obj->get_property("_stack");
            if (stack_val.is_object()) {
                Object* stack = stack_val.as_object();
                stack->push(Value(wrapper.release()));
            }

            return Value();
        }, 1);
    disposablestack_prototype->set_property("defer", Value(ds_defer_fn.release()), PropertyAttributes::BuiltinFunction);

    auto ds_move_fn = ObjectFactory::create_native_function("move",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value();

            Value disposed = this_obj->get_property("_disposed");
            if (disposed.to_boolean()) {
                ctx.throw_reference_error("DisposableStack already disposed");
                return Value();
            }

            auto disposable_ctor = ctx.get_binding("DisposableStack");
            if (disposable_ctor.is_function()) {
                Function* ctor = disposable_ctor.as_function();
                std::vector<Value> no_args;
                Value new_stack = ctor->call(ctx, no_args);

                if (new_stack.is_object()) {
                    Object* new_stack_obj = new_stack.as_object();
                    Value old_stack = this_obj->get_property("_stack");
                    new_stack_obj->set_property("_stack", old_stack);
                    this_obj->set_property("_stack", Value(ObjectFactory::create_array(0).release()));
                    this_obj->set_property("_disposed", Value(true));
                    return new_stack;
                }
            }

            return Value();
        }, 0);
    disposablestack_prototype->set_property("move", Value(ds_move_fn.release()), PropertyAttributes::BuiltinFunction);

    disposablestack_constructor->set_property("prototype", Value(disposablestack_prototype.release()));

    ctx.register_built_in_object("DisposableStack", disposablestack_constructor.release());

    auto asyncdisposablestack_constructor = ObjectFactory::create_native_constructor("AsyncDisposableStack",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* constructor = ctx.get_this_binding();
            auto stack_obj = ObjectFactory::create_object();

            if (constructor && constructor->is_function()) {
                Value prototype_val = constructor->get_property("prototype");
                if (prototype_val.is_object()) {
                    stack_obj->set_prototype(prototype_val.as_object());
                }
            }

            stack_obj->set_property("_stack", Value(ObjectFactory::create_array(0).release()));
            stack_obj->set_property("_disposed", Value(false));

            return Value(stack_obj.release());
        }, 0);

    auto asyncdisposablestack_prototype = ObjectFactory::create_object();

    auto ads_use_fn = ObjectFactory::create_native_function("use",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value();

            Value disposed = this_obj->get_property("_disposed");
            if (disposed.to_boolean()) {
                ctx.throw_reference_error("AsyncDisposableStack already disposed");
                return Value();
            }

            if (args.size() > 0) {
                Value stack_val = this_obj->get_property("_stack");
                if (stack_val.is_object()) {
                    Object* stack = stack_val.as_object();
                    stack->push(args[0]);
                }
                return args[0];
            }
            return Value();
        }, 1);
    asyncdisposablestack_prototype->set_property("use", Value(ads_use_fn.release()), PropertyAttributes::BuiltinFunction);

    auto ads_disposeAsync_fn = ObjectFactory::create_native_function("disposeAsync",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value();

            Value disposed = this_obj->get_property("_disposed");
            if (disposed.to_boolean()) {
                Value promise_ctor = ctx.get_binding("Promise");
                if (promise_ctor.is_function()) {
                    Function* ctor = promise_ctor.as_function();
                    Value resolve_method = ctor->get_property("resolve");
                    if (resolve_method.is_function()) {
                        Function* resolve_fn = resolve_method.as_function();
                        std::vector<Value> args;
                        return resolve_fn->call(ctx, args, promise_ctor);
                    }
                }
                return Value();
            }

            this_obj->set_property("_disposed", Value(true));

            Value promise_ctor = ctx.get_binding("Promise");
            if (promise_ctor.is_function()) {
                Function* ctor = promise_ctor.as_function();
                Value resolve_method = ctor->get_property("resolve");
                if (resolve_method.is_function()) {
                    Function* resolve_fn = resolve_method.as_function();
                    std::vector<Value> args;
                    return resolve_fn->call(ctx, args, promise_ctor);
                }
            }

            return Value();
        }, 0);
    asyncdisposablestack_prototype->set_property("disposeAsync", Value(ads_disposeAsync_fn.release()), PropertyAttributes::BuiltinFunction);

    auto ads_adopt_fn = ObjectFactory::create_native_function("adopt",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value();

            Value disposed = this_obj->get_property("_disposed");
            if (disposed.to_boolean()) {
                ctx.throw_reference_error("AsyncDisposableStack already disposed");
                return Value();
            }

            if (args.size() < 2) return Value();

            Value value = args[0];
            Value onDisposeAsync = args[1];

            if (!onDisposeAsync.is_function()) {
                ctx.throw_type_error("onDisposeAsync must be a function");
                return Value();
            }

            auto wrapper = ObjectFactory::create_object();
            wrapper->set_property("_value", value);
            wrapper->set_property("_onDisposeAsync", onDisposeAsync);

            Value stack_val = this_obj->get_property("_stack");
            if (stack_val.is_object()) {
                Object* stack = stack_val.as_object();
                stack->push(Value(wrapper.release()));
            }

            return value;
        }, 2);
    asyncdisposablestack_prototype->set_property("adopt", Value(ads_adopt_fn.release()), PropertyAttributes::BuiltinFunction);

    auto ads_defer_fn = ObjectFactory::create_native_function("defer",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value();

            Value disposed = this_obj->get_property("_disposed");
            if (disposed.to_boolean()) {
                ctx.throw_reference_error("AsyncDisposableStack already disposed");
                return Value();
            }

            if (args.empty() || !args[0].is_function()) {
                ctx.throw_type_error("defer requires a function argument");
                return Value();
            }

            auto wrapper = ObjectFactory::create_object();
            wrapper->set_property("_onDisposeAsync", args[0]);

            Value stack_val = this_obj->get_property("_stack");
            if (stack_val.is_object()) {
                Object* stack = stack_val.as_object();
                stack->push(Value(wrapper.release()));
            }

            return Value();
        }, 1);
    asyncdisposablestack_prototype->set_property("defer", Value(ads_defer_fn.release()), PropertyAttributes::BuiltinFunction);

    auto ads_move_fn = ObjectFactory::create_native_function("move",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value();

            Value disposed = this_obj->get_property("_disposed");
            if (disposed.to_boolean()) {
                ctx.throw_reference_error("AsyncDisposableStack already disposed");
                return Value();
            }

            auto disposable_ctor = ctx.get_binding("AsyncDisposableStack");
            if (disposable_ctor.is_function()) {
                Function* ctor = disposable_ctor.as_function();
                std::vector<Value> no_args;
                Value new_stack = ctor->call(ctx, no_args);

                if (new_stack.is_object()) {
                    Object* new_stack_obj = new_stack.as_object();
                    Value old_stack = this_obj->get_property("_stack");
                    new_stack_obj->set_property("_stack", old_stack);
                    this_obj->set_property("_stack", Value(ObjectFactory::create_array(0).release()));
                    this_obj->set_property("_disposed", Value(true));
                    return new_stack;
                }
            }

            return Value();
        }, 0);
    asyncdisposablestack_prototype->set_property("move", Value(ads_move_fn.release()), PropertyAttributes::BuiltinFunction);

    asyncdisposablestack_constructor->set_property("prototype", Value(asyncdisposablestack_prototype.release()));

    ctx.register_built_in_object("AsyncDisposableStack", asyncdisposablestack_constructor.release());
}

} // namespace Quanta
