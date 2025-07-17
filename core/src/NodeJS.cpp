#include "../include/NodeJS.h"
#include <iostream>
#include <sstream>
#include <cstdlib>
#include <random>
#include <ctime>
#include <unistd.h>
#include <sys/stat.h>

namespace Quanta {

// File System API
Value NodeJS::fs_readFile(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        std::cout << "fs.readFile: Missing filename" << std::endl;
        return Value("Error: Missing filename");
    }
    
    std::string filename = args[0].to_string();
    if (!is_safe_path(filename)) {
        std::cout << "fs.readFile: Unsafe path detected" << std::endl;
        return Value("Error: Unsafe path");
    }
    
    std::cout << "fs.readFile: Reading file '" << filename << "' (async simulated)" << std::endl;
    
    // Simulate async operation
    std::ifstream file(filename);
    if (!file.is_open()) {
        return Value("Error: File not found");
    }
    
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();
    
    return Value(content);
}

Value NodeJS::fs_writeFile(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.size() < 2) {
        std::cout << "fs.writeFile: Missing filename or data" << std::endl;
        return Value("Error: Missing parameters");
    }
    
    std::string filename = args[0].to_string();
    std::string data = args[1].to_string();
    
    if (!is_safe_path(filename)) {
        std::cout << "fs.writeFile: Unsafe path detected" << std::endl;
        return Value("Error: Unsafe path");
    }
    
    std::cout << "fs.writeFile: Writing to file '" << filename << "' (async simulated)" << std::endl;
    
    std::ofstream file(filename);
    if (!file.is_open()) {
        return Value("Error: Cannot write file");
    }
    
    file << data;
    file.close();
    
    std::cout << "fs.writeFile: File written successfully" << std::endl;
    return Value("Success");
}

Value NodeJS::fs_readFileSync(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        std::cout << "fs.readFileSync: Missing filename" << std::endl;
        return Value("Error: Missing filename");
    }
    
    std::string filename = args[0].to_string();
    if (!is_safe_path(filename)) {
        std::cout << "fs.readFileSync: Unsafe path detected" << std::endl;
        return Value("Error: Unsafe path");
    }
    
    std::cout << "fs.readFileSync: Reading file '" << filename << "' synchronously" << std::endl;
    
    std::ifstream file(filename);
    if (!file.is_open()) {
        return Value("Error: File not found");
    }
    
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();
    
    return Value(content);
}

Value NodeJS::fs_writeFileSync(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.size() < 2) {
        std::cout << "fs.writeFileSync: Missing filename or data" << std::endl;
        return Value("Error: Missing parameters");
    }
    
    std::string filename = args[0].to_string();
    std::string data = args[1].to_string();
    
    if (!is_safe_path(filename)) {
        std::cout << "fs.writeFileSync: Unsafe path detected" << std::endl;
        return Value("Error: Unsafe path");
    }
    
    std::cout << "fs.writeFileSync: Writing to file '" << filename << "' synchronously" << std::endl;
    
    std::ofstream file(filename);
    if (!file.is_open()) {
        return Value("Error: Cannot write file");
    }
    
    file << data;
    file.close();
    
    std::cout << "fs.writeFileSync: File written successfully" << std::endl;
    return Value("Success");
}

Value NodeJS::fs_existsSync(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        return Value(false);
    }
    
    std::string filename = args[0].to_string();
    std::cout << "fs.existsSync: Checking if '" << filename << "' exists" << std::endl;
    
    std::ifstream file(filename);
    bool exists = file.good();
    file.close();
    
    std::cout << "fs.existsSync: File " << (exists ? "exists" : "does not exist") << std::endl;
    return Value(exists);
}

Value NodeJS::fs_mkdirSync(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        return Value("Error: Missing directory name");
    }
    
    std::string dirname = args[0].to_string();
    std::cout << "fs.mkdirSync: Creating directory '" << dirname << "'" << std::endl;
    
    if (mkdir(dirname.c_str(), 0755) == 0) {
        std::cout << "fs.mkdirSync: Directory created successfully" << std::endl;
        return Value("Success");
    } else {
        std::cout << "fs.mkdirSync: Failed to create directory" << std::endl;
        return Value("Error: Cannot create directory");
    }
}

Value NodeJS::fs_readdirSync(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        return Value("Error: Missing directory name");
    }
    
    std::string dirname = args[0].to_string();
    std::cout << "fs.readdirSync: Reading directory '" << dirname << "'" << std::endl;
    
    // Simulate directory reading
    return Value("['file1.txt', 'file2.js', 'subdirectory']");
}

// Path API
Value NodeJS::path_join(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        return Value("");
    }
    
    std::string result = args[0].to_string();
    for (size_t i = 1; i < args.size(); ++i) {
        result += "/" + args[i].to_string();
    }
    
    std::cout << "path.join: Joined path: " << result << std::endl;
    return Value(result);
}

Value NodeJS::path_dirname(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        return Value(".");
    }
    
    std::string path = args[0].to_string();
    size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos) {
        return Value(".");
    }
    
    std::string dirname = path.substr(0, pos);
    std::cout << "path.dirname: " << dirname << std::endl;
    return Value(dirname);
}

Value NodeJS::path_basename(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        return Value("");
    }
    
    std::string path = args[0].to_string();
    size_t pos = path.find_last_of("/\\");
    std::string basename = (pos == std::string::npos) ? path : path.substr(pos + 1);
    
    std::cout << "path.basename: " << basename << std::endl;
    return Value(basename);
}

Value NodeJS::path_extname(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        return Value("");
    }
    
    std::string path = args[0].to_string();
    size_t pos = path.find_last_of('.');
    if (pos == std::string::npos) {
        return Value("");
    }
    
    std::string extname = path.substr(pos);
    std::cout << "path.extname: " << extname << std::endl;
    return Value(extname);
}

// HTTP API (basic simulation)
Value NodeJS::http_createServer(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    std::cout << "http.createServer: Created HTTP server (simulated)" << std::endl;
    return Value("HTTP Server object");
}

Value NodeJS::http_request(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        return Value("Error: Missing options");
    }
    
    std::string options = args[0].to_string();
    std::cout << "http.request: Making HTTP request to " << options << " (simulated)" << std::endl;
    return Value("HTTP Response: 200 OK");
}

Value NodeJS::http_get(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        return Value("Error: Missing URL");
    }
    
    std::string url = args[0].to_string();
    std::cout << "http.get: GET request to " << url << " (simulated)" << std::endl;
    return Value("HTTP Response: 200 OK");
}

// OS API
Value NodeJS::os_platform(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    #ifdef _WIN32
        return Value("win32");
    #elif __APPLE__
        return Value("darwin");
    #elif __linux__
        return Value("linux");
    #else
        return Value("unknown");
    #endif
}

Value NodeJS::os_arch(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    #ifdef __x86_64__
        return Value("x64");
    #elif __i386__
        return Value("ia32");
    #elif __arm__
        return Value("arm");
    #else
        return Value("unknown");
    #endif
}

Value NodeJS::os_hostname(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    char hostname[256];
    gethostname(hostname, sizeof(hostname));
    std::cout << "os.hostname: " << hostname << std::endl;
    return Value(std::string(hostname));
}

Value NodeJS::os_homedir(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    const char* home = getenv("HOME");
    if (!home) {
        home = getenv("USERPROFILE"); // Windows
    }
    if (!home) {
        home = "/tmp";
    }
    std::cout << "os.homedir: " << home << std::endl;
    return Value(std::string(home));
}

Value NodeJS::os_tmpdir(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    const char* tmp = getenv("TMPDIR");
    if (!tmp) {
        tmp = getenv("TMP");
    }
    if (!tmp) {
        tmp = "/tmp";
    }
    std::cout << "os.tmpdir: " << tmp << std::endl;
    return Value(std::string(tmp));
}

// Process API
Value NodeJS::process_exit(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    int code = args.empty() ? 0 : static_cast<int>(args[0].to_number());
    std::cout << "process.exit: Exiting with code " << code << " (simulated)" << std::endl;
    return Value("Process exited");
}

Value NodeJS::process_cwd(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    std::string cwd = get_current_directory();
    std::cout << "process.cwd: " << cwd << std::endl;
    return Value(cwd);
}

Value NodeJS::process_env_get(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        return Value("Error: Missing environment variable name");
    }
    
    std::string varname = args[0].to_string();
    const char* value = getenv(varname.c_str());
    std::string result = value ? std::string(value) : "";
    std::cout << "process.env." << varname << ": " << result << std::endl;
    return Value(result);
}

// Crypto API (basic)
Value NodeJS::crypto_randomBytes(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    int size = args.empty() ? 16 : static_cast<int>(args[0].to_number());
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);
    
    std::ostringstream oss;
    for (int i = 0; i < size; ++i) {
        oss << std::hex << dis(gen);
    }
    
    std::string result = oss.str();
    std::cout << "crypto.randomBytes: Generated " << size << " random bytes" << std::endl;
    return Value(result);
}

Value NodeJS::crypto_createHash(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    std::string algorithm = args.empty() ? "sha256" : args[0].to_string();
    std::cout << "crypto.createHash: Created " << algorithm << " hash object (simulated)" << std::endl;
    return Value("Hash object for " + algorithm);
}

// Util API
Value NodeJS::util_format(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        return Value("");
    }
    
    std::string format = args[0].to_string();
    std::cout << "util.format: Formatted string: " << format << std::endl;
    return Value(format);
}

Value NodeJS::util_inspect(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        return Value("undefined");
    }
    
    std::string value = args[0].to_string();
    std::cout << "util.inspect: Inspected value: " << value << std::endl;
    return Value("Inspected: " + value);
}

// Helper functions
std::string NodeJS::get_current_directory() {
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        return std::string(cwd);
    }
    return "/";
}

bool NodeJS::is_safe_path(const std::string& path) {
    // Basic security check - prevent directory traversal
    return path.find("..") == std::string::npos && 
           path.find("/etc/") == std::string::npos &&
           path.find("/sys/") == std::string::npos &&
           path.find("/proc/") == std::string::npos;
}

} // namespace Quanta