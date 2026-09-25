#!/usr/bin/env bash
# Build .kkrieger for WebAssembly / WebGL2.
#
#   ./wasm/build.sh          incremental
#   ./wasm/build.sh clean    rebuild from scratch
#
# Requires emsdk to be activated (source ~/emsdk/emsdk_env.sh).
set -euo pipefail

cd "$(dirname "$0")/.."          # werkkzeug3_kkrieger/
ROOT=$(pwd)
OBJ=${KK_OBJDIR:-$ROOT/wasm/obj}
OUTDIR=${KK_OUTDIR:-$ROOT/wasm/dist}

# KK_RELEASE=1: optimise for size/speed, drop debug info and runtime checks.
# Objects land in a separate directory so debug and release can coexist.
if [ "${KK_RELEASE:-}" = 1 ]; then
  OBJ=${KK_OBJDIR:-$ROOT/wasm/obj_release}
  OUTDIR=${KK_OUTDIR:-$ROOT/wasm/dist_release}
  OPTFLAGS=( -O3 )
  LDDEBUG=( -O3 --closure 0 )
else
  OPTFLAGS=( -O2 -g2 )
  LDDEBUG=( -O2 -g2 -sASSERTIONS=2 -sSTACK_OVERFLOW_CHECK=2 )
fi

[ "${1:-}" = clean ] && rm -rf "$OBJ" "$OUTDIR"
mkdir -p "$OBJ" "$OUTDIR"

CXXFLAGS=(
  -std=c++14 -fpermissive
  -fno-delete-null-pointer-checks    # legacy code calls methods on null (KInstanceMem::DeleteChain)
  -fno-strict-aliasing               # MSVC-era code indexes struct members as arrays ((&v.x)[i]); with
                                     # type-based alias analysis clang miscompiles that (Mesh_Cube lost
                                     # its z tessellation, so walls/trims came out 1/tz as long)
  -fwrapv                            # fixed-point generator code assumes signed overflow wraps like x86
  "${OPTFLAGS[@]}"
  -I"$ROOT" -I"$ROOT/materials" -I"$ROOT/../v2" -I"$ROOT/wasm/mojoshader"
  -include "$ROOT/wasm/compat_wasm.hpp"
  -include "$ROOT/player_kkrieger/kkrieger_config.hpp"
  -Wno-unknown-pragmas -Wno-writable-strings -Wno-deprecated
  -Wno-invalid-offsetof -Wno-parentheses -Wno-logical-op-parentheses
  -Wno-dangling-else -Wno-tautological-compare -Wno-unused-value
  -Wno-null-conversion -Wno-non-c-typedef-for-linkage
  -Wno-everything
)
if [ "${KK_HEADLESS:-}" = 1 ]; then
  CXXFLAGS+=( -DKK_HEADLESS -fsanitize=address -fsanitize-recover=address )
  CFLAGS_EXTRA=( -fsanitize=address -fsanitize-recover=address )
else
  CXXFLAGS+=( -sUSE_SDL=2 )
  CFLAGS_EXTRA=()
fi

CFLAGS=(
  "${OPTFLAGS[@]}"
  -I"$ROOT/wasm/mojoshader" -I"$ROOT/wasm/mojoshader/profiles"
  -DSUPPORT_PROFILE_ARB1=0 -DSUPPORT_PROFILE_ARB1_NV=0 -DSUPPORT_PROFILE_METAL=0
  -DSUPPORT_PROFILE_SPIRV=0 -DSUPPORT_PROFILE_GLSPIRV=0 -DSUPPORT_PROFILE_HLSL=0
  -DMOJOSHADER_VERSION=1 -DMOJOSHADER_CHANGESET='"kkrieger-wasm"'
  -Wno-everything
)
CFLAGS+=( "${CFLAGS_EXTRA[@]}" )

# The game, minus the platform layer (_start.cpp) and the x86 synth core.
SOURCES=(
  _types.cpp
  kdoc.cpp
  engine.cpp
  genbitmap.cpp
  genblobspline.cpp
  geneffect.cpp
  genmaterial.cpp
  genmesh.cpp
  genminmesh.cpp
  genoverlay.cpp
  genscene.cpp
  kkriegergame.cpp
  shadercodegen.cpp
  _lekktor.cpp
  materials/material11.cpp
  materials/material20.cpp
  materials/materialdirect.cpp
  materials/rtmanager.cpp
  mainplayer.cpp
  wasm/kop_thunks.cpp
  wasm/_start_wasm.cpp
  wasm/v2_shim.cpp
  ../v2/synth_core.cpp
  ../v2/v2mplayer.cpp
  ../v2/v2mconv.cpp
  wasm/v2_bridge.cpp
  wasm/shader_translate.cpp
  wasm/render2004.cpp
  wasm/mojoshader/mojoshader.c
  wasm/mojoshader/mojoshader_common.c
  wasm/mojoshader/profiles/mojoshader_profile_common.c
  wasm/mojoshader/profiles/mojoshader_profile_d3d.c
  wasm/mojoshader/profiles/mojoshader_profile_bytecode.c
  wasm/mojoshader/profiles/mojoshader_profile_glsl.c
)
[ "${KK_HEADLESS:-}" = 1 ] && SOURCES+=( wasm/gl_stub.cpp )

python3 "$ROOT/wasm/gen_thunks.py"

# newest header anywhere in the tree: any object older than it is rebuilt
NEWEST_HDR=$( { ls -t "$ROOT"/*.hpp "$ROOT"/materials/*.hpp "$ROOT"/wasm/*.hpp "$ROOT"/player_kkrieger/*.hpp 2>/dev/null || true; } | head -1 )   # || true: head closing the pipe must not trip pipefail

OBJS=()
FAILED=0
for src in "${SOURCES[@]}"; do
  obj="$OBJ/$(echo "$src" | tr '/.' '__').o"
  OBJS+=("$obj")
  if [ -f "$obj" ] && [ "$obj" -nt "$ROOT/$src" ] && [ "$obj" -nt "$NEWEST_HDR" ]; then continue; fi
  # em++ would compile .c as C++ (like g++); MojoShader is C.
  case "$src" in
    *.c) printf '  CC  %s\n' "$src"; CC=emcc; FLAGS=("${CFLAGS[@]}");;
    *)   printf '  CXX %s\n' "$src"; CC=em++; FLAGS=("${CXXFLAGS[@]}");;
  esac
  if ! "$CC" -c "${FLAGS[@]}" "$ROOT/$src" -o "$obj"; then
    FAILED=1
  fi
done
[ "$FAILED" = 1 ] && { echo "compile failed"; exit 1; }

LDFLAGS=(
  "${LDDEBUG[@]}"
  -sMALLOC=emmalloc
  -sUSE_SDL=2
  -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 -sFULL_ES3=1
  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=536870912
  -sSTACK_SIZE=8388608
  -sASYNCIFY=1 -sASYNCIFY_STACK_SIZE=32768
  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,callMain
  -sENVIRONMENT=web
  --preload-file "$ROOT/data/kkrieger3383.kx@/kkrieger.kx"
  --preload-file "$ROOT/data/kkrieger_beta_conv.kx@/kkrieger_beta.kx"
  --shell-file "$ROOT/wasm/shell.html"
)

if [ "${KK_HEADLESS:-}" = 1 ]; then
  echo "  LINK kk_headless.js"
  em++ "${OBJS[@]}" -O2 -g2 -fsanitize=address -fsanitize-recover=address -sENVIRONMENT=node -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=1073741824 \
    -sSTACK_SIZE=8388608 -sASYNCIFY=1 -sASYNCIFY_STACK_SIZE=32768 -sASSERTIONS=1 -sEXIT_RUNTIME=1 \
    --preload-file "$ROOT/data/kkrieger3383.kx@/kkrieger.kx" -o "$OUTDIR/kk_headless.js"
  echo "done -> $OUTDIR/kk_headless.js"
else
  echo "  LINK kkrieger.html"
  em++ "${OBJS[@]}" "${LDFLAGS[@]}" -o "$OUTDIR/kkrieger.html"
  echo "done -> $OUTDIR/kkrieger.html"
fi
