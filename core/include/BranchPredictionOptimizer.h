#ifndef QUANTA_BRANCH_PREDICTION_OPTIMIZER_H
#define QUANTA_BRANCH_PREDICTION_OPTIMIZER_H

#include "OptimizedAST.h"
#include "SpecializedNodes.h"
#include "Value.h"
#include "Context.h"
#include <vector>
#include <unordered_map>
#include <array>
#include <cstdint>

namespace Quanta {

// Branch prediction types
enum class BranchType : uint8_t {
    CONDITIONAL_IF,      // if/else statements
    LOOP_CONDITION,      // while/for loop conditions
    TERNARY_OPERATOR,    // condition ? true : false
    LOGICAL_AND,         // && short-circuit
    LOGICAL_OR,          // || short-circuit
    SWITCH_CASE,         // switch statement cases
    TRY_CATCH,           // exception handling branches
    UNKNOWN
};

// Branch prediction strategy
enum class PredictionStrategy : uint8_t {
    ALWAYS_TAKEN,        // Branch always taken
    NEVER_TAKEN,         // Branch never taken
    BIMODAL,             // Two-bit saturating counter
    GSHARE,              // Global history with XOR
    PERCEPTRON,          // Perceptron-based prediction
    ADAPTIVE             // Dynamically choose best strategy
};

// Branch history entry
struct BranchHistoryEntry {
    uint32_t branch_id;
    uint64_t total_executions;
    uint64_t taken_count;
    uint64_t not_taken_count;
    
    // Pattern history
    std::array<bool, 64> recent_outcomes;
    uint8_t history_index;
    
    // Prediction accuracy
    uint64_t correct_predictions;
    uint64_t total_predictions;
    double accuracy_rate;
    
    // Current prediction state
    PredictionStrategy current_strategy;
    uint8_t bimodal_state;        // 2-bit counter for bimodal predictor
    uint32_t global_history;      // Global branch history
    
    // Performance impact
    uint64_t misprediction_penalty;
    uint64_t time_saved_by_prediction;
};

class BranchPredictionOptimizer {
private:
    OptimizedAST* ast_context_;
    SpecializedNodeProcessor* specialized_processor_;
    
    // Branch prediction tables
    std::unordered_map<uint32_t, BranchHistoryEntry> branch_history_;
    std::unordered_map<uint32_t, PredictionStrategy> optimal_strategies_;
    
    // Global prediction state
    uint32_t global_history_register_;
    std::array<uint8_t, 4096> pattern_history_table_;
    
    // Performance counters
    uint64_t total_branches_predicted_;
    uint64_t total_correct_predictions_;
    uint64_t total_mispredictions_;
    uint64_t total_prediction_time_saved_;
    
public:
    BranchPredictionOptimizer(OptimizedAST* ast, SpecializedNodeProcessor* processor);
    
    // Branch analysis and optimization
    void analyze_branch_patterns(uint32_t branch_node_id);
    bool should_optimize_branch(uint32_t branch_node_id);
    void optimize_conditional_branch(uint32_t branch_node_id);
    
    // Prediction methods
    bool predict_branch_outcome(uint32_t branch_id, const Value& condition);
    void update_branch_prediction(uint32_t branch_id, bool actual_outcome);
    PredictionStrategy determine_optimal_strategy(uint32_t branch_id);
    
    // Specific prediction algorithms
    bool bimodal_predict(BranchHistoryEntry& entry);
    bool gshare_predict(BranchHistoryEntry& entry, uint32_t pc);
    bool perceptron_predict(BranchHistoryEntry& entry, uint32_t pc);
    
    // Branch execution optimization
    Value execute_optimized_conditional(uint32_t branch_id, const Value& condition,
                                       uint32_t true_branch, uint32_t false_branch, Context& ctx);
    Value execute_optimized_loop(uint32_t loop_id, Context& ctx);
    
    // Pattern recognition
    BranchType classify_branch_type(uint32_t node_id);
    void detect_branch_correlations();
    void identify_hot_branches();
    
    // Code reordering for better prediction
    void reorder_basic_blocks_for_prediction(uint32_t function_id);
    void move_likely_branches_first(std::vector<uint32_t>& branch_sequence);
    
    // Performance monitoring
    double get_overall_prediction_accuracy() const;
    uint64_t get_total_time_saved() const { return total_prediction_time_saved_; }
    void print_branch_prediction_stats() const;
    
    // Memory management
    void clear_branch_history();
    size_t get_memory_usage() const;
};

// Specific branch predictors
class BimodalPredictor {
private:
    std::vector<uint8_t> counters_; // 2-bit saturating counters
    
public:
    BimodalPredictor(size_t table_size = 4096);
    
    bool predict(uint32_t pc);
    void update(uint32_t pc, bool taken);
    double get_accuracy() const;
    void reset();
};

class GSharePredictor {
private:
    std::vector<uint8_t> pattern_table_;
    uint32_t global_history_;
    uint32_t history_bits_;
    
public:
    GSharePredictor(size_t table_size = 4096, uint32_t history_bits = 12);
    
    bool predict(uint32_t pc);
    void update(uint32_t pc, bool taken);
    uint32_t hash_function(uint32_t pc) const;
    void shift_global_history(bool taken);
};

class PerceptronPredictor {
private:
    struct Perceptron {
        std::vector<int8_t> weights;
        int16_t bias;
        uint32_t training_count;
    };
    
    std::vector<Perceptron> perceptrons_;
    uint32_t global_history_;
    uint32_t history_length_;
    int16_t threshold_;
    
public:
    PerceptronPredictor(size_t table_size = 1024, uint32_t history_length = 32);
    
    bool predict(uint32_t pc);
    void update(uint32_t pc, bool taken);
    int32_t compute_output(const Perceptron& p) const;
    void train_perceptron(Perceptron& p, bool taken, int32_t output);
};

// Branch optimization strategies
class ConditionalBranchOptimizer {
public:
    static uint32_t optimize_if_statement(uint32_t condition_node, uint32_t true_branch,
                                         uint32_t false_branch, BranchHistoryEntry& history);
    static void apply_likely_unlikely_hints(uint32_t branch_id, double taken_probability);
    static void eliminate_redundant_conditions(uint32_t branch_sequence);
};

class LoopBranchOptimizer {
public:
    static void optimize_loop_exit_conditions(uint32_t loop_id);
    static void predict_loop_iteration_counts(uint32_t loop_id, BranchHistoryEntry& history);
    static bool can_eliminate_loop_condition_check(uint32_t loop_id);
};

// Runtime branch profiling
class RuntimeBranchProfiler {
private:
    struct BranchProfile {
        uint64_t execution_count;
        uint64_t taken_count;
        double taken_probability;
        uint64_t last_update_time;
        std::vector<bool> recent_outcomes;
    };
    
    std::unordered_map<uint32_t, BranchProfile> profiles_;
    uint64_t profiling_start_time_;
    
public:
    RuntimeBranchProfiler();
    
    void profile_branch_execution(uint32_t branch_id, bool taken);
    double get_taken_probability(uint32_t branch_id) const;
    std::vector<uint32_t> get_most_predictable_branches() const;
    std::vector<uint32_t> get_least_predictable_branches() const;
    
    void start_profiling();
    void stop_profiling();
    void reset_profiles();
    void save_profile_data(const std::string& filename) const;
    void load_profile_data(const std::string& filename);
};

// Adaptive branch prediction
class AdaptiveBranchPredictor {
private:
    BimodalPredictor bimodal_;
    GSharePredictor gshare_;
    PerceptronPredictor perceptron_;
    
    std::unordered_map<uint32_t, PredictionStrategy> predictor_selection_;
    std::unordered_map<uint32_t, std::array<uint64_t, 3>> predictor_accuracy_;
    
public:
    AdaptiveBranchPredictor();
    
    bool predict(uint32_t pc);
    void update(uint32_t pc, bool taken);
    PredictionStrategy select_best_predictor(uint32_t pc);
    void update_predictor_accuracy(uint32_t pc, PredictionStrategy strategy, bool correct);
    
    void print_predictor_statistics() const;
    void reset_all_predictors();
};

// Branch target buffer for indirect branches
class BranchTargetBuffer {
private:
    struct BTBEntry {
        uint32_t source_pc;
        uint32_t target_pc;
        bool valid;
        uint64_t last_access_time;
        uint32_t access_count;
    };
    
    std::vector<BTBEntry> buffer_;
    size_t buffer_size_;
    uint32_t replacement_policy_;
    
public:
    BranchTargetBuffer(size_t size = 512);
    
    uint32_t lookup_target(uint32_t source_pc);
    void update_target(uint32_t source_pc, uint32_t target_pc);
    void invalidate_entry(uint32_t source_pc);
    
    double get_hit_rate() const;
    void clear_buffer();
};

} // namespace Quanta

#endif // QUANTA_BRANCH_PREDICTION_OPTIMIZER_H