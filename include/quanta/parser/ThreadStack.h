/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_PARSER_THREADSTACK_H
#define QUANTA_PARSER_THREADSTACK_H

#include <cstddef>

namespace Quanta {

// How much stack this thread was actually given, in bytes. The answer comes
// from the platform where it can be had and from that platform's usual
// default where it cannot -- the two defaults differ by a factor of eight,
// so there is no shared number to fall back to.
//
// Its own translation unit on purpose: the Windows form of the question needs
// <windows.h>, which defines CONST, DELETE, IN, VOID and interface as macros.
// A parser is made of identifiers like those.
size_t thread_stack_bytes();

}  // namespace Quanta

#endif
