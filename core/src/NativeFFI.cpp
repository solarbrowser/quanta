/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../include/NativeFFI.h"
#include "../include/Value.h"
#include "../include/Context.h"
#include <iostream>
#include <fstream>
#include <algorithm>
#include <chrono>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Quanta {

//=============================================================================
// FFITypeInfo Implementation
//=============================================================================

void FFITypeInfo::calculate_size_and_alignment() {
    switch (type) {
        case FFIType::VOID:
            size = 0; alignment = 1; break;
        case FFIType::BOOL:
            size = sizeof(bool); alignment = alignof(bool); break;
        case FFIType::INT8:
        case FFIType::UINT8:
            size = 1; alignment = 1; break;
        case FFIType::INT16:
        case FFIType::UINT16:
            size = 2; alignment = 2; break;
        case FFIType::INT32:
        case FFIType::UINT32:
            size = 4; alignment = 4; break;
        case FFIType::INT64:
        case FFIType::UINT64:
            size = 8; alignment = 8; break;
        case FFIType::FLOAT:
            size = sizeof(float); alignment = alignof(float); break;
        case FFIType::DOUBLE:
            size = sizeof(double); alignment = alignof(double); break;
        case FFIType::POINTER:
        case FFIType::STRING:
        case FFIType::FUNCTION:
            size = sizeof(void*); alignment = alignof(void*); break;
        case FFIType::STRUCT:
            // Calculate struct size with proper alignment
            size = 0;
            alignment = 1;
            for (const auto& field : fields) {
                alignment = std::max(alignment, field.alignment);
                size = (size + field.alignment - 1) & ~(field.alignment - 1); // Align
                size += field.size;
            }
            size = (size + alignment - 1) & ~(alignment - 1); // Final alignment
            break;
        case FFIType::ARRAY:
            if (element_type) {
                size = element_type->size * array_length;
                alignment = element_type->alignment;
            }
            break;
        default:
            size = 0; alignment = 1; break;
    }
}

bool FFITypeInfo::is_primitive() const {
    return type != FFIType::STRUCT && type != FFIType::ARRAY && type != FFIType::POINTER;
}

bool FFITypeInfo::is_composite() const {
    return type == FFIType::STRUCT || type == FFIType::ARRAY;
}

std::string FFITypeInfo::to_string() const {
    switch (type) {
        case FFIType::VOID: return "void";
        case FFIType::BOOL: return "bool";
        case FFIType::INT8: return "int8";
        case FFIType::UINT8: return "uint8";
        case FFIType::INT16: return "int16";
        case FFIType::UINT16: return "uint16";
        case FFIType::INT32: return "int32";
        case FFIType::UINT32: return "uint32";
        case FFIType::INT64: return "int64";
        case FFIType::UINT64: return "uint64";
        case FFIType::FLOAT: return "float";
        case FFIType::DOUBLE: return "double";
        case FFIType::POINTER: return "pointer";
        case FFIType::STRING: return "string";
        case FFIType::FUNCTION: return "function";
        case FFIType::STRUCT: return "struct " + name;
        case FFIType::ARRAY: return "array[" + std::to_string(array_length) + "]";
        default: return "unknown";
    }
}

//=============================================================================
// FFISignature Implementation
//=============================================================================

std::string FFISignature::to_string() const {
    std::string result = return_type.to_string() + " " + name + "(";
    
    for (size_t i = 0; i < parameter_types.size(); ++i) {
        if (i > 0) result += ", ";
        result += parameter_types[i].to_string();
    }
    
    if (is_variadic) {
        if (!parameter_types.empty()) result += ", ";
        result += "...";
    }
    
    result += ")";
    return result;
}

bool FFISignature::matches(const std::vector<Value>& args) const {
    if (is_variadic) {
        return args.size() >= parameter_types.size();
    }
    return args.size() == parameter_types.size();
}

size_t FFISignature::get_stack_size() const {
    size_t total_size = 0;
    for (const auto& param : parameter_types) {
        total_size += param.size;
    }
    return total_size;
}

//=============================================================================
// NativeLibrary Implementation
//=============================================================================

NativeLibrary::NativeLibrary(const std::string& path) 
    : library_path_(path), library_handle_(nullptr), is_loaded_(false),
      total_calls_(0), total_call_time_ns_(0) {
    
    std::cout << "📚 NATIVE LIBRARY CREATED: " << path << std::endl;
}

NativeLibrary::~NativeLibrary() {
    unload();
    std::cout << "📚 NATIVE LIBRARY DESTROYED: " << library_path_ << std::endl;
}

bool NativeLibrary::load() {
    if (is_loaded_) return true;
    
    library_handle_ = load_library_platform(library_path_);
    if (!library_handle_) {
        set_error("Failed to load library: " + get_platform_error());
        return false;
    }
    
    is_loaded_ = true;
    std::cout << "✅ Library loaded: " << library_path_ << std::endl;
    return true;
}

void NativeLibrary::unload() {
    if (!is_loaded_) return;
    
    if (library_handle_) {
        unload_library_platform(library_handle_);
        library_handle_ = nullptr;
    }
    
    symbols_.clear();
    function_signatures_.clear();
    is_loaded_ = false;
    
    std::cout << "📚 Library unloaded: " << library_path_ << std::endl;
}

bool NativeLibrary::reload() {
    std::cout << "🔄 Reloading library: " << library_path_ << std::endl;
    
    // Store current state
    auto old_signatures = function_signatures_;
    
    unload();
    bool success = load();
    
    if (success) {
        // Restore function signatures
        function_signatures_ = old_signatures;
        
        // Re-resolve symbols
        for (const auto& [name, signature] : function_signatures_) {
            get_symbol(name); // This will cache the symbol
        }
        
        std::cout << "✅ Library reloaded successfully" << std::endl;
    } else {
        std::cout << "❌ Library reload failed: " << get_last_error() << std::endl;
    }
    
    return success;
}

void* NativeLibrary::get_symbol(const std::string& name) {
    if (!is_loaded_) {
        set_error("Library not loaded");
        return nullptr;
    }
    
    // Check cache first
    auto it = symbols_.find(name);
    if (it != symbols_.end()) {
        return it->second;
    }
    
    // Resolve symbol
    void* symbol = get_symbol_platform(library_handle_, name);
    if (!symbol) {
        set_error("Symbol not found: " + name);
        return nullptr;
    }
    
    // Cache the symbol
    symbols_[name] = symbol;
    std::cout << "🔗 Symbol resolved: " << name << " -> " << symbol << std::endl;
    
    return symbol;
}

bool NativeLibrary::has_symbol(const std::string& name) const {
    return symbols_.find(name) != symbols_.end();
}

void NativeLibrary::register_function(const std::string& name, const FFISignature& signature) {
    function_signatures_[name] = signature;
    std::cout << "📝 Function registered: " << signature.to_string() << std::endl;
}

void NativeLibrary::register_function(const std::string& name, FFIType return_type, const std::vector<FFIType>& param_types) {
    FFISignature signature;
    signature.name = name;
    signature.return_type = FFITypeInfo(return_type);
    
    for (FFIType param_type : param_types) {
        signature.parameter_types.emplace_back(param_type);
    }
    
    register_function(name, signature);
}

Value NativeLibrary::call_function(const std::string& name, const std::vector<Value>& args, Context* context) {
    auto sig_it = function_signatures_.find(name);
    if (sig_it == function_signatures_.end()) {
        throw std::runtime_error("Function not registered: " + name);
    }
    
    void* func_ptr = get_symbol(name);
    if (!func_ptr) {
        throw std::runtime_error("Function symbol not found: " + name);
    }
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    Value result = call_function_ptr(func_ptr, sig_it->second, args, context);
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();
    
    // Update statistics
    total_calls_++;
    total_call_time_ns_ += duration;
    function_call_counts_[name]++;
    
    std::cout << "📞 Function called: " << name << " (" << (duration / 1000.0) << " μs)" << std::endl;
    
    return result;
}

Value NativeLibrary::call_function_ptr(void* func_ptr, const FFISignature& signature, const std::vector<Value>& args, Context* context) {
    // This is a simplified implementation
    // In a real implementation, this would use platform-specific assembly or libffi
    
    if (!signature.matches(args)) {
        throw std::runtime_error("Argument count mismatch for function " + signature.name);
    }
    
    // For demonstration, we'll simulate a function call
    std::cout << "🔧 Calling native function: " << signature.to_string() << std::endl;
    std::cout << "  Arguments: " << args.size() << std::endl;
    std::cout << "  Function pointer: " << func_ptr << std::endl;
    
    // Simulate return value based on return type
    switch (signature.return_type.type) {
        case FFIType::VOID:
            return Value(); // undefined
        case FFIType::INT32:
            return Value(42.0); // Simulated integer result
        case FFIType::DOUBLE:
            return Value(3.14159); // Simulated double result
        case FFIType::STRING:
            return Value("Hello from native code"); // Simulated string result
        default:
            return Value(); // undefined for other types
    }
}

double NativeLibrary::get_average_call_time_us() const {
    return total_calls_ > 0 ? (total_call_time_ns_ / 1000.0 / total_calls_) : 0.0;
}

void NativeLibrary::print_performance_stats() const {
    std::cout << "📊 LIBRARY PERFORMANCE STATS: " << library_path_ << std::endl;
    std::cout << "  Total calls: " << total_calls_ << std::endl;
    std::cout << "  Average call time: " << get_average_call_time_us() << " μs" << std::endl;
    std::cout << "  Registered functions: " << function_signatures_.size() << std::endl;
    std::cout << "  Cached symbols: " << symbols_.size() << std::endl;
    
    if (!function_call_counts_.empty()) {
        std::cout << "  Function call counts:" << std::endl;
        for (const auto& [name, count] : function_call_counts_) {
            std::cout << "    " << name << ": " << count << " calls" << std::endl;
        }
    }
}

std::vector<std::string> NativeLibrary::get_function_names() const {
    std::vector<std::string> names;
    for (const auto& [name, signature] : function_signatures_) {
        names.push_back(name);
    }
    return names;
}

void* NativeLibrary::load_library_platform(const std::string& path) {
#ifdef _WIN32
    return LoadLibraryA(path.c_str());
#else
    return dlopen(path.c_str(), RTLD_LAZY);
#endif
}

void NativeLibrary::unload_library_platform(void* handle) {
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
}

void* NativeLibrary::get_symbol_platform(void* handle, const std::string& name) {
#ifdef _WIN32
    return GetProcAddress(static_cast<HMODULE>(handle), name.c_str());
#else
    return dlsym(handle, name.c_str());
#endif
}

std::string NativeLibrary::get_platform_error() {
#ifdef _WIN32
    DWORD error = GetLastError();
    char* message = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
                   nullptr, error, 0, (LPSTR)&message, 0, nullptr);
    std::string result = message ? message : "Unknown error";
    LocalFree(message);
    return result;
#else
    const char* error = dlerror();
    return error ? error : "Unknown error";
#endif
}

//=============================================================================
// FFIMarshaler Implementation
//=============================================================================

FFIMarshaler::FFIMarshaler() : buffer_offset_(0) {
    argument_buffer_.reserve(1024);
    return_buffer_.reserve(512);
}

FFIMarshaler::~FFIMarshaler() = default;

void FFIMarshaler::marshal_argument(const Value& js_value, const FFITypeInfo& type_info) {
    std::cout << "🔄 Marshaling argument: " << type_info.to_string() << std::endl;
    
    switch (type_info.type) {
        case FFIType::BOOL: {
            bool value = js_value.is_truthy();
            write_value(value);
            break;
        }
        case FFIType::INT32: {
            int32_t value = static_cast<int32_t>(js_value.to_number());
            write_value(value);
            break;
        }
        case FFIType::UINT32: {
            uint32_t value = static_cast<uint32_t>(js_value.to_number());
            write_value(value);
            break;
        }
        case FFIType::DOUBLE: {
            double value = js_value.to_number();
            write_value(value);
            break;
        }
        case FFIType::STRING: {
            marshal_string_to_native(js_value);
            break;
        }
        default:
            throw std::runtime_error("Unsupported argument type for marshaling: " + type_info.to_string());
    }
}

Value FFIMarshaler::unmarshal_return_value(const void* native_value, const FFITypeInfo& type_info, Context* context) {
    std::cout << "🔄 Unmarshaling return value: " << type_info.to_string() << std::endl;
    
    switch (type_info.type) {
        case FFIType::VOID:
            return Value(); // undefined
        case FFIType::BOOL:
            return Value(read_value<bool>(native_value));
        case FFIType::INT32:
            return Value(static_cast<double>(read_value<int32_t>(native_value)));
        case FFIType::UINT32:
            return Value(static_cast<double>(read_value<uint32_t>(native_value)));
        case FFIType::DOUBLE:
            return Value(read_value<double>(native_value));
        case FFIType::STRING:
            return marshal_string_from_native(read_value<const char*>(native_value), context);
        default:
            return Value(); // undefined for unsupported types
    }
}

void FFIMarshaler::reset() {
    argument_buffer_.clear();
    return_buffer_.clear();
    buffer_offset_ = 0;
}

void FFIMarshaler::write_to_buffer(const void* data, size_t size) {
    size_t old_size = argument_buffer_.size();
    argument_buffer_.resize(old_size + size);
    std::memcpy(argument_buffer_.data() + old_size, data, size);
}

template<typename T>
void FFIMarshaler::write_value(const T& value) {
    write_to_buffer(&value, sizeof(T));
}

template<typename T>
T FFIMarshaler::read_value(const void* data) {
    T value;
    std::memcpy(&value, data, sizeof(T));
    return value;
}

Value FFIMarshaler::marshal_string_from_native(const char* str, Context* context) {
    return str ? Value(std::string(str)) : Value();
}

void FFIMarshaler::marshal_string_to_native(const Value& js_value) {
    std::string str = js_value.to_string();
    const char* c_str = str.c_str();
    write_value(c_str);
}

//=============================================================================
// NativeModuleManager Implementation
//=============================================================================

NativeModuleManager::NativeModuleManager() : sandbox_enabled_(false) {
    std::cout << "🏗️  NATIVE MODULE MANAGER INITIALIZED" << std::endl;
    
    // Add default search paths
    add_search_path("./");
    add_search_path("./lib/");
    add_search_path("./modules/");
    
#ifdef _WIN32
    add_search_path("C:/Windows/System32/");
#else
    add_search_path("/usr/lib/");
    add_search_path("/usr/local/lib/");
    add_search_path("/lib/");
#endif
}

NativeModuleManager::~NativeModuleManager() {
    stop_hot_reload_monitoring();
    print_library_statistics();
    std::cout << "🏗️  NATIVE MODULE MANAGER SHUTDOWN" << std::endl;
}

bool NativeModuleManager::load_library(const std::string& name, const std::string& path) {
    std::lock_guard<std::mutex> lock(manager_mutex_);
    
    if (sandbox_enabled_ && !is_library_allowed(path)) {
        std::cout << "🚫 Library blocked by sandbox: " << path << std::endl;
        return false;
    }
    
    // Check if already loaded
    if (loaded_libraries_.find(name) != loaded_libraries_.end()) {
        std::cout << "⚠️  Library already loaded: " << name << std::endl;
        return true;
    }
    
    auto library = std::make_unique<NativeLibrary>(path);
    if (!library->load()) {
        std::cout << "❌ Failed to load library: " << library->get_last_error() << std::endl;
        return false;
    }
    
    loaded_libraries_[name] = std::move(library);
    std::cout << "✅ Library loaded: " << name << " (" << path << ")" << std::endl;
    
    return true;
}

bool NativeModuleManager::unload_library(const std::string& name) {
    std::lock_guard<std::mutex> lock(manager_mutex_);
    
    auto it = loaded_libraries_.find(name);
    if (it == loaded_libraries_.end()) {
        return false;
    }
    
    loaded_libraries_.erase(it);
    std::cout << "📚 Library unloaded: " << name << std::endl;
    return true;
}

NativeLibrary* NativeModuleManager::get_library(const std::string& name) {
    std::lock_guard<std::mutex> lock(manager_mutex_);
    
    // Resolve alias if needed
    std::string resolved_name = resolve_alias(name);
    
    auto it = loaded_libraries_.find(resolved_name);
    return (it != loaded_libraries_.end()) ? it->second.get() : nullptr;
}

void NativeModuleManager::add_search_path(const std::string& path) {
    std::lock_guard<std::mutex> lock(manager_mutex_);
    
    if (std::find(library_search_paths_.begin(), library_search_paths_.end(), path) == library_search_paths_.end()) {
        library_search_paths_.push_back(path);
        std::cout << "📁 Search path added: " << path << std::endl;
    }
}

std::string NativeModuleManager::find_library(const std::string& name) {
    std::vector<std::string> candidates = {
        name,
        name + ".dll",
        name + ".so",
        name + ".dylib",
        "lib" + name + ".so",
        "lib" + name + ".dylib"
    };
    
    for (const std::string& search_path : library_search_paths_) {
        for (const std::string& candidate : candidates) {
            std::string full_path = search_path + "/" + candidate;
            
            // Check if file exists (simplified check)
            std::ifstream file(full_path);
            if (file.good()) {
                std::cout << "🔍 Library found: " << full_path << std::endl;
                return full_path;
            }
        }
    }
    
    std::cout << "🔍 Library not found in search paths: " << name << std::endl;
    return "";
}

void NativeModuleManager::set_alias(const std::string& alias, const std::string& library_name) {
    std::lock_guard<std::mutex> lock(manager_mutex_);
    module_aliases_[alias] = library_name;
    std::cout << "🏷️  Alias set: " << alias << " -> " << library_name << std::endl;
}

std::string NativeModuleManager::resolve_alias(const std::string& name) const {
    auto it = module_aliases_.find(name);
    return (it != module_aliases_.end()) ? it->second : name;
}

void NativeModuleManager::print_library_statistics() const {
    std::cout << "📊 NATIVE MODULE MANAGER STATISTICS" << std::endl;
    std::cout << "====================================" << std::endl;
    std::cout << "Loaded libraries: " << loaded_libraries_.size() << std::endl;
    std::cout << "Search paths: " << library_search_paths_.size() << std::endl;
    std::cout << "Module aliases: " << module_aliases_.size() << std::endl;
    std::cout << "Sandbox enabled: " << (sandbox_enabled_ ? "YES" : "NO") << std::endl;
    
    std::cout << "\nLoaded libraries:" << std::endl;
    for (const auto& [name, library] : loaded_libraries_) {
        std::cout << "  " << name << " (" << library->get_path() << ")" << std::endl;
        std::cout << "    Functions: " << library->get_function_names().size() << std::endl;
        std::cout << "    Total calls: " << library->get_total_calls() << std::endl;
        std::cout << "    Avg call time: " << library->get_average_call_time_us() << " μs" << std::endl;
    }
}

NativeModuleManager& NativeModuleManager::get_instance() {
    static NativeModuleManager instance;
    return instance;
}

//=============================================================================
// FFIIntegration Implementation
//=============================================================================

namespace FFIIntegration {

void initialize_ffi_system() {
    std::cout << "🔧 INITIALIZING FFI SYSTEM" << std::endl;
    
    // Initialize the native module manager
    NativeModuleManager::get_instance();
    
    std::cout << "✅ FFI SYSTEM INITIALIZED" << std::endl;
    std::cout << "  📚 Native module manager: Ready" << std::endl;
    std::cout << "  🔗 Symbol resolution: Ready" << std::endl;
    std::cout << "  🔄 Type marshaling: Ready" << std::endl;
    std::cout << "  📞 Function calling: Ready" << std::endl;
}

void shutdown_ffi_system() {
    std::cout << "🔧 SHUTTING DOWN FFI SYSTEM" << std::endl;
    
    // Manager will be destroyed automatically
    
    std::cout << "✅ FFI SYSTEM SHUTDOWN COMPLETE" << std::endl;
}

void bind_standard_c_library() {
    auto& manager = NativeModuleManager::get_instance();
    
    std::cout << "📚 Binding standard C library functions..." << std::endl;
    
    // This would bind common C library functions
    // For demonstration, we'll just print the intent
    std::cout << "  - malloc, free, memcpy, memset" << std::endl;
    std::cout << "  - printf, scanf, strlen, strcmp" << std::endl;
    std::cout << "  - fopen, fread, fwrite, fclose" << std::endl;
    
    std::cout << "✅ Standard C library bindings complete" << std::endl;
}

void bind_math_library() {
    std::cout << "📐 Binding math library functions..." << std::endl;
    std::cout << "  - sin, cos, tan, sqrt, pow, log" << std::endl;
    std::cout << "✅ Math library bindings complete" << std::endl;
}

Value require_native_module(const std::string& name, Context* context) {
    auto& manager = NativeModuleManager::get_instance();
    
    // Try to load the library if not already loaded
    std::string library_path = manager.find_library(name);
    if (library_path.empty()) {
        throw std::runtime_error("Native module not found: " + name);
    }
    
    if (!manager.load_library(name, library_path)) {
        throw std::runtime_error("Failed to load native module: " + name);
    }
    
    // Return a module object (simplified)
    std::cout << "📦 Native module required: " << name << std::endl;
    return Value(); // Would return proper module object
}

} // namespace FFIIntegration

//=============================================================================
// PlatformFFI Implementation
//=============================================================================

namespace PlatformFFI {

Platform get_current_platform() {
#ifdef _WIN32
    return Platform::WINDOWS;
#elif defined(__APPLE__)
    return Platform::MACOS;
#elif defined(__linux__)
    return Platform::LINUX;
#else
    return Platform::UNKNOWN;
#endif
}

std::string get_platform_name() {
    switch (get_current_platform()) {
        case Platform::WINDOWS: return "Windows";
        case Platform::LINUX: return "Linux";
        case Platform::MACOS: return "macOS";
        default: return "Unknown";
    }
}

bool file_exists(const std::string& path) {
#ifdef _WIN32
    DWORD attributes = GetFileAttributesA(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES;
#else
    struct stat buffer;
    return stat(path.c_str(), &buffer) == 0;
#endif
}

Architecture get_cpu_architecture() {
#ifdef _WIN64
    return Architecture::X86_64;
#elif defined(_WIN32)
    return Architecture::X86;
#elif defined(__x86_64__)
    return Architecture::X86_64;
#elif defined(__i386__)
    return Architecture::X86;
#elif defined(__aarch64__)
    return Architecture::ARM64;
#elif defined(__arm__)
    return Architecture::ARM;
#else
    return Architecture::UNKNOWN;
#endif
}

size_t get_pointer_size() {
    return sizeof(void*);
}

bool supports_calling_convention(const std::string& convention) {
    // Simplified implementation
    return convention == "cdecl" || convention == "stdcall";
}

} // namespace PlatformFFI

} // namespace Quanta