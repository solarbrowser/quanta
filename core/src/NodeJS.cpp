#include "../include/NodeJS.h"
#include <iostream>
#include <sstream>
#include <fstream>
#include <string>
#include <random>
#include <ctime>
#include <cstdlib>

// Minimal platform compatibility without problematic headers
#ifdef _WIN32
    #include <stdlib.h>
    #include <stdio.h>
    extern "C" {
        int _mkdir(const char* dirname);
        int _rmdir(const char* dirname);
        int _chdir(const char* dirname);
        char* _getcwd(char* buffer, int maxlen);
        char* getenv(const char* name);
    }
    #define mkdir(path, mode) _mkdir(path)
    #define getcwd _getcwd
    #define PATH_MAX 260
#else
    #include <unistd.h>
    #include <sys/stat.h>
    #ifndef PATH_MAX
        #define PATH_MAX 4096
    #endif
#endif

namespace Quanta {

// Utility function for path safety
bool NodeJS::is_safe_path(const std::string& path) {
    return path.find("..") == std::string::npos && 
           path.find("//") == std::string::npos &&
           path.length() < PATH_MAX;
}

std::string NodeJS::get_current_directory() {
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) != nullptr) {
        return std::string(cwd);
    }
    return ".";
}

std::string NodeJS::get_mime_type(const std::string& filename) {
    size_t pos = filename.find_last_of(".");
    if (pos != std::string::npos) {
        std::string ext = filename.substr(pos);
        if (ext == ".html") return "text/html";
        if (ext == ".css") return "text/css";
        if (ext == ".js") return "application/javascript";
        if (ext == ".json") return "application/json";
        if (ext == ".png") return "image/png";
        if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    }
    return "text/plain";
}

// File System API
Value NodeJS::fs_readFile(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        return Value("Error: Missing filename");
    }
    
    std::string filename = args[0].to_string();
    if (!is_safe_path(filename)) {
        return Value("Error: Unsafe path");
    }
    
    std::ifstream file(filename);
    if (!file.is_open()) {
        return Value("Error: File not found");
    }
    
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    return Value(content);
}

Value NodeJS::fs_writeFile(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.size() < 2) {
        return Value("Error: Missing filename or content");
    }
    
    std::string filename = args[0].to_string();
    std::string content = args[1].to_string();
    
    if (!is_safe_path(filename)) {
        return Value("Error: Unsafe path");
    }
    
    std::ofstream file(filename);
    if (!file.is_open()) {
        return Value("Error: Cannot create file");
    }
    
    file << content;
    return Value("File written successfully");
}

Value NodeJS::fs_appendFile(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.size() < 2) {
        return Value("Error: Missing filename or content");
    }
    
    std::string filename = args[0].to_string();
    std::string content = args[1].to_string();
    
    if (!is_safe_path(filename)) {
        return Value("Error: Unsafe path");
    }
    
    std::ofstream file(filename, std::ios::app);
    if (!file.is_open()) {
        return Value("Error: Cannot open file");
    }
    
    file << content;
    return Value("Content appended successfully");
}

Value NodeJS::fs_exists(Context& ctx, const std::vector<Value>& args) {
    return fs_existsSync(ctx, args);
}

Value NodeJS::fs_existsSync(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        return Value(false);
    }
    
    std::string filename = args[0].to_string();
    if (!is_safe_path(filename)) {
        return Value(false);
    }
    
    std::ifstream file(filename);
    return Value(file.good());
}

Value NodeJS::fs_mkdir(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        return Value("Error: Missing directory name");
    }
    
    std::string dirname = args[0].to_string();
    if (!is_safe_path(dirname)) {
        return Value("Error: Unsafe path");
    }
    
    int result = mkdir(dirname.c_str(), 0755);
    if (result == 0) {
        return Value("Directory created successfully");
    } else {
        return Value("Error: Cannot create directory");
    }
}

Value NodeJS::fs_rmdir(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        return Value("Error: Missing directory name");
    }
    
    std::string dirname = args[0].to_string();
    if (!is_safe_path(dirname)) {
        return Value("Error: Unsafe path");
    }
    
#ifdef _WIN32
    int result = _rmdir(dirname.c_str());
#else
    int result = rmdir(dirname.c_str());
#endif
    if (result == 0) {
        return Value("Directory removed successfully");
    } else {
        return Value("Error: Cannot remove directory");
    }
}

Value NodeJS::fs_unlink(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        return Value("Error: Missing filename");
    }
    
    std::string filename = args[0].to_string();
    if (!is_safe_path(filename)) {
        return Value("Error: Unsafe path");
    }
    
    int result = remove(filename.c_str());
    if (result == 0) {
        return Value("File deleted successfully");
    } else {
        return Value("Error: Cannot delete file");
    }
}

Value NodeJS::fs_stat(Context& ctx, const std::vector<Value>& args) {
    return fs_statSync(ctx, args);
}

Value NodeJS::fs_readdir(Context& ctx, const std::vector<Value>& args) {
    return fs_readdirSync(ctx, args);
}

Value NodeJS::fs_readFileSync(Context& ctx, const std::vector<Value>& args) {
    return fs_readFile(ctx, args);
}

Value NodeJS::fs_writeFileSync(Context& ctx, const std::vector<Value>& args) {
    return fs_writeFile(ctx, args);
}

Value NodeJS::fs_mkdirSync(Context& ctx, const std::vector<Value>& args) {
    return fs_mkdir(ctx, args);
}

Value NodeJS::fs_statSync(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        return Value("Error: Missing filename");
    }
    
    std::string filename = args[0].to_string();
    if (!is_safe_path(filename)) {
        return Value("Error: Unsafe path");
    }
    
    // Simple stat implementation
    std::ifstream file(filename);
    if (file.good()) {
        file.seekg(0, std::ios::end);
        size_t size = file.tellg();
        file.close();
        
        auto stat_obj = ObjectFactory::create_object();
        stat_obj->set_property("size", Value(static_cast<double>(size)));
        stat_obj->set_property("isFile", Value(true));
        stat_obj->set_property("isDirectory", Value(false));
        return Value(stat_obj.release());
    }
    
    return Value("Error: File not found");
}

Value NodeJS::fs_readdirSync(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        return Value("Error: Missing directory name");
    }
    
    // Simple placeholder implementation
    auto files_array = ObjectFactory::create_array();
    files_array->push(Value("placeholder.txt"));
    return Value(files_array.release());
}

// Path API
Value NodeJS::path_join(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        return Value("");
    }
    
    std::string result = args[0].to_string();
    for (size_t i = 1; i < args.size(); i++) {
        std::string part = args[i].to_string();
        if (!result.empty() && result.back() != '/' && result.back() != '\\') {
#ifdef _WIN32
            result += "\\";
#else
            result += "/";
#endif
        }
        result += part;
    }
    return Value(result);
}

Value NodeJS::path_resolve(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    std::string result = get_current_directory();
    for (const auto& arg : args) {
        std::string path = arg.to_string();
        if (!path.empty()) {
#ifdef _WIN32
            if (path[0] != '\\' && (path.length() < 2 || path[1] != ':')) {
                result += "\\" + path;
            } else {
                result = path;
            }
#else
            if (path[0] != '/') {
                result += "/" + path;
            } else {
                result = path;
            }
#endif
        }
    }
    return Value(result);
}

Value NodeJS::path_dirname(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        return Value(".");
    }
    
    std::string path = args[0].to_string();
    size_t pos = path.find_last_of("/\\");
    if (pos != std::string::npos) {
        return Value(path.substr(0, pos));
    }
    return Value(".");
}

Value NodeJS::path_basename(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        return Value("");
    }
    
    std::string path = args[0].to_string();
    size_t pos = path.find_last_of("/\\");
    if (pos != std::string::npos) {
        return Value(path.substr(pos + 1));
    }
    return Value(path);
}

Value NodeJS::path_extname(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        return Value("");
    }
    
    std::string path = args[0].to_string();
    size_t pos = path.find_last_of(".");
    size_t slash_pos = path.find_last_of("/\\");
    
    if (pos != std::string::npos && (slash_pos == std::string::npos || pos > slash_pos)) {
        return Value(path.substr(pos));
    }
    return Value("");
}

Value NodeJS::path_normalize(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        return Value("");
    }
    
    // Simple normalization - remove double slashes
    std::string path = args[0].to_string();
    std::string result;
    bool lastWasSlash = false;
    
    for (char c : path) {
        if (c == '/' || c == '\\') {
            if (!lastWasSlash) {
#ifdef _WIN32
                result += '\\';
#else
                result += '/';
#endif
                lastWasSlash = true;
            }
        } else {
            result += c;
            lastWasSlash = false;
        }
    }
    
    return Value(result);
}

Value NodeJS::path_isAbsolute(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        return Value(false);
    }
    
    std::string path = args[0].to_string();
    if (path.empty()) {
        return Value(false);
    }
    
#ifdef _WIN32
    return Value((path.length() >= 2 && path[1] == ':') || path[0] == '\\');
#else
    return Value(path[0] == '/');
#endif
}

// HTTP API (placeholders)
Value NodeJS::http_createServer(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    return Value("HTTP server placeholder");
}

Value NodeJS::http_request(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    return Value("HTTP request placeholder");
}

Value NodeJS::http_get(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    return Value("HTTP GET placeholder");
}

// OS API
Value NodeJS::os_platform(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
#ifdef _WIN32
    return Value("win32");
#elif defined(__linux__)
    return Value("linux");
#elif defined(__APPLE__)
    return Value("darwin");
#else
    return Value("unknown");
#endif
}

Value NodeJS::os_arch(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
#ifdef _WIN64
    return Value("x64");
#elif defined(_WIN32)
    return Value("x86");
#elif defined(__x86_64__)
    return Value("x64");
#elif defined(__i386__)
    return Value("x86");
#elif defined(__arm__)
    return Value("arm");
#elif defined(__aarch64__)
    return Value("arm64");
#else
    return Value("unknown");
#endif
}

Value NodeJS::os_cpus(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    auto cpus_array = ObjectFactory::create_array();
    auto cpu_obj = ObjectFactory::create_object();
    cpu_obj->set_property("model", Value("Generic CPU"));
    cpu_obj->set_property("speed", Value(2400.0));
    cpus_array->push(Value(cpu_obj.release()));
    return Value(cpus_array.release());
}

Value NodeJS::os_hostname(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    std::string hostname = "localhost";
#ifdef _WIN32
    const char* computer_name = getenv("COMPUTERNAME");
    if (computer_name) {
        hostname = std::string(computer_name);
    }
#else
    char host_buffer[256];
    if (gethostname(host_buffer, sizeof(host_buffer)) == 0) {
        hostname = std::string(host_buffer);
    }
#endif
    return Value(hostname);
}

Value NodeJS::os_homedir(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
#ifdef _WIN32
    const char* home = getenv("USERPROFILE");
    if (!home) home = getenv("HOMEDRIVE");
#else
    const char* home = getenv("HOME");
#endif
    if (home) {
        return Value(std::string(home));
    }
    return Value(".");
}

Value NodeJS::os_tmpdir(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
#ifdef _WIN32
    const char* tmp = getenv("TEMP");
    if (!tmp) tmp = getenv("TMP");
    if (!tmp) tmp = "C:\\temp";
#else
    const char* tmp = getenv("TMPDIR");
    if (!tmp) tmp = "/tmp";
#endif
    return Value(std::string(tmp));
}

// Process API
Value NodeJS::process_exit(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    int code = 0;
    if (!args.empty()) {
        code = static_cast<int>(args[0].to_number());
    }
    std::cout << "Process exit with code: " << code << std::endl;
    exit(code);
    return Value();
}

Value NodeJS::process_cwd(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    return Value(get_current_directory());
}

Value NodeJS::process_chdir(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        return Value("Error: Missing directory");
    }
    
    std::string dir = args[0].to_string();
    if (!is_safe_path(dir)) {
        return Value("Error: Unsafe path");
    }
    
#ifdef _WIN32
    int result = _chdir(dir.c_str());
#else
    int result = chdir(dir.c_str());
#endif
    
    if (result == 0) {
        return Value("Directory changed successfully");
    } else {
        return Value("Error: Cannot change directory");
    }
}

Value NodeJS::process_env_get(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        return Value("Error: Missing environment variable name");
    }
    
    std::string var_name = args[0].to_string();
    const char* value = getenv(var_name.c_str());
    if (value) {
        return Value(std::string(value));
    }
    return Value();
}

// Crypto API
Value NodeJS::crypto_randomBytes(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    int length = 16;
    if (!args.empty()) {
        length = static_cast<int>(args[0].to_number());
        if (length < 0 || length > 1024) length = 16;
    }
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);
    
    std::string result;
    for (int i = 0; i < length; i++) {
        char hex[3];
        sprintf(hex, "%02x", dis(gen));
        result += hex;
    }
    
    return Value(result);
}

Value NodeJS::crypto_createHash(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    return Value("Hash placeholder");
}

// Util API
Value NodeJS::util_format(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        return Value("");
    }
    
    std::string result = args[0].to_string();
    return Value(result);
}

Value NodeJS::util_inspect(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        return Value("undefined");
    }
    
    return Value(args[0].to_string());
}

// Events API
Value NodeJS::events_EventEmitter(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    auto emitter = ObjectFactory::create_object();
    emitter->set_property("emit", Value("EventEmitter placeholder"));
    return Value(emitter.release());
}

} // namespace Quanta