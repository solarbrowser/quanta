/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_TYPES_H
#define QUANTA_TYPES_H

#include <cstdint>

namespace Quanta {

class Value;
class Object;
class Function;
class String;
class Context;
class Engine;

enum PropertyAttributes : uint8_t {
    None = 0,
    Writable = 1 << 0,
    Enumerable = 1 << 1,
    Configurable = 1 << 2,
    Default = Writable | Enumerable | Configurable
};

}

#endif
