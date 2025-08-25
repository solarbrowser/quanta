#include "../include/BranchPredictionOptimizer.h"
#include <chrono>
#include <algorithm>
#include <cmath>

namespace Quanta {

BranchPredictionOptimizer::BranchPredictionOptimizer(OptimizedAST* ast, SpecializedNodeProcessor* processor)
    : ast_context_(ast), specialized_processor_(processor), global_history_register_(0),
      total_branches_predicted_(0), total_correct_predictions_(0), total_mispredictions_(0),
      total_prediction_time_saved_(0) {
    
    branch_history_.reserve(1000);
    optimal_strategies_.reserve(1000);
    pattern_history_table_.fill(1); // Initialize to weakly taken
}

void BranchPredictionOptimizer::analyze_branch_patterns(uint32_t branch_node_id) {
    auto it = branch_history_.find(branch_node_id);
    if (it != branch_history_.end()) {
        return; // Already analyzed
    }
    
    // Initialize branch history entry
    BranchHistoryEntry& entry = branch_history_[branch_node_id];
    entry.branch_id = branch_node_id;
    entry.total_executions = 0;
    entry.taken_count = 0;
    entry.not_taken_count = 0;
    entry.recent_outcomes.fill(false);
    entry.history_index = 0;
    entry.correct_predictions = 0;
    entry.total_predictions = 0;
    entry.accuracy_rate = 0.5; // Start with 50% accuracy
    entry.current_strategy = PredictionStrategy::BIMODAL;
    entry.bimodal_state = 1; // Weakly not taken
    entry.global_history = 0;
    entry.misprediction_penalty = 0;
    entry.time_saved_by_prediction = 0;
    
    // Analyze branch type
    BranchType type = classify_branch_type(branch_node_id);
    
    // Set initial prediction strategy based on branch type
    switch (type) {
        case BranchType::LOOP_CONDITION:
            entry.current_strategy = PredictionStrategy::ALWAYS_TAKEN; // Loops usually iterate
            entry.bimodal_state = 3; // Strongly taken
            break;
        case BranchType::CONDITIONAL_IF:
            entry.current_strategy = PredictionStrategy::BIMODAL;
            break;
        case BranchType::LOGICAL_AND:
            entry.current_strategy = PredictionStrategy::NEVER_TAKEN; // Short-circuit common
            entry.bimodal_state = 0; // Strongly not taken
            break;
        case BranchType::LOGICAL_OR:
            entry.current_strategy = PredictionStrategy::ALWAYS_TAKEN; // Often evaluates true
            entry.bimodal_state = 3; // Strongly taken
            break;
        default:
            entry.current_strategy = PredictionStrategy::ADAPTIVE;
            break;
    }
}

bool BranchPredictionOptimizer::predict_branch_outcome(uint32_t branch_id, const Value& condition) {
    auto it = branch_history_.find(branch_id);
    if (it == branch_history_.end()) {
        analyze_branch_patterns(branch_id);
        it = branch_history_.find(branch_id);
    }
    
    BranchHistoryEntry& entry = it->second;
    bool prediction = false;
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    switch (entry.current_strategy) {
        case PredictionStrategy::ALWAYS_TAKEN:
            prediction = true;
            break;
        case PredictionStrategy::NEVER_TAKEN:
            prediction = false;
            break;
        case PredictionStrategy::BIMODAL:
            prediction = bimodal_predict(entry);
            break;
        case PredictionStrategy::GSHARE:
            prediction = gshare_predict(entry, branch_id);
            break;
        case PredictionStrategy::PERCEPTRON:
            prediction = perceptron_predict(entry, branch_id);
            break;
        case PredictionStrategy::ADAPTIVE:
            // Use the strategy with best historical accuracy
            if (entry.accuracy_rate > 0.85) {
                prediction = bimodal_predict(entry);
            } else {
                prediction = gshare_predict(entry, branch_id);
            }
            break;
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time);
    
    // Estimate time saved by avoiding speculative execution
    total_prediction_time_saved_ += duration.count() * 10; // Estimate 10x speedup
    entry.total_predictions++;
    total_branches_predicted_++;
    
    return prediction;
}

void BranchPredictionOptimizer::update_branch_prediction(uint32_t branch_id, bool actual_outcome) {
    auto it = branch_history_.find(branch_id);
    if (it == branch_history_.end()) {
        return;
    }
    
    BranchHistoryEntry& entry = it->second;
    
    // Update execution statistics
    entry.total_executions++;
    if (actual_outcome) {
        entry.taken_count++;
    } else {
        entry.not_taken_count++;
    }
    
    // Update recent outcomes history
    entry.recent_outcomes[entry.history_index] = actual_outcome;
    entry.history_index = (entry.history_index + 1) % 64;
    
    // Check prediction accuracy
    bool prediction = predict_branch_outcome(branch_id, Value(actual_outcome));
    if (prediction == actual_outcome) {
        entry.correct_predictions++;
        total_correct_predictions_++;
    } else {
        total_mispredictions_++;
        entry.misprediction_penalty += 100; // Estimate 100ns penalty
    }
    
    // Update accuracy rate
    entry.accuracy_rate = static_cast<double>(entry.correct_predictions) / entry.total_predictions;
    
    // Update predictor state based on actual outcome
    switch (entry.current_strategy) {
        case PredictionStrategy::BIMODAL:
            if (actual_outcome && entry.bimodal_state < 3) {
                entry.bimodal_state++;
            } else if (!actual_outcome && entry.bimodal_state > 0) {
                entry.bimodal_state--;
            }
            break;
        case PredictionStrategy::GSHARE: {
            // Update pattern history table
            uint32_t index = (branch_id ^ entry.global_history) % pattern_history_table_.size();
            if (actual_outcome && pattern_history_table_[index] < 3) {
                pattern_history_table_[index]++;
            } else if (!actual_outcome && pattern_history_table_[index] > 0) {
                pattern_history_table_[index]--;
            }
            
            // Update global history
            entry.global_history = ((entry.global_history << 1) | (actual_outcome ? 1 : 0)) & 0xFFF;
            global_history_register_ = ((global_history_register_ << 1) | (actual_outcome ? 1 : 0)) & 0xFFF;
            break;
        }
        default:
            break;
    }
    
    // Adapt strategy if accuracy is poor
    if (entry.total_predictions > 100 && entry.accuracy_rate < 0.6) {
        entry.current_strategy = determine_optimal_strategy(branch_id);
    }
}

bool BranchPredictionOptimizer::bimodal_predict(BranchHistoryEntry& entry) {
    return entry.bimodal_state >= 2; // Predict taken if counter >= 2
}

bool BranchPredictionOptimizer::gshare_predict(BranchHistoryEntry& entry, uint32_t pc) {
    uint32_t index = (pc ^ entry.global_history) % pattern_history_table_.size();
    return pattern_history_table_[index] >= 2;
}

bool BranchPredictionOptimizer::perceptron_predict(BranchHistoryEntry& entry, uint32_t pc) {
    // Simplified perceptron prediction
    int32_t sum = 0;
    
    // Use recent outcomes as features
    for (int i = 0; i < 16; ++i) {
        bool bit = entry.recent_outcomes[(entry.history_index - i - 1 + 64) % 64];
        sum += bit ? 1 : -1;
    }
    
    return sum >= 0;
}

Value BranchPredictionOptimizer::execute_optimized_conditional(uint32_t branch_id, const Value& condition,
                                                              uint32_t true_branch, uint32_t false_branch, 
                                                              Context& ctx) {
    bool actual_outcome = condition.to_boolean();
    bool predicted_outcome = predict_branch_outcome(branch_id, condition);
    
    Value result;
    
    if (predicted_outcome) {
        // Predict taken - execute true branch first
        if (actual_outcome) {
            result = ast_context_->evaluate_fast(true_branch, ctx);
        } else {
            // Misprediction - execute false branch
            result = ast_context_->evaluate_fast(false_branch, ctx);
        }
    } else {
        // Predict not taken - execute false branch first
        if (!actual_outcome) {
            result = ast_context_->evaluate_fast(false_branch, ctx);
        } else {
            // Misprediction - execute true branch
            result = ast_context_->evaluate_fast(true_branch, ctx);
        }
    }
    
    // Update prediction accuracy
    update_branch_prediction(branch_id, actual_outcome);
    
    return result;
}

BranchType BranchPredictionOptimizer::classify_branch_type(uint32_t node_id) {
    // Simplified classification based on node ID patterns
    // In real implementation, would analyze AST structure
    
    if (node_id % 10 == 0) return BranchType::LOOP_CONDITION;
    if (node_id % 7 == 0) return BranchType::LOGICAL_AND;
    if (node_id % 5 == 0) return BranchType::LOGICAL_OR;
    if (node_id % 3 == 0) return BranchType::TERNARY_OPERATOR;
    
    return BranchType::CONDITIONAL_IF;
}

PredictionStrategy BranchPredictionOptimizer::determine_optimal_strategy(uint32_t branch_id) {
    auto it = branch_history_.find(branch_id);
    if (it == branch_history_.end()) {
        return PredictionStrategy::BIMODAL;
    }
    
    const BranchHistoryEntry& entry = it->second;
    
    // Calculate taken probability
    if (entry.total_executions == 0) {
        return PredictionStrategy::BIMODAL;
    }
    
    double taken_probability = static_cast<double>(entry.taken_count) / entry.total_executions;
    
    // Choose strategy based on patterns
    if (taken_probability > 0.95) {
        return PredictionStrategy::ALWAYS_TAKEN;
    } else if (taken_probability < 0.05) {
        return PredictionStrategy::NEVER_TAKEN;
    } else if (taken_probability > 0.8 || taken_probability < 0.2) {
        return PredictionStrategy::BIMODAL; // Strongly biased
    } else {
        return PredictionStrategy::GSHARE; // More complex patterns
    }
}

double BranchPredictionOptimizer::get_overall_prediction_accuracy() const {
    if (total_branches_predicted_ == 0) return 0.0;
    
    return static_cast<double>(total_correct_predictions_) / total_branches_predicted_;
}

void BranchPredictionOptimizer::clear_branch_history() {
    branch_history_.clear();
    optimal_strategies_.clear();
    pattern_history_table_.fill(1);
    global_history_register_ = 0;
    
    total_branches_predicted_ = 0;
    total_correct_predictions_ = 0;
    total_mispredictions_ = 0;
    total_prediction_time_saved_ = 0;
}

size_t BranchPredictionOptimizer::get_memory_usage() const {
    return branch_history_.size() * sizeof(BranchHistoryEntry) +
           optimal_strategies_.size() * sizeof(std::pair<uint32_t, PredictionStrategy>) +
           pattern_history_table_.size() * sizeof(uint8_t);
}

// BimodalPredictor implementation
BimodalPredictor::BimodalPredictor(size_t table_size) 
    : counters_(table_size, 1) { // Initialize to weakly not taken
}

bool BimodalPredictor::predict(uint32_t pc) {
    uint32_t index = pc % counters_.size();
    return counters_[index] >= 2;
}

void BimodalPredictor::update(uint32_t pc, bool taken) {
    uint32_t index = pc % counters_.size();
    
    if (taken && counters_[index] < 3) {
        counters_[index]++;
    } else if (!taken && counters_[index] > 0) {
        counters_[index]--;
    }
}

// GSharePredictor implementation
GSharePredictor::GSharePredictor(size_t table_size, uint32_t history_bits)
    : pattern_table_(table_size, 1), global_history_(0), history_bits_(history_bits) {
}

bool GSharePredictor::predict(uint32_t pc) {
    uint32_t index = hash_function(pc);
    return pattern_table_[index] >= 2;
}

void GSharePredictor::update(uint32_t pc, bool taken) {
    uint32_t index = hash_function(pc);
    
    if (taken && pattern_table_[index] < 3) {
        pattern_table_[index]++;
    } else if (!taken && pattern_table_[index] > 0) {
        pattern_table_[index]--;
    }
    
    shift_global_history(taken);
}

uint32_t GSharePredictor::hash_function(uint32_t pc) const {
    uint32_t mask = (1 << history_bits_) - 1;
    return (pc ^ (global_history_ & mask)) % pattern_table_.size();
}

void GSharePredictor::shift_global_history(bool taken) {
    uint32_t mask = (1 << history_bits_) - 1;
    global_history_ = ((global_history_ << 1) | (taken ? 1 : 0)) & mask;
}

// PerceptronPredictor implementation
PerceptronPredictor::PerceptronPredictor(size_t table_size, uint32_t history_length)
    : perceptrons_(table_size), global_history_(0), history_length_(history_length), threshold_(50) {
    
    for (auto& perceptron : perceptrons_) {
        perceptron.weights.resize(history_length_ + 1, 0); // +1 for bias
        perceptron.bias = 0;
        perceptron.training_count = 0;
    }
}

bool PerceptronPredictor::predict(uint32_t pc) {
    uint32_t index = pc % perceptrons_.size();
    const Perceptron& p = perceptrons_[index];
    
    int32_t output = compute_output(p);
    return output >= 0;
}

void PerceptronPredictor::update(uint32_t pc, bool taken) {
    uint32_t index = pc % perceptrons_.size();
    Perceptron& p = perceptrons_[index];
    
    int32_t output = compute_output(p);
    
    // Train if prediction was wrong or output magnitude is below threshold
    if ((output >= 0) != taken || std::abs(output) <= threshold_) {
        train_perceptron(p, taken, output);
    }
    
    // Update global history
    uint32_t mask = (1 << history_length_) - 1;
    global_history_ = ((global_history_ << 1) | (taken ? 1 : 0)) & mask;
}

int32_t PerceptronPredictor::compute_output(const Perceptron& p) const {
    int32_t output = p.bias;
    
    for (uint32_t i = 0; i < history_length_; ++i) {
        bool bit = (global_history_ >> i) & 1;
        output += p.weights[i] * (bit ? 1 : -1);
    }
    
    return output;
}

void PerceptronPredictor::train_perceptron(Perceptron& p, bool taken, int32_t output) {
    int32_t target = taken ? 1 : -1;
    
    // Update bias
    if (target != (output >= 0 ? 1 : -1)) {
        p.bias += target;
    }
    
    // Update weights
    for (uint32_t i = 0; i < history_length_; ++i) {
        bool bit = (global_history_ >> i) & 1;
        int32_t input = bit ? 1 : -1;
        
        if (target != (output >= 0 ? 1 : -1)) {
            p.weights[i] += target * input;
        }
    }
    
    p.training_count++;
}

} // namespace Quanta