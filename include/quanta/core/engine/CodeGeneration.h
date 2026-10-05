/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_ENGINE_CODE_GENERATION_H
#define QUANTA_ENGINE_CODE_GENERATION_H

#include "quanta/core/runtime/Value.h"
#include <span>
#include <string>
#include <vector>

namespace Quanta {

class Context;
class Engine;

// What is about to compile a string.
enum class CompileKind {
    DirectEval,
    IndirectEval,
    Function,
    GeneratorFunction,
    AsyncFunction,
    AsyncGeneratorFunction,
    Timer          // setTimeout("code"): a host's own, through prepare_string_source
};

// A host that decides what may be compiled from a string (Content-Security-Policy `unsafe-eval`, Trusted
// Types). With none set on its Engine, everything may.
class CodeGenerationHost {
public:
    virtual ~CodeGenerationHost() = default;

    // HostGetCodeForEval: eval(x) where x is an object. True with the code of it (a TrustedScript's), false to
    // leave x alone, as eval does for any non-string.
    virtual bool code_for_eval(Context& ctx, const Value& object, std::string& code) = 0;

    // The host's chance to replace what will be compiled (the Trusted Types default policy). `originals` are the
    // values the caller passed (parameters first, the body last for a Function; the one argument for eval) and
    // `parts` their strings, to be replaced in place. False, with `error`, throws a TypeError.
    virtual bool transform(Context& ctx, CompileKind kind, const std::vector<Value>& originals,
                           std::vector<std::string>& parts, std::string& error) = 0;

    // HostEnsureCanCompileStrings on the final strings. False, with `error`, throws an EvalError.
    virtual bool ensure_can_compile(Context& ctx, CompileKind kind, const std::vector<std::string>& parts,
                                    std::string& error) = 0;
};

// eval's step before it compiles: `code` is what to compile when this returns true with `proceed` set, and
// `argument` itself is eval's answer when `proceed` is not. False with an exception pending.
bool prepare_eval_source(Context& ctx, Engine* engine, bool direct, const Value& argument, std::string& code,
                         bool& proceed);

// The Function constructors' step before they compile. If the Engine has a host, every argument is converted to a
// string (once: the caller must not convert again) and put through the host; `converted` then holds the strings
// as values, to be used in place of `args`. False with an exception pending.
bool prepare_function_source(Context& ctx, Engine* engine, CompileKind kind, std::span<const Value> args,
                             std::vector<Value>& converted);

// The same for one string of host code (a string timer handler).
bool prepare_string_source(Context& ctx, Engine* engine, CompileKind kind, const Value& original, std::string& code);

}

#endif
