/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/runtime/Iterator.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/core/runtime/MapSet.h"
#include "quanta/parser/AST.h"
#include <iostream>

namespace Quanta {


Iterator::Iterator(NextFunction next_fn) 
    : Object(ObjectType::Custom), next_fn_(next_fn), done_(false) {
    auto next_method = ObjectFactory::create_native_function("next", 
        [this](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            auto result = this->next();
            return Iterator::create_iterator_result(result.value, result.done);
        });
    this->set_property("next", Value(next_method.release()));
}

Iterator::Iterator() 
    : Object(ObjectType::Custom), done_(false) {
}

void Iterator::set_next_function(NextFunction next_fn) {
    next_fn_ = next_fn;
    auto next_method = ObjectFactory::create_native_function("next", 
        [this](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            auto result = this->next();
            return Iterator::create_iterator_result(result.value, result.done);
        });
    this->set_property("next", Value(next_method.release()));
}

Iterator::IteratorResult Iterator::next() {
    if (done_) {
        return IteratorResult(Value(), true);
    }
    
    auto result = next_fn_();
    if (result.done) {
        done_ = true;
    }
    
    return result;
}

Value Iterator::iterator_next(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    
    Value this_value = ctx.get_binding("this");
    if (!this_value.is_object()) {
        ctx.throw_exception(Value(std::string("Iterator.prototype.next called on non-object")));
        return Value();
    }
    
    Object* obj = this_value.as_object();
    if (obj->get_type() != Object::ObjectType::Custom) {
        ctx.throw_exception(Value(std::string("Iterator.prototype.next called on non-iterator")));
        return Value();
    }
    
    Iterator* iterator = static_cast<Iterator*>(obj);
    auto result = iterator->next();
    
    return create_iterator_result(result.value, result.done);
}

Value Iterator::iterator_return(Context& ctx, const std::vector<Value>& args) {
    Value return_value = args.empty() ? Value() : args[0];
    
    Value this_value = ctx.get_binding("this");
    if (!this_value.is_object()) {
        ctx.throw_exception(Value(std::string("Iterator.prototype.return called on non-object")));
        return Value();
    }
    
    Object* obj = this_value.as_object();
    if (obj->get_type() != Object::ObjectType::Custom) {
        ctx.throw_exception(Value(std::string("Iterator.prototype.return called on non-iterator")));
        return Value();
    }
    
    Iterator* iterator = static_cast<Iterator*>(obj);
    iterator->done_ = true;
    
    return create_iterator_result(return_value, true);
}

Value Iterator::iterator_throw(Context& ctx, const std::vector<Value>& args) {
    Value exception = args.empty() ? Value() : args[0];
    
    Value this_value = ctx.get_binding("this");
    if (!this_value.is_object()) {
        ctx.throw_exception(Value(std::string("Iterator.prototype.throw called on non-object")));
        return Value();
    }
    
    Object* obj = this_value.as_object();
    if (obj->get_type() != Object::ObjectType::Custom) {
        ctx.throw_exception(Value(std::string("Iterator.prototype.throw called on non-iterator")));
        return Value();
    }
    
    Iterator* iterator = static_cast<Iterator*>(obj);
    iterator->done_ = true;
    
    ctx.throw_exception(exception);
    return Value();
}

void Iterator::setup_iterator_prototype(Context& ctx) {
    auto iterator_prototype = ObjectFactory::create_object();
    
    auto next_fn = ObjectFactory::create_native_function("next", iterator_next);
    iterator_prototype->set_property("next", Value(next_fn.release()));
    
    auto return_fn = ObjectFactory::create_native_function("return", iterator_return);
    iterator_prototype->set_property("return", Value(return_fn.release()));
    
    auto throw_fn = ObjectFactory::create_native_function("throw", iterator_throw);
    iterator_prototype->set_property("throw", Value(throw_fn.release()));
    
    Symbol* iterator_symbol = Symbol::get_well_known(Symbol::ITERATOR);
    if (iterator_symbol) {
        auto self_iterator_fn = ObjectFactory::create_native_function("@@iterator", 
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)args;
                return ctx.get_binding("this");
            });
        iterator_prototype->set_property(iterator_symbol->to_string(), Value(self_iterator_fn.release()));
    }
    
    ctx.create_binding("IteratorPrototype", Value(iterator_prototype.release()));
}

Value Iterator::create_iterator_result(const Value& value, bool done) {
    auto result_obj = ObjectFactory::create_object();
    result_obj->set_property("value", value);
    result_obj->set_property("done", Value(done));
    return Value(result_obj.release());
}


ArrayIterator::ArrayIterator(Object* array, Kind kind) 
    : Iterator([this]() { return this->next_impl(); }), array_(array), kind_(kind), index_(0) {
}

std::unique_ptr<ArrayIterator> ArrayIterator::create_keys_iterator(Object* array) {
    return std::make_unique<ArrayIterator>(array, Kind::Keys);
}

std::unique_ptr<ArrayIterator> ArrayIterator::create_values_iterator(Object* array) {
    return std::make_unique<ArrayIterator>(array, Kind::Values);
}

std::unique_ptr<ArrayIterator> ArrayIterator::create_entries_iterator(Object* array) {
    return std::make_unique<ArrayIterator>(array, Kind::Entries);
}

Iterator::IteratorResult ArrayIterator::next_impl() {
    if (!array_ || index_ >= array_->get_length()) {
        return IteratorResult(Value(), true);
    }

    Value element = array_->get_element(index_);

    switch (kind_) {
        case Kind::Keys:
            return IteratorResult(Value(static_cast<double>(index_++)), false);

        case Kind::Values:
            index_++;
            return IteratorResult(element, false);
            
        case Kind::Entries: {
            auto entry_array = ObjectFactory::create_array(2);
            entry_array->set_element(0, Value(static_cast<double>(index_)));
            entry_array->set_element(1, element);
            index_++;
            return IteratorResult(Value(entry_array.release()), false);
        }
    }
    
    return IteratorResult(Value(), true);
}


StringIterator::StringIterator(const std::string& str) 
    : Iterator(), string_(str), position_(0) {
    auto next_method = ObjectFactory::create_native_function("next", StringIterator::string_iterator_next_method);
    this->set_property("next", Value(next_method.release()));
}

Iterator::IteratorResult StringIterator::next() {
    return next_impl();
}

Iterator::IteratorResult StringIterator::next_impl() {
    if (position_ >= string_.length()) {
        return IteratorResult(Value(), true);
    }

    // UTF-8 codepoint-aware: read full multi-byte character
    unsigned char ch = static_cast<unsigned char>(string_[position_]);
    size_t char_len = 1;
    if (ch >= 0xF0) char_len = 4;
    else if (ch >= 0xE0) char_len = 3;
    else if (ch >= 0xC0) char_len = 2;
    if (position_ + char_len > string_.length()) char_len = 1;
    std::string codepoint = string_.substr(position_, char_len);
    position_ += char_len;
    return IteratorResult(Value(codepoint), false);
}

Value StringIterator::string_iterator_next_method(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    
    Object* this_obj = ctx.get_this_binding();
    if (!this_obj) {
        ctx.throw_exception(Value(std::string("StringIterator next() called without proper this binding")));
        return Value();
    }
    
    if (this_obj->get_type() != Object::ObjectType::Custom) {
        ctx.throw_exception(Value(std::string("StringIterator next() called on non-iterator object")));
        return Value();
    }
    
    StringIterator* string_iter = static_cast<StringIterator*>(this_obj);
    auto result = string_iter->next();
    return Iterator::create_iterator_result(result.value, result.done);
}


MapIterator::MapIterator(Map* map, Kind kind) 
    : Iterator(), map_(map), kind_(kind), index_(0) {
    auto next_method = ObjectFactory::create_native_function("next", MapIterator::map_iterator_next_method);
    this->set_property("next", Value(next_method.release()));
}

Iterator::IteratorResult MapIterator::next() {
    return next_impl();
}

Value MapIterator::map_iterator_next_method(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    
    Object* this_obj = ctx.get_this_binding();
    if (!this_obj) {
        ctx.throw_exception(Value(std::string("MapIterator next() called without proper this binding")));
        return Value();
    }
    
    if (this_obj->get_type() != Object::ObjectType::Custom) {
        ctx.throw_exception(Value(std::string("MapIterator next() called on non-iterator object")));
        return Value();
    }
    
    MapIterator* map_iter = static_cast<MapIterator*>(this_obj);
    auto result = map_iter->next();
    return Iterator::create_iterator_result(result.value, result.done);
}

Iterator::IteratorResult MapIterator::next_impl() {
    if (!map_ || index_ >= map_->size()) {
        return IteratorResult(Value(), true);
    }
    
    auto entries = map_->entries();
    if (index_ >= entries.size()) {
        return IteratorResult(Value(), true);
    }
    
    auto& entry = entries[index_];
    index_++;
    
    switch (kind_) {
        case Kind::Keys:
            return IteratorResult(entry.first, false);
            
        case Kind::Values:
            return IteratorResult(entry.second, false);
            
        case Kind::Entries: {
            auto entry_array = ObjectFactory::create_array(2);
            entry_array->set_element(0, entry.first);
            entry_array->set_element(1, entry.second);
            return IteratorResult(Value(entry_array.release()), false);
        }
    }
    
    return IteratorResult(Value(), true);
}


SetIterator::SetIterator(Set* set, Kind kind) 
    : Iterator(), set_(set), kind_(kind), index_(0) {
    auto next_method = ObjectFactory::create_native_function("next", SetIterator::set_iterator_next_method);
    this->set_property("next", Value(next_method.release()));
}

Iterator::IteratorResult SetIterator::next() {
    return next_impl();
}

Value SetIterator::set_iterator_next_method(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    
    Object* this_obj = ctx.get_this_binding();
    if (!this_obj) {
        ctx.throw_exception(Value(std::string("SetIterator next() called without proper this binding")));
        return Value();
    }
    
    if (this_obj->get_type() != Object::ObjectType::Custom) {
        ctx.throw_exception(Value(std::string("SetIterator next() called on non-iterator object")));
        return Value();
    }
    
    SetIterator* set_iter = static_cast<SetIterator*>(this_obj);
    auto result = set_iter->next();
    return Iterator::create_iterator_result(result.value, result.done);
}

Iterator::IteratorResult SetIterator::next_impl() {
    if (!set_ || index_ >= set_->size()) {
        return IteratorResult(Value(), true);
    }
    
    auto values = set_->values();
    if (index_ >= values.size()) {
        return IteratorResult(Value(), true);
    }
    
    Value value = values[index_];
    
    size_t old_index = index_;
    index_++;
    
    if (old_index >= values.size()) {
        return IteratorResult(Value(), true);
    }
    
    switch (kind_) {
        case Kind::Values:
            return IteratorResult(value, false);
            
        case Kind::Entries: {
            auto entry_array = ObjectFactory::create_array(2);
            entry_array->set_element(0, value);
            entry_array->set_element(1, value);
            return IteratorResult(Value(entry_array.release()), false);
        }
    }
    
    return IteratorResult(Value(), true);
}


namespace IterableUtils {

bool is_iterable(const Value& value) {
    if (!value.is_object()) {
        return false;
    }
    
    Object* obj = value.as_object();
    Symbol* iterator_symbol = Symbol::get_well_known(Symbol::ITERATOR);
    
    if (!iterator_symbol) {
        return false;
    }
    
    return obj->has_property(iterator_symbol->to_string());
}

std::unique_ptr<Iterator> get_iterator(const Value& value, Context& ctx) {
    if (!value.is_object()) {
        return nullptr;
    }
    
    Object* obj = value.as_object();
    Symbol* iterator_symbol = Symbol::get_well_known(Symbol::ITERATOR);
    
    if (!iterator_symbol) {
        return nullptr;
    }
    
    Value iterator_method = obj->get_property(iterator_symbol->to_string());
    if (!iterator_method.is_function()) {
        return nullptr;
    }
    
    Function* iterator_fn = iterator_method.as_function();
    Value iterator_result = iterator_fn->call(ctx, {}, value);
    
    if (!iterator_result.is_object()) {
        return nullptr;
    }
    
    Object* iterator_obj = iterator_result.as_object();
    if (iterator_obj->get_type() != Object::ObjectType::Custom) {
        return nullptr;
    }
    
    return std::unique_ptr<Iterator>(static_cast<Iterator*>(iterator_obj));
}

std::vector<Value> to_array(const Value& iterable, Context& ctx) {
    std::vector<Value> result;
    
    auto iterator = get_iterator(iterable, ctx);
    if (!iterator) {
        return result;
    }
    
    while (true) {
        auto iter_result = iterator->next();
        if (iter_result.done) {
            break;
        }
        result.push_back(iter_result.value);
    }
    
    return result;
}

void for_of_loop(const Value& iterable, 
                 std::function<void(const Value&)> callback, 
                 Context& ctx) {
    auto iterator = get_iterator(iterable, ctx);
    if (!iterator) {
        ctx.throw_exception(Value(std::string("Value is not iterable")));
        return;
    }
    
    while (true) {
        auto iter_result = iterator->next();
        if (iter_result.done) {
            break;
        }
        callback(iter_result.value);
    }
}

void setup_array_iterator_methods(Context& ctx) {
    Value array_constructor = ctx.get_binding("Array");
    if (!array_constructor.is_function()) {
        return;
    }
    
    Function* array_fn = array_constructor.as_function();
    Value array_prototype = array_fn->get_property("prototype");
    if (!array_prototype.is_object()) {
        return;
    }
    
    Object* array_proto = array_prototype.as_object();
    
    auto keys_fn = ObjectFactory::create_native_function("keys",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* array = ctx.get_this_binding();
            if (!array) {
                ctx.throw_exception(Value(std::string("Array.prototype.keys called on non-object")));
                return Value();
            }
            auto iterator = ArrayIterator::create_keys_iterator(array);
            return Value(iterator.release());
        });
    
    auto values_fn = ObjectFactory::create_native_function("values",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* array = ctx.get_this_binding();
            if (!array) {
                ctx.throw_exception(Value(std::string("Array.prototype.values called on non-object")));
                return Value();
            }
            auto iterator = ArrayIterator::create_values_iterator(array);
            return Value(iterator.release());
        });
    
    auto entries_fn = ObjectFactory::create_native_function("entries",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* array = ctx.get_this_binding();
            if (!array) {
                ctx.throw_exception(Value(std::string("Array.prototype.entries called on non-object")));
                return Value();
            }
            auto iterator = ArrayIterator::create_entries_iterator(array);
            return Value(iterator.release());
        });
    
    array_proto->set_property("keys", Value(keys_fn.release()));
    array_proto->set_property("values", Value(values_fn.release()));
    array_proto->set_property("entries", Value(entries_fn.release()));
    
    Symbol* iterator_symbol = Symbol::get_well_known(Symbol::ITERATOR);
    if (iterator_symbol) {
        auto default_iterator_fn = ObjectFactory::create_native_function("@@iterator", 
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)args;
                Object* array = ctx.get_this_binding();
                if (!array) {
                    ctx.throw_exception(Value(std::string("Array.prototype[Symbol.iterator] called on non-object")));
                    return Value();
                }
                auto iterator = ArrayIterator::create_values_iterator(array);
                return Value(iterator.release());
            });
        
        array_proto->set_property(iterator_symbol->to_string(), Value(default_iterator_fn.release()));
    }
}

void setup_string_iterator_methods(Context& ctx) {
    Value string_constructor = ctx.get_binding("String");
    if (!string_constructor.is_function()) {
        return;
    }
    
    Function* string_fn = string_constructor.as_function();
    Value string_prototype = string_fn->get_property("prototype");
    if (!string_prototype.is_object()) {
        return;
    }
    
    Object* string_proto = string_prototype.as_object();
    
    Symbol* iterator_symbol = Symbol::get_well_known(Symbol::ITERATOR);
    if (iterator_symbol) {
        auto string_iterator_fn = ObjectFactory::create_native_function("@@iterator", 
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)args;
                Value this_value = ctx.get_binding("this");
                std::string str = this_value.to_string();
                
                auto iterator = std::make_unique<StringIterator>(str);
                return Value(iterator.release());
            });
        
        string_proto->set_property(iterator_symbol->to_string(), Value(string_iterator_fn.release()));
    }
}

void setup_map_iterator_methods(Context& ctx) {
}

void setup_set_iterator_methods(Context& ctx) {
}

}

}
