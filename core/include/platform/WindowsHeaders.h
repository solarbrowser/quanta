/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once
// WindowsHeaders.h - Maximum performance Windows API headers
// Optimized for native Windows builds (MSVC/MinGW-w64 standalone)

#ifdef _WIN32

// Force inline for maximum performance
#ifndef FORCEINLINE
#define FORCEINLINE __forceinline
#endif

// Target Windows 7+ for modern APIs
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

// Prevent Windows API pollution
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX  
#define NOMINMAX
#endif

// Exclude ALL unnecessary Windows APIs for maximum performance
#define VC_EXTRALEAN
#define WIN32_EXTRA_LEAN
#define STRICT

// Disable problematic Windows macros
#define NOGDICAPMASKS
#define NOVIRTUALKEYCODES
// #define NOWINMESSAGES  // Disabled - needed for MSG type in shobjidl.h
#define NOWINSTYLES
#define NOSYSMETRICS
#define NOMENUS
#define NOICONS
#define NOKEYSTATES
#define NOSYSCOMMANDS
#define NORASTEROPS
#define NOSHOWWINDOW
#define OEMRESOURCE
#define NOATOM
// Note: Need clipboard, user, GDI, and NLS functions for native APIs
// #define NOCLIPBOARD  // Disabled - need clipboard functions
// #define NOUSER       // Disabled - need user functions  
// #define NOGDI        // Disabled - need GDI functions
// #define NONLS        // Disabled - need NLS (string conversion) functions
#define NOCOLOR
#define NOCTLMGR
#define NODRAWTEXT
#define NOKERNEL
#define NOMB
#define NOMEMMGR
#define NOMETAFILE
#define NOMSG
#define NOOPENFILE
#define NOSCROLL
#define NOSERVICE
#define NOSOUND
#define NOTEXTMETRIC
#define NOWH
#define NOWINOFFSETS
#define NOCOMM
#define NOKANJI
#define NOHELP
#define NOPROFILER
#define NODEFERWINDOWPOS
#define NOMCX

// Only include absolutely necessary Windows headers
#include <winsock2.h>  // Network sockets (must be before windows.h)
#include <windows.h>   // Core Windows APIs

// Restore std functions that might be hidden by Windows macros
#ifdef min
#undef min
#endif
#ifdef max  
#undef max
#endif

#endif // _WIN32