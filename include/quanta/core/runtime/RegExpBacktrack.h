/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_REGEXP_BACKTRACK_H
#define QUANTA_REGEXP_BACKTRACK_H

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Quanta {

struct BacktrackMatch {
    bool matched = false;
    size_t start = 0, end = 0; // UTF-16 unit offsets
    std::vector<std::pair<int64_t, int64_t>> captures; // 1-based groups; (-1,-1) = unset
};

// Backtracking matcher used as a fallback where PCRE2's lookbehind diverges
// from spec (see pattern_needs_backtrack_engine). Unsupported syntax throws
// from the constructor, signaling the caller to fall back to PCRE2 as-is.
class RegexBacktrackEngine {
public:
    RegexBacktrackEngine(const std::string& pattern_wtf8, bool ignore_case, bool multiline,
                          bool dot_all, bool unicode);
    ~RegexBacktrackEngine();
    RegexBacktrackEngine(const RegexBacktrackEngine&) = delete;
    RegexBacktrackEngine& operator=(const RegexBacktrackEngine&) = delete;

    uint32_t capture_count() const;

    // start_at/sticky are UTF-16 unit offsets.
    bool exec(const std::u16string& subject, size_t start_at, bool sticky, BacktrackMatch& out) const;

    static bool pattern_needs_backtrack_engine(const std::string& pattern);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace Quanta

#endif
