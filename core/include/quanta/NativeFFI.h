/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <vector>
#include <unordered_map>
#include <memory>
#include <string>
#include <functional>
#include <cstdint>
#include <mutex>

namespace Quanta {

class Value;
class Context;



enum class FFIType : uint8_t {
    VOID = 0,
    BOOL,
    INT8,
    UINT8,
    INT16,
    UINT16,
    INT32,
    UINT32,
    INT64,
    UINT64,
    FLOAT,
    DOUBLE,
    POINTER,
    STRING,
    BUFFER,
    FUNCTION,
    STRUCT,
    ARRAY
};

struct FFITypeInfo {
    FFIType type;
    size_t size;
    size_t alignment;
    std::string name;
    
    std::vector<FFITypeInfo> fields;
    FFITypeInfo* element_type;
    size_t array_length;
    
    FFITypeInfo(FFIType t = FFIType::VOID) 
        : type(t), size(0), alignment(0), element_type(nullptr), array_length(0) {
        calculate_size_and_alignment();
    }
    
    void calculate_size_and_alignment();
    bool is_primitive() const;
    bool is_composite() const;
    std::string to_string() const;
};


struct FFISignature {
    std::string name;
    FFITypeInfo return_type;
    std::vector<FFITypeInfo> parameter_types;
    bool is_variadic;
    std::string calling_convention;
    
    FFISignature() : is_variadic(false), calling_convention("cdecl") {}
    
    std::string to_string() const;
    bool matches(const std::vector<Value>& args) const;
    size_t get_stack_size() const;
};


class NativeLibrary {
private:
    std::string library_path_;
    void* library_handle_;
    std::unordered_map<std::string, void*> symbols_;
    std::unordered_map<std::string, FFISignature> function_signatures_;
    bool is_loaded_;
    std::string last_error_;
    
    uint64_t total_calls_;
    uint64_t total_call_time_ns_;
    std::unordered_map<std::string, uint64_t> function_call_counts_;

public:
    NativeLibrary(const std::string& path);
    ~NativeLibrary();
    
    bool load();
    void unload();
    bool reload();
    bool is_loaded() const { return is_loaded_; }
    
    void* get_symbol(const std::string& name);
    bool has_symbol(const std::string& name) const;
    std::vector<std::string> get_symbol_names() const;
    
    void register_function(const std::string& name, const FFISignature& signature);
    void register_function(const std::string& name, FFIType return_type, const std::vector<FFIType>& param_types);
    bool has_function(const std::string& name) const;
    const FFISignature* get_function_signature(const std::string& name) const;
    
    Value call_function(const std::string& name, const std::vector<Value>& args, Context* context);
    Value call_function_ptr(void* func_ptr, const FFISignature& signature, const std::vector<Value>& args, Context* context);
    
    uint64_t get_total_calls() const { return total_calls_; }
    double get_average_call_time_us() const;
    uint64_t get_function_call_count(const std::string& name) const;
    void print_performance_stats() const;
    
    std::string get_last_error() const { return last_error_; }
    void clear_error() { last_error_.clear(); }
    
    std::string get_path() const { return library_path_; }
    std::vector<std::string> get_function_names() const;

private:
    void set_error(const std::string& error) { last_error_ = error; }
    void* load_library_platform(const std::string& path);
    void unload_library_platform(void* handle);
    void* get_symbol_platform(void* handle, const std::string& name);
    std::string get_platform_error();
};


class FFIMarshaler {
private:
    std::vector<uint8_t> argument_buffer_;
    std::vector<uint8_t> return_buffer_;
    size_t buffer_offset_;

public:
    FFIMarshaler();
    ~FFIMarshaler();
    
    void marshal_argument(const Value& js_value, const FFITypeInfo& type_info);
    void* get_argument_buffer() { return argument_buffer_.data(); }
    size_t get_argument_buffer_size() const { return argument_buffer_.size(); }
    
    Value unmarshal_return_value(const void* native_value, const FFITypeInfo& type_info, Context* context);
    void prepare_return_buffer(size_t size);
    void* get_return_buffer() { return return_buffer_.data(); }
    
    void reset();
    void reserve_argument_space(size_t size);
    
    static bool can_convert_to_native(const Value& js_value, const FFITypeInfo& type_info);
    static bool can_convert_from_native(const FFITypeInfo& type_info);

private:
    void write_to_buffer(const void* data, size_t size);
    template<typename T> void write_value(const T& value);
    template<typename T> T read_value(const void* data);
    
    Value marshal_string_from_native(const char* str, Context* context);
    Value marshal_buffer_from_native(const void* data, size_t size, Context* context);
    Value marshal_struct_from_native(const void* data, const FFITypeInfo& type_info, Context* context);
    Value marshal_array_from_native(const void* data, const FFITypeInfo& type_info, Context* context);
    
    void marshal_string_to_native(const Value& js_value);
    void marshal_buffer_to_native(const Value& js_value);
    void marshal_struct_to_native(const Value& js_value, const FFITypeInfo& type_info);
    void marshal_array_to_native(const Value& js_value, const FFITypeInfo& type_info);
};


class FFICallDispatcher {
private:
    std::unordered_map<std::string, std::function<Value(void*, const FFISignature&, const std::vector<Value>&, Context*)>> convention_handlers_;
    
    struct CallCache {
        void* function_pointer;
        FFISignature signature;
        std::unique_ptr<FFIMarshaler> marshaler;
        uint64_t call_count;
        uint64_t total_time_ns;
    };
    
    std::unordered_map<std::string, std::unique_ptr<CallCache>> call_cache_;
    mutable std::mutex cache_mutex_;

public:
    FFICallDispatcher();
    ~FFICallDispatcher();
    
    Value dispatch_call(void* func_ptr, const FFISignature& signature, const std::vector<Value>& args, Context* context);
    
    void register_calling_convention(const std::string& name, 
        std::function<Value(void*, const FFISignature&, const std::vector<Value>&, Context*)> handler);
    bool supports_calling_convention(const std::string& convention) const;
    
    void cache_function_call(const std::string& name, void* func_ptr, const FFISignature& signature);
    void clear_call_cache();
    void optimize_hot_calls();
    
    void print_call_statistics() const;
    double get_cache_hit_ratio() const;

private:
    Value call_cdecl(void* func_ptr, const FFISignature& signature, const std::vector<Value>& args, Context* context);
    Value call_stdcall(void* func_ptr, const FFISignature& signature, const std::vector<Value>& args, Context* context);
    Value call_fastcall(void* func_ptr, const FFISignature& signature, const std::vector<Value>& args, Context* context);
    
    Value invoke_native_function(void* func_ptr, void* args, size_t stack_size, const FFITypeInfo& return_type, Context* context);
    void setup_call_stack(void* args, const std::vector<Value>& js_args, const FFISignature& signature);
};


class NativeModuleManager {
private:
    std::unordered_map<std::string, std::unique_ptr<NativeLibrary>> loaded_libraries_;
    std::vector<std::string> library_search_paths_;
    std::unordered_map<std::string, std::string> module_aliases_;
    
    struct ModuleWatcher {
        std::string file_path;
        uint64_t last_modified;
        bool auto_reload;
    };
    
    std::unordered_map<std::string, ModuleWatcher> watched_modules_;
    std::thread hot_reload_thread_;
    std::atomic<bool> should_stop_watching_{false};
    
    std::vector<std::string> allowed_libraries_;
    std::vector<std::string> blocked_symbols_;
    bool sandbox_enabled_;
    
    mutable std::mutex manager_mutex_;

public:
    NativeModuleManager();
    ~NativeModuleManager();
    
    bool load_library(const std::string& name, const std::string& path);
    bool unload_library(const std::string& name);
    bool reload_library(const std::string& name);
    bool is_library_loaded(const std::string& name) const;
    NativeLibrary* get_library(const std::string& name);
    
    void add_search_path(const std::string& path);
    void remove_search_path(const std::string& path);
    std::vector<std::string> get_search_paths() const;
    std::string find_library(const std::string& name);
    
    void set_alias(const std::string& alias, const std::string& library_name);
    void remove_alias(const std::string& alias);
    std::string resolve_alias(const std::string& name) const;
    
    void enable_hot_reload(const std::string& library_name, bool enable = true);
    void start_hot_reload_monitoring();
    void stop_hot_reload_monitoring();
    
    void enable_sandbox(bool enable = true) { sandbox_enabled_ = enable; }
    void add_allowed_library(const std::string& pattern);
    void block_symbol(const std::string& symbol_pattern);
    bool is_library_allowed(const std::string& path) const;
    bool is_symbol_allowed(const std::string& symbol) const;
    
    void register_global_function(const std::string& name, const std::string& library, const std::string& symbol, const FFISignature& signature);
    Value call_global_function(const std::string& name, const std::vector<Value>& args, Context* context);
    
    std::vector<std::string> get_loaded_library_names() const;
    std::vector<std::string> get_available_functions(const std::string& library_name) const;
    const FFISignature* get_function_signature(const std::string& library_name, const std::string& function_name) const;
    
    void print_library_statistics() const;
    void print_hot_reload_status() const;
    void export_library_info(const std::string& filename) const;
    
    static NativeModuleManager& get_instance();

private:
    void hot_reload_monitoring_loop();
    bool check_file_modified(const std::string& file_path, uint64_t& last_modified);
    uint64_t get_file_modification_time(const std::string& file_path);
    void handle_library_reload(const std::string& library_name);
};


namespace FFIHelpers {
    template<typename T> FFITypeInfo get_ffi_type();
    template<typename T> FFIType get_ffi_type_enum();
    
    FFISignature make_signature(const std::string& name, FFIType return_type, const std::vector<FFIType>& param_types);
    template<typename R, typename... Args> FFISignature make_signature(const std::string& name);
    
    template<typename T> Value native_to_js(const T& value, Context* context);
    template<typename T> T js_to_native(const Value& value);
    
    FFITypeInfo define_struct(const std::string& name, const std::vector<std::pair<std::string, FFITypeInfo>>& fields);
    FFITypeInfo define_array(const FFITypeInfo& element_type, size_t length);
    FFITypeInfo define_pointer(const FFITypeInfo& pointed_type);
    
    void bind_function(NativeLibrary& lib, const std::string& name, void* func_ptr, const FFISignature& signature);
    template<typename R, typename... Args> void bind_function(NativeLibrary& lib, const std::string& name, R(*func)(Args...));
    
    void throw_ffi_error(const std::string& message);
    void throw_type_error(const std::string& expected, const std::string& actual);
    void throw_arity_error(size_t expected, size_t actual);
}


namespace FFIIntegration {
    void initialize_ffi_system();
    void shutdown_ffi_system();
    
    void register_ffi_globals(Context* context);
    void register_library_functions(Context* context);
    void register_type_constructors(Context* context);
    
    void bind_standard_c_library();
    void bind_math_library();
    void bind_string_library();
    void bind_system_library();
    void bind_file_io_library();
    
    Value require_native_module(const std::string& name, Context* context);
    Value create_native_function(const std::string& library, const std::string& function, const FFISignature& signature, Context* context);
    Value create_struct_constructor(const FFITypeInfo& struct_type, Context* context);
    
    void register_native_memory_with_gc(void* ptr, size_t size, std::function<void()> finalizer);
    void track_native_allocation(void* ptr, size_t size);
    void track_native_deallocation(void* ptr);
    
    void optimize_ffi_calls();
    void precompile_hot_functions();
    void enable_ffi_jit_compilation();
}


namespace PlatformFFI {
    enum class Platform {
        WINDOWS,
        LINUX,
        MACOS,
        UNKNOWN
    };
    
    Platform get_current_platform();
    std::string get_platform_name();
    
    void* load_dynamic_library(const std::string& path);
    void unload_dynamic_library(void* handle);
    void* get_library_symbol(void* handle, const std::string& name);
    std::string get_library_error();
    
    bool file_exists(const std::string& path);
    uint64_t get_file_size(const std::string& path);
    uint64_t get_file_modified_time(const std::string& path);
    std::vector<std::string> list_directory(const std::string& path);
    
    void* allocate_executable_memory(size_t size);
    void free_executable_memory(void* ptr, size_t size);
    void make_memory_executable(void* ptr, size_t size);
    void make_memory_writable(void* ptr, size_t size);
    
    enum class Architecture {
        X86,
        X86_64,
        ARM,
        ARM64,
        UNKNOWN
    };
    
    Architecture get_cpu_architecture();
    bool supports_calling_convention(const std::string& convention);
    size_t get_pointer_size();
    size_t get_register_size();
    
    void* create_mutex();
    void destroy_mutex(void* mutex);
    void lock_mutex(void* mutex);
    void unlock_mutex(void* mutex);
}

}
