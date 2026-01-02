/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#ifdef _WIN32

#ifndef FORCEINLINE
#define FORCEINLINE __forceinline
#endif

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX  
#define NOMINMAX
#endif

#define VC_EXTRALEAN
#define WIN32_EXTRA_LEAN
#define STRICT

#define OEMRESOURCE
#define NOATOM
#define NOKERNEL
#define NOMEMMGR
#define NOMETAFILE
#define NOOPENFILE
#define NOSERVICE
#define NOSOUND
#define NOWH
#define NOWINOFFSETS
#define NOCOMM
#define NOKANJI
#define NOHELP
#define NOPROFILER
#define NODEFERWINDOWPOS
#define NOMCX

#include <winsock2.h>
#include <windows.h>

#ifdef min
#undef min
#endif
#ifdef max  
#undef max
#endif

#endif
