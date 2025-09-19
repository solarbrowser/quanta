/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "UltimatePatternDetector.h"
#include <iostream>
#include <algorithm>
#include <sstream>

namespace Quanta {

// Static regex patterns for ultimate detection
const std::regex UltimatePatternDetector::OBJECT_CREATION_PATTERN(
    R"((let|const|var)\s+(\w+)\s*=\s*\{\s*\}|\w+\.\w+\s*=)"
);

const std::regex UltimatePatternDetector::PROPERTY_ACCESS_PATTERN(
    R"(\w+\.\w+|\w+\[\w+\])"
);

const std::regex UltimatePatternDetector::FUNCTION_CALL_PATTERN(
    R"(\w+\s*\([^)]*\))"
);

const std::regex UltimatePatternDetector::STRING_CONCAT_PATTERN(
    R"(\w+\s*\+\s*\w+|["`'].*["`'])"
);

const std::regex UltimatePatternDetector::VARIABLE_ASSIGN_PATTERN(
    R"((let|const|var)\s+\w+\s*=|\w+\s*=)"
);

const std::regex UltimatePatternDetector::LOOP_PATTERN(
    R"(for\s*\(.*\)|while\s*\(.*\)|\.forEach|\.map)"
);

const std::regex UltimatePatternDetector::DOM_PATTERN(
    R"(document\.|window\.|element\.|\.style\.|\.innerHTML|\.addEventListener)"
);

// Complexity weights for different operations
std::unordered_map<std::string, double> UltimatePatternDetector::complexity_weights_ = {
    {"object_creation", 3.0},
    {"property_access", 2.0},
    {"function_call", 4.0},
    {"string_operation", 2.5},
    {"variable_assignment", 1.0},
    {"loop_iteration", 5.0},
    {"dom_operation", 10.0}
};

UltimatePatternDetector::PatternInfo UltimatePatternDetector::analyze_complete_pattern(const std::string& source) {
    // Ultimate pattern analysis
    
    PatternInfo info;
    info.complexity_score = calculate_complexity_score(source);
    info.estimated_operations = estimate_operations_count(source);
    info.can_optimize = false;
    
    // Test all pattern types
    std::vector<bool> detected_patterns = {
        detect_object_creation_intensive(source, info),
        detect_property_access_intensive(source, info),
        detect_function_call_intensive(source, info),
        detect_string_manipulation_intensive(source, info),
        detect_variable_assignment_intensive(source, info),
        detect_control_flow_intensive(source, info)
    };
    
    // SMART PATTERN PRIORITIZATION - Count actual operations for best optimization
    std::vector<int> pattern_counts = {0, 0, 0, 0, 0, 0};
    
    // Count actual occurrences for priority ranking
    if (detected_patterns[0]) {
        std::sregex_iterator iter(source.begin(), source.end(), OBJECT_CREATION_PATTERN);
        pattern_counts[0] = std::distance(iter, std::sregex_iterator{});
    }
    if (detected_patterns[1]) {
        std::sregex_iterator iter(source.begin(), source.end(), PROPERTY_ACCESS_PATTERN);
        pattern_counts[1] = std::distance(iter, std::sregex_iterator{});
    }
    if (detected_patterns[2]) {
        std::sregex_iterator iter(source.begin(), source.end(), FUNCTION_CALL_PATTERN);
        pattern_counts[2] = std::distance(iter, std::sregex_iterator{});
    }
    if (detected_patterns[3]) {
        std::sregex_iterator iter(source.begin(), source.end(), STRING_CONCAT_PATTERN);
        pattern_counts[3] = std::distance(iter, std::sregex_iterator{});
    }
    if (detected_patterns[4]) {
        std::sregex_iterator iter(source.begin(), source.end(), VARIABLE_ASSIGN_PATTERN);
        pattern_counts[4] = std::distance(iter, std::sregex_iterator{});
    }
    if (detected_patterns[5]) {
        std::sregex_iterator iter(source.begin(), source.end(), LOOP_PATTERN);
        pattern_counts[5] = std::distance(iter, std::sregex_iterator{});
    }
    
    // Find the most dominant pattern by count
    int max_count = 0;
    int dominant_pattern = -1;
    for (int i = 0; i < 6; i++) {
        if (detected_patterns[i] && pattern_counts[i] > max_count) {
            max_count = pattern_counts[i];
            dominant_pattern = i;
        }
    }
    
    // Set pattern type based on most dominant
    if (dominant_pattern == 0) {
        info.type = OBJECT_INTENSIVE;
        info.description = "Object Creation and Property Manipulation";
        info.can_optimize = true;
    } else if (dominant_pattern == 1) {
        info.type = PROPERTY_INTENSIVE;
        info.description = "Property Access and Assignment";
        info.can_optimize = true;
    } else if (dominant_pattern == 2) {
        info.type = FUNCTION_INTENSIVE;
        info.description = "Function Calls and Method Invocations";
        info.can_optimize = true;
    } else if (dominant_pattern == 3) {
        info.type = STRING_INTENSIVE;
        info.description = "String Operations and Concatenation";
        info.can_optimize = true;
    } else if (dominant_pattern == 4) {
        info.type = VARIABLE_INTENSIVE;
        info.description = "Variable Declaration and Assignment";
        info.can_optimize = true;
    } else if (dominant_pattern == 5) {
        info.type = CONTROL_FLOW_INTENSIVE;
        info.description = "Control Flow and Loop Operations";
        info.can_optimize = true;
    } else {
        info.type = MIXED_INTENSIVE;
        info.description = "Mixed JavaScript Operations";
        info.can_optimize = false;
    }
    
    return info;
}

bool UltimatePatternDetector::detect_object_creation_intensive(const std::string& source, PatternInfo& info) {
    std::sregex_iterator iter(source.begin(), source.end(), OBJECT_CREATION_PATTERN);
    std::sregex_iterator end;
    
    int count = std::distance(iter, end);
    
    if (count >= 3) { // Intensive if 3+ object operations
        // Object-intensive pattern detected
        return true;
    }
    return false;
}

bool UltimatePatternDetector::detect_property_access_intensive(const std::string& source, PatternInfo& info) {
    std::sregex_iterator iter(source.begin(), source.end(), PROPERTY_ACCESS_PATTERN);
    std::sregex_iterator end;
    
    int count = std::distance(iter, end);
    
    if (count >= 5) { // Intensive if 5+ property accesses
        // Property-access-intensive pattern detected
        return true;
    }
    return false;
}

bool UltimatePatternDetector::detect_function_call_intensive(const std::string& source, PatternInfo& info) {
    std::sregex_iterator iter(source.begin(), source.end(), FUNCTION_CALL_PATTERN);
    std::sregex_iterator end;
    
    int count = std::distance(iter, end);
    
    if (count >= 4) { // Intensive if 4+ function calls
        // Function-call-intensive pattern detected
        return true;
    }
    return false;
}

bool UltimatePatternDetector::detect_string_manipulation_intensive(const std::string& source, PatternInfo& info) {
    std::sregex_iterator iter(source.begin(), source.end(), STRING_CONCAT_PATTERN);
    std::sregex_iterator end;
    
    int count = std::distance(iter, end);
    
    if (count >= 3) { // Intensive if 3+ string operations
        // String-intensive pattern detected
        return true;
    }
    return false;
}

bool UltimatePatternDetector::detect_variable_assignment_intensive(const std::string& source, PatternInfo& info) {
    std::sregex_iterator iter(source.begin(), source.end(), VARIABLE_ASSIGN_PATTERN);
    std::sregex_iterator end;
    
    int count = std::distance(iter, end);
    
    if (count >= 5) { // Intensive if 5+ variable assignments
        // Variable-intensive pattern detected
        return true;
    }
    return false;
}

bool UltimatePatternDetector::detect_control_flow_intensive(const std::string& source, PatternInfo& info) {
    std::sregex_iterator iter(source.begin(), source.end(), LOOP_PATTERN);
    std::sregex_iterator end;
    
    int count = std::distance(iter, end);
    
    if (count >= 2) { // Intensive if 2+ loops/iterations
        // Control-flow-intensive pattern detected
        return true;
    }
    return false;
}

double UltimatePatternDetector::calculate_complexity_score(const std::string& source) {
    double score = 0.0;
    
    // Count each type of operation and apply weights
    for (const auto& weight : complexity_weights_) {
        std::string pattern_name = weight.first;
        double weight_value = weight.second;
        
        // Simple heuristic: count occurrences of key patterns
        size_t count = 0;
        if (pattern_name == "object_creation") {
            std::sregex_iterator iter(source.begin(), source.end(), OBJECT_CREATION_PATTERN);
            count = std::distance(iter, std::sregex_iterator{});
        } else if (pattern_name == "property_access") {
            std::sregex_iterator iter(source.begin(), source.end(), PROPERTY_ACCESS_PATTERN);
            count = std::distance(iter, std::sregex_iterator{});
        } else if (pattern_name == "function_call") {
            std::sregex_iterator iter(source.begin(), source.end(), FUNCTION_CALL_PATTERN);
            count = std::distance(iter, std::sregex_iterator{});
        }
        
        score += count * weight_value;
    }
    
    return score;
}

int UltimatePatternDetector::estimate_operations_count(const std::string& source) {
    // Rough estimate based on lines and complexity
    int lines = std::count(source.begin(), source.end(), '\n') + 1;
    double complexity = calculate_complexity_score(source);
    
    return static_cast<int>(lines * complexity * 1.5);
}

bool UltimatePatternDetector::can_achieve_150m_ops_per_sec(const PatternInfo& info) {
    return info.can_optimize && info.complexity_score < 50.0;
}

void UltimatePatternDetector::print_pattern_analysis(const PatternInfo& info) {
    // Pattern analysis results
    // Analysis complete
}

void UltimatePatternDetector::print_optimization_roadmap(const PatternInfo& info) {
    // Optimization roadmap generated silently
}

} // namespace Quanta