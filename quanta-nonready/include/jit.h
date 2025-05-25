// Stage 3: JIT Compiler & Optimizer - Main JIT Engine
// Purpose: Just-In-Time compilation for hot code paths
// Max Lines: 5000 (Current: ~200)

#ifndef QUANTA_JIT_H
#define QUANTA_JIT_H

#include "ast.h"
#include "env.h"
#include "error.h"
#include "ir.h"
#include <memory>
#include <unordered_map>
#include <vector>
#include <functional>

namespace Quanta {

//<---------FORWARD DECLARATIONS--------->
class IRGenerator;
class NativeCodeEmitter;
class HotPathProfiler;

//<---------COMPILED FUNCTION TYPE--------->
using CompiledFunction = std::function<JSValue(const std::vector<JSValue>&)>;

//<---------JIT COMPILATION STATISTICS--------->
struct JITStats {
    size_t compilationsCount = 0;
    size_t executionCount = 0;
    size_t bytecodeSize = 0;
    size_t nativeCodeSize = 0;
    double averageExecutionTime = 0.0;
    double compilationTime = 0.0;
};

//<---------HOT FUNCTION INFO--------->
struct HotFunction {
    const ASTNode* astNode;
    size_t callCount = 0;
    double totalExecutionTime = 0.0;
    bool isCompiled = false;
    CompiledFunction compiledCode;
    JITStats stats;
    
    HotFunction(const ASTNode* node) : astNode(node) {}
};

//<---------JIT COMPILER CLASS--------->
class JITCompiler {
private:
    std::shared_ptr<IRGenerator> irGenerator;
    std::shared_ptr<NativeCodeEmitter> codeEmitter;
    std::shared_ptr<HotPathProfiler> profiler;
    ErrorHandler& errorHandler;
    
    // Hot function tracking
    std::unordered_map<const ASTNode*, std::shared_ptr<HotFunction>> hotFunctions;
    
    // JIT Configuration
    size_t hotThreshold = 10;        // Calls before JIT compilation
    double minExecutionTime = 0.001; // Minimum execution time to consider
    bool enableOptimizations = true;
    
    // Statistics
    JITStats globalStats;

public:
    JITCompiler(ErrorHandler& errorHandler);
    ~JITCompiler() = default;
    
    // Main JIT interface
    JSValue executeFunction(const ASTNode* functionNode, 
                           const std::vector<JSValue>& args,
                           ScopeManager& scopeManager);
    
    // Hot path management
    void recordExecution(const ASTNode* node, double executionTime);
    bool shouldCompile(const ASTNode* node);
    void compileFunction(const ASTNode* node);
    
    // JIT Configuration
    void setHotThreshold(size_t threshold) { hotThreshold = threshold; }
    void setMinExecutionTime(double time) { minExecutionTime = time; }
    void enableOptimization(bool enable) { enableOptimizations = enable; }
    
    // Statistics and debugging
    JITStats getGlobalStats() const { return globalStats; }
    JITStats getFunctionStats(const ASTNode* node) const;
    void printHotFunctions() const;
    void clearStats();
    
    // Memory management
    void cleanupUnusedFunctions();
    size_t getCompiledFunctionCount() const { return hotFunctions.size(); }
};

//<---------HOT PATH PROFILER--------->
class HotPathProfiler {
private:
    struct ProfileData {
        size_t callCount = 0;
        double totalTime = 0.0;
        double averageTime = 0.0;
        bool isHot = false;
    };
    
    std::unordered_map<const ASTNode*, ProfileData> profiles;
    size_t hotThreshold;
    double minExecutionTime;

public:
    HotPathProfiler(size_t threshold = 10, double minTime = 0.001);
    
    void recordCall(const ASTNode* node, double executionTime);
    bool isHotPath(const ASTNode* node) const;
    ProfileData getProfile(const ASTNode* node) const;
    void clearProfiles();
    
    // Configuration
    void setHotThreshold(size_t threshold) { hotThreshold = threshold; }
    void setMinExecutionTime(double time) { minExecutionTime = time; }
    
    // Statistics
    std::vector<const ASTNode*> getHotPaths() const;
    size_t getProfileCount() const { return profiles.size(); }
};

//<---------JIT OPTIMIZATION HINTS--------->
enum class OptimizationHint {
    NONE,
    INLINE_SMALL_FUNCTIONS,
    CONSTANT_FOLDING,
    DEAD_CODE_ELIMINATION,
    LOOP_OPTIMIZATION,
    TYPE_SPECIALIZATION
};

//<---------JIT COMPILATION CONTEXT--------->
struct JITContext {
    const ASTNode* rootNode;
    ScopeManager* scopeManager;
    std::vector<OptimizationHint> optimizations;
    bool enableDebugging = false;
    size_t maxInstructions = 10000; // Limit for safety
    
    JITContext(const ASTNode* node, ScopeManager* scope) 
        : rootNode(node), scopeManager(scope) {}
};

//<---------JIT COMPILER FACTORY--------->
class JITCompilerFactory {
public:
    static std::unique_ptr<JITCompiler> createCompiler(ErrorHandler& errorHandler);
    static std::unique_ptr<JITCompiler> createOptimizedCompiler(ErrorHandler& errorHandler);
    static std::unique_ptr<JITCompiler> createDebugCompiler(ErrorHandler& errorHandler);
};

//<---------UTILITY FUNCTIONS--------->
std::string jitStatsToString(const JITStats& stats);
double measureExecutionTime(const std::function<void()>& func);
bool isJITSupported();

} // namespace Quanta

#endif // QUANTA_JIT_H
