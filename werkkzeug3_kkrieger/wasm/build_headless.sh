#!/usr/bin/env bash
# Headless (node) build of the game logic with AddressSanitizer: GL is stubbed,
# no SDL, frames are driven from InitX. Used to debug generation / game code
# without a browser.  ./wasm/build_headless.sh && node wasm/dist_headless/kk_headless.js
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)
export KK_OBJDIR="$ROOT/wasm/obj_headless"
export KK_OUTDIR="$ROOT/wasm/dist_headless"
export KK_HEADLESS=1
exec bash "$ROOT/wasm/build.sh" "$@"
