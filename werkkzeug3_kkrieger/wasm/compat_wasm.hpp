// .kkrieger WebAssembly port -- MSVC/Win32 compatibility shim.
// Force-included (-include) ahead of everything else, before kkrieger_config.hpp.
// This file is distributed under a BSD license. See LICENSE.txt for details.

#ifndef __COMPAT_WASM_HPP__
#define __COMPAT_WASM_HPP__

#if !defined(__EMSCRIPTEN__) && !defined(__wasm__)
#error "compat_wasm.hpp is for the WebAssembly build only"
#endif

/****************************************************************************/
/***                                                                      ***/
/***   Calling conventions / MSVC keywords                                ***/
/***                                                                      ***/
/****************************************************************************/

// wasm has a single calling convention.
#define __stdcall
#define _stdcall
#define __cdecl
#define __fastcall
#define __declspec(x)
#define __w64
#define __int64 long long
#define APIENTRY
#define WINAPI
#define CALLBACK

#define __forceinline inline __attribute__((always_inline))

// MSVC-only pragmas that clang warns about are silenced on the command line
// (-Wno-unknown-pragmas); "#pragma lekktor(...)" is inert because
// sUSE_LEKKTOR == 0.

/****************************************************************************/
/***                                                                      ***/
/***   CRT / platform bits the original code expects from windows.h       ***/
/***                                                                      ***/
/****************************************************************************/

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdarg.h>

// The original sources say "0x0000ffffh"-style MSVC hex nowhere in C++ code,
// but they do use _stricmp/_strnicmp and friends.
#define _stricmp   strcasecmp
#define _strnicmp  strncasecmp
#define stricmp    strcasecmp
#define strnicmp   strncasecmp

/****************************************************************************/
/***                                                                      ***/
/***   Project configuration                                              ***/
/***                                                                      ***/
/****************************************************************************/

// Mirrors WIN32/NDEBUG/KKRIEGER from player_kkrieger.vcxproj. WIN32 is *not*
// defined: it gates windows.h usage all over the place.
#ifndef KKRIEGER
#define KKRIEGER 1
#endif
#ifndef NDEBUG
#define NDEBUG 1
#endif

#endif // __COMPAT_WASM_HPP__

// 1 = per-operator generation logs and heap validation (very slow, very noisy)
#ifndef KK_VERBOSE
#define KK_VERBOSE 0
#endif
