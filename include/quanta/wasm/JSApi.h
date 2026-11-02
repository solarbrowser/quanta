/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_WASM_JSAPI_H
#define QUANTA_WASM_JSAPI_H

namespace Quanta {
class Context;
namespace Wasm {
// Makes the WebAssembly namespace of the realm of `ctx`.
void register_webassembly_builtins(Context& ctx);
}
}

#endif
