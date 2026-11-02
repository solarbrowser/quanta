/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

// Reads the *.wast.js files of the WPT WebAssembly suite and runs every module in them through Decode and Validate, with what each
// should come to: module(...) valid, assert_malformed(...) and assert_invalid(...) not.
//
//   wasm-scan [-v] file.wast.js...

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "quanta/wasm/Decoder.h"

namespace {

// The JavaScript string literal that starts at `at` (the quote), as the bytes it stands for.
bool ReadLiteral(const std::string& text, size_t at, std::string& out, size_t& end) {
    if (at >= text.size() || (text[at] != '"' && text[at] != '\'')) return false;
    const char quote = text[at++];
    out.clear();
    while (at < text.size() && text[at] != quote) {
        char c = text[at++];
        if (c != '\\') { out.push_back(c); continue; }
        if (at >= text.size()) return false;
        c = text[at++];
        switch (c) {
            case 'x': {
                const std::string hex = text.substr(at, 2);
                out.push_back(static_cast<char>(std::stoi(hex, nullptr, 16)));
                at += 2;
                break;
            }
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case '0': out.push_back('\0'); break;
            default: out.push_back(c); break;
        }
    }
    end = at + 1;
    return at < text.size();
}

}  // namespace

int main(int argc, char** argv) {
    bool verbose = false;
    int wrong = 0, total = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string path = argv[i];
        if (path == "-v") { verbose = true; continue; }
        std::ifstream in(path, std::ios::binary);
        std::stringstream buffer;
        buffer << in.rdbuf();
        const std::string text = buffer.str();
        int fileWrong = 0, fileTotal = 0;
        struct Form { const char* open; bool expectValid; };
        const Form forms[] = {{"module(", true}, {"assert_malformed(", false}, {"assert_invalid(", false}, {"assert_malformed_custom(", true}, {"assert_invalid_custom(", true}};
        for (const Form& form : forms) {
            const std::string open = form.open;
            size_t at = 0;
            while ((at = text.find(open, at)) != std::string::npos) {
                // (Only at the start of a line or after `= `: `module(` is also the end of other names.)
                const bool starts = at == 0 || text[at - 1] == ' ' || text[at - 1] == '\n' || text[at - 1] == '(' || text[at - 1] == ',';
                const size_t lit = at + open.size();
                at = lit;
                if (!starts) continue;
                std::string bytes;
                size_t end = 0;
                if (!ReadLiteral(text, lit, bytes, end)) continue;
                Quanta::Wasm::Module module;
                std::string error;
                bool ok = Quanta::Wasm::Decode(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), module, error);
                if (ok) ok = Quanta::Wasm::Validate(module, error);
                ++fileTotal;
                if (ok != form.expectValid) {
                    ++fileWrong;
                    if (verbose) {
                        const size_t line = 1 + static_cast<size_t>(std::count(text.begin(), text.begin() + static_cast<long>(lit), '\n'));
                        std::printf("  %s:%zu: %s but %s%s%s\n", path.c_str(), line, form.expectValid ? "valid" : "not valid", ok ? "it is" : "it is not: ", ok ? "" : error.c_str(), "");
                    }
                }
            }
        }
        total += fileTotal;
        wrong += fileWrong;
        std::printf("%-60s %5d modules, %d wrong\n", path.c_str(), fileTotal, fileWrong);
    }
    std::printf("total: %d modules, %d wrong\n", total, wrong);
    return wrong ? 1 : 0;
}
