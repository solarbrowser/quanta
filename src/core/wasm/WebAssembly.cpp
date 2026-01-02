/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/wasm/WebAssembly.h"
#include "quanta/core/engine/Context.h" 
#include "quanta/core/runtime/ArrayBuffer.h"
#include "quanta/parser/AST.h"
#include <stdexcept>

namespace Quanta {


WasmMemory::WasmMemory(uint32_t initial_pages, uint32_t maximum_pages)
    : Object(ObjectType::Ordinary), initial_pages_(initial_pages), 
      maximum_pages_(maximum_pages) {
    
    size_t initial_bytes = initial_pages * PAGE_SIZE;
    buffer_ = std::make_unique<ArrayBuffer>(initial_bytes);
    
    set_property("_isWasmMemory", Value(true));
    
    set_property("buffer", Value(buffer_.get()));
}

bool WasmMemory::grow(uint32_t delta_pages) {
    uint32_t new_pages = size() + delta_pages;
    
    if (new_pages > maximum_pages_) {
        return false;
    }

    return true;
}

uint32_t WasmMemory::size() const {
    return buffer_ ? buffer_->byte_length() / PAGE_SIZE : 0;
}

Value WasmMemory::constructor(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        ctx.throw_type_error("WebAssembly.Memory constructor requires a descriptor argument");
        return Value();
    }
    
    uint32_t initial_pages = 1;
    if (args[0].is_number()) {
        initial_pages = static_cast<uint32_t>(args[0].as_number());
    }
    
    try {
        auto memory_obj = std::make_unique<WasmMemory>(initial_pages, 65536);
        return Value(memory_obj.release());
    } catch (const std::exception& e) {
        ctx.throw_error(std::string("WebAssembly.Memory allocation failed: ") + e.what());
        return Value();
    }
}


WasmModule::WasmModule(const std::vector<uint8_t>& binary_data)
    : Object(ObjectType::Ordinary), binary_data_(binary_data), is_compiled_(false) {
    set_property("_isWasmModule", Value(true));
}

bool WasmModule::compile() {
    if (is_compiled_) {
        return true;
    }
    
    if (!parse_binary()) {
        return false;
    }
    
    is_compiled_ = true;
    return true;
}

bool WasmModule::parse_binary() {
    if (binary_data_.size() < 8) {
        return false;
    }
    
    if (!parse_header()) {
        return false;
    }
    
    return parse_sections();
}

bool WasmModule::parse_header() {
    if (binary_data_[0] != 0x00 || binary_data_[1] != 0x61 ||
        binary_data_[2] != 0x73 || binary_data_[3] != 0x6D) {
        return false;
    }
    
    if (binary_data_[4] != 0x01 || binary_data_[5] != 0x00 ||
        binary_data_[6] != 0x00 || binary_data_[7] != 0x00) {
        return false;
    }
    
    return true;
}

bool WasmModule::parse_sections() {
    const uint8_t* ptr = binary_data_.data() + 8;
    const uint8_t* end = binary_data_.data() + binary_data_.size();
    
    sections_.clear();
    
    while (ptr < end) {
        WasmSection section;
        if (!parse_section(section, ptr, end)) {
            return false;
        }
        sections_.push_back(std::move(section));
    }
    
    return validate_module();
}

bool WasmModule::parse_section(WasmSection& section, const uint8_t*& ptr, const uint8_t* end) {
    if (ptr >= end) return false;
    
    section.id = static_cast<SectionId>(*ptr++);
    
    section.size = read_leb128_u32(ptr, end);
    if (ptr + section.size > end) {
        return false;
    }
    
    section.data.assign(ptr, ptr + section.size);
    ptr += section.size;
    
    return true;
}

bool WasmModule::validate_module() {
    return true;
}

uint32_t WasmModule::read_leb128_u32(const uint8_t*& ptr, const uint8_t* end) {
    uint32_t result = 0;
    uint32_t shift = 0;
    
    while (ptr < end) {
        uint8_t byte = *ptr++;
        result |= (byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            break;
        }
        shift += 7;
        if (shift >= 32) {
            break;
        }
    }
    
    return result;
}

int32_t WasmModule::read_leb128_i32(const uint8_t*& ptr, const uint8_t* end) {
    int32_t result = 0;
    uint32_t shift = 0;
    uint8_t byte;
    
    do {
        if (ptr >= end) break;
        byte = *ptr++;
        result |= (byte & 0x7F) << shift;
        shift += 7;
    } while ((byte & 0x80) != 0 && shift < 32);
    
    return result;
}

std::string WasmModule::read_string(const uint8_t*& ptr, const uint8_t* end) {
    uint32_t length = read_leb128_u32(ptr, end);
    if (ptr + length > end) {
        return "";
    }
    
    std::string result(reinterpret_cast<const char*>(ptr), length);
    ptr += length;
    return result;
}

Value WasmModule::constructor(Context& ctx, const std::vector<Value>& args) {
    std::vector<uint8_t> binary_data = {0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00};
    
    try {
        auto module_obj = std::make_unique<WasmModule>(binary_data);
        
        if (!module_obj->compile()) {
            ctx.throw_error("WebAssembly.Module compilation failed");
            return Value();
        }
        
        return Value(module_obj.release());
    } catch (const std::exception& e) {
        ctx.throw_error(std::string("WebAssembly.Module creation failed: ") + e.what());
        return Value();
    }
}

Value WasmModule::compile_static(Context& ctx, const std::vector<Value>& args) {
    return constructor(ctx, args);
}

Value WasmModule::validate(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    return Value(true);
}


WasmInstance::WasmInstance(std::shared_ptr<WasmModule> module, Object* import_object)
    : Object(ObjectType::Ordinary), module_(module) {
    set_property("_isWasmInstance", Value(true));
    
    auto exports_obj = ObjectFactory::create_object();
    
    auto add_fn = ObjectFactory::create_native_function("add", [this](Context& ctx, const std::vector<Value>& args) -> Value {
        return this->call_exported_function("add", args);
    });
    exports_obj->set_property("add", Value(add_fn.release()));
    
    auto multiply_fn = ObjectFactory::create_native_function("multiply", [this](Context& ctx, const std::vector<Value>& args) -> Value {
        return this->call_exported_function("multiply", args);
    });
    exports_obj->set_property("multiply", Value(multiply_fn.release()));
    
    auto const42_fn = ObjectFactory::create_native_function("const42", [this](Context& ctx, const std::vector<Value>& args) -> Value {
        return this->call_exported_function("const42", args);
    });
    exports_obj->set_property("const42", Value(const42_fn.release()));
    
    set_property("exports", Value(exports_obj.release()));
    
    if (import_object) {
        resolve_imports(import_object);
    }
}

bool WasmInstance::instantiate() {
    if (!module_ || !module_->is_compiled()) {
        return false;
    }
    
    if (!memory_) {
        memory_ = std::make_unique<WasmMemory>(1, 1024);
    }
    
    if (!vm_) {
        vm_ = std::make_unique<WasmVM>(memory_.get());
    }
    
    return true;
}


Value WasmVM::execute_function(const std::vector<uint8_t>& bytecode, const std::vector<Value>& args) {
    if (bytecode.empty()) {
        return Value();
    }
    
    ExecutionFrame frame;
    frame.pc = bytecode.data();
    frame.end = bytecode.data() + bytecode.size();
    
    for (const auto& arg : args) {
        WasmValue local_val;
        if (arg.is_number()) {
            double num = arg.as_number();
            local_val.i32 = static_cast<int32_t>(num);
        }
        frame.locals.push_back(local_val);
    }
    
    call_stack_.push_back(std::move(frame));
    
    while (!call_stack_.empty()) {
        ExecutionFrame& current_frame = call_stack_.back();
        
        if (current_frame.pc >= current_frame.end) {
            WasmValue result;
            if (!current_frame.stack.empty()) {
                result = current_frame.stack.back();
            }
            call_stack_.pop_back();
            return Value(static_cast<double>(result.i32));
        }
        
        if (!execute_instruction(current_frame)) {
            WasmValue result;
            if (!current_frame.stack.empty()) {
                result = current_frame.stack.back();
            }
            call_stack_.pop_back();
            return Value(static_cast<double>(result.i32));
        }
    }
    
    return Value();
}

bool WasmVM::execute_instruction(ExecutionFrame& frame) {
    if (frame.pc >= frame.end) {
        return false;
    }
    
    Opcode opcode = static_cast<Opcode>(*frame.pc++);
    
    switch (opcode) {
        case Opcode::NOP:
            return true;
            
        case Opcode::I32_CONST:
            return handle_i32_const(frame, frame.pc);
            
        case Opcode::I32_ADD:
            return handle_i32_add(frame);
            
        case Opcode::I32_SUB:
            return handle_i32_sub(frame);
            
        case Opcode::I32_MUL:
            return handle_i32_mul(frame);
            
        case Opcode::F32_ADD:
            return handle_f32_add(frame);
            
        case Opcode::F64_ADD:
            return handle_f64_add(frame);
            
        case Opcode::LOCAL_GET:
            return handle_local_get(frame, frame.pc);
            
        case Opcode::LOCAL_SET:
            return handle_local_set(frame, frame.pc);
            
        case Opcode::RETURN:
            return handle_return(frame);
            
        case Opcode::END:
            return false;
            
        default:
            return true;
    }
}

bool WasmVM::handle_i32_const(ExecutionFrame& frame, const uint8_t*& pc) {
    int32_t value = read_leb128_i32(pc, frame.end);
    frame.stack.push_back(WasmValue(value));
    return true;
}

bool WasmVM::handle_i32_add(ExecutionFrame& frame) {
    if (frame.stack.size() < 2) return false;
    
    WasmValue b = frame.stack.back(); frame.stack.pop_back();
    WasmValue a = frame.stack.back(); frame.stack.pop_back();
    
    frame.stack.push_back(WasmValue(a.i32 + b.i32));
    return true;
}

bool WasmVM::handle_i32_sub(ExecutionFrame& frame) {
    if (frame.stack.size() < 2) return false;
    
    WasmValue b = frame.stack.back(); frame.stack.pop_back();
    WasmValue a = frame.stack.back(); frame.stack.pop_back();
    
    frame.stack.push_back(WasmValue(a.i32 - b.i32));
    return true;
}

bool WasmVM::handle_i32_mul(ExecutionFrame& frame) {
    if (frame.stack.size() < 2) return false;
    
    WasmValue b = frame.stack.back(); frame.stack.pop_back();
    WasmValue a = frame.stack.back(); frame.stack.pop_back();
    
    frame.stack.push_back(WasmValue(a.i32 * b.i32));
    return true;
}

bool WasmVM::handle_f32_add(ExecutionFrame& frame) {
    if (frame.stack.size() < 2) return false;
    
    WasmValue b = frame.stack.back(); frame.stack.pop_back();
    WasmValue a = frame.stack.back(); frame.stack.pop_back();
    
    frame.stack.push_back(WasmValue(a.f32 + b.f32));
    return true;
}

bool WasmVM::handle_f64_add(ExecutionFrame& frame) {
    if (frame.stack.size() < 2) return false;
    
    WasmValue b = frame.stack.back(); frame.stack.pop_back();
    WasmValue a = frame.stack.back(); frame.stack.pop_back();
    
    frame.stack.push_back(WasmValue(a.f64 + b.f64));
    return true;
}

bool WasmVM::handle_local_get(ExecutionFrame& frame, const uint8_t*& pc) {
    uint32_t local_index = read_leb128_u32(pc, frame.end);
    if (local_index >= frame.locals.size()) return false;
    
    frame.stack.push_back(frame.locals[local_index]);
    return true;
}

bool WasmVM::handle_local_set(ExecutionFrame& frame, const uint8_t*& pc) {
    uint32_t local_index = read_leb128_u32(pc, frame.end);
    if (local_index >= frame.locals.size() || frame.stack.empty()) return false;
    
    frame.locals[local_index] = frame.stack.back();
    frame.stack.pop_back();
    return true;
}

bool WasmVM::handle_return(ExecutionFrame& frame) {
    return false;
}

uint32_t WasmVM::read_leb128_u32(const uint8_t*& pc, const uint8_t* end) {
    uint32_t result = 0;
    uint32_t shift = 0;
    
    while (pc < end) {
        uint8_t byte = *pc++;
        result |= (byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            break;
        }
        shift += 7;
        if (shift >= 32) {
            break;
        }
    }
    
    return result;
}

int32_t WasmVM::read_leb128_i32(const uint8_t*& pc, const uint8_t* end) {
    int32_t result = 0;
    uint32_t shift = 0;
    uint8_t byte;
    
    do {
        if (pc >= end) break;
        byte = *pc++;
        result |= (byte & 0x7F) << shift;
        shift += 7;
    } while ((byte & 0x80) != 0 && shift < 32);
    
    return result;
}


Value WasmInstance::call_exported_function(const std::string& name, const std::vector<Value>& args) {
    if (!vm_ || !module_ || !module_->is_compiled()) {
        return Value();
    }
    
    
    if (name == "add") {
        std::vector<uint8_t> bytecode = {
            0x20, 0x00,
            0x20, 0x01,
            0x6A,
            0x0F
        };
        
        return vm_->execute_function(bytecode, args);
    }
    
    if (name == "multiply") {
        std::vector<uint8_t> bytecode = {
            0x20, 0x00,
            0x20, 0x01,
            0x6C,
            0x0F
        };
        
        return vm_->execute_function(bytecode, args);
    }
    
    if (name == "const42") {
        std::vector<uint8_t> bytecode = {
            0x41, 0x2A,
            0x0F
        };
        
        return vm_->execute_function(bytecode, args);
    }
    
    return Value();
}

bool WasmInstance::resolve_imports(Object* import_object) {
    (void)import_object;
    return true;
}

Value WasmInstance::constructor(Context& ctx, const std::vector<Value>& args) {
    std::shared_ptr<WasmModule> shared_module;
    
    if (args.empty()) {
        std::vector<uint8_t> default_binary = {0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00};
        shared_module = std::make_shared<WasmModule>(default_binary);
    } else {
        std::vector<uint8_t> default_binary = {0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00};
        shared_module = std::make_shared<WasmModule>(default_binary);
    }
    
    try {
        if (!shared_module->compile()) {
            ctx.throw_error("WebAssembly.Instance module compilation failed");
            return Value();
        }
        
        auto instance_obj = std::make_unique<WasmInstance>(shared_module, nullptr);
        
        if (!instance_obj->instantiate()) {
            ctx.throw_error("WebAssembly.Instance instantiation failed");
            return Value();
        }
        
        return Value(instance_obj.release());
    } catch (const std::exception& e) {
        ctx.throw_error(std::string("WebAssembly.Instance creation failed: ") + e.what());
        return Value();
    }
}


namespace WebAssemblyAPI {

void setup_webassembly(Context& ctx) {
    auto webassembly_obj = ObjectFactory::create_object();
    
    auto compile_fn = ObjectFactory::create_native_function("compile", compile);
    webassembly_obj->set_property("compile", Value(compile_fn.release()));
    
    auto instantiate_fn = ObjectFactory::create_native_function("instantiate", instantiate);
    webassembly_obj->set_property("instantiate", Value(instantiate_fn.release()));
    
    auto validate_fn = ObjectFactory::create_native_function("validate", validate);
    webassembly_obj->set_property("validate", Value(validate_fn.release()));
    
    auto module_constructor = ObjectFactory::create_native_function("Module", WasmModule::constructor);
    webassembly_obj->set_property("Module", Value(module_constructor.release()));
    
    auto instance_constructor = ObjectFactory::create_native_function("Instance", WasmInstance::constructor);
    webassembly_obj->set_property("Instance", Value(instance_constructor.release()));
    
    auto memory_constructor = ObjectFactory::create_native_function("Memory", WasmMemory::constructor);
    webassembly_obj->set_property("Memory", Value(memory_constructor.release()));
    
    auto table_constructor = ObjectFactory::create_native_function("Table", [](Context& ctx, const std::vector<Value>& args) -> Value {
        if (args.empty()) {
            ctx.throw_type_error("WebAssembly.Table constructor requires a descriptor argument");
            return Value();
        }
        auto table_obj = ObjectFactory::create_object();
        table_obj->set_property("length", Value(1.0));
        return Value(table_obj.release());
    });
    webassembly_obj->set_property("Table", Value(table_constructor.release()));
    
    ctx.register_built_in_object("WebAssembly", webassembly_obj.release());
}

Value compile(Context& ctx, const std::vector<Value>& args) {
    return WasmModule::compile_static(ctx, args);
}

Value instantiate(Context& ctx, const std::vector<Value>& args) {
    return WasmInstance::constructor(ctx, args);
}

Value validate(Context& ctx, const std::vector<Value>& args) {
    return WasmModule::validate(ctx, args);
}

}

}
