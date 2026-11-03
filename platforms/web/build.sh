#!/bin/bash
# tab5web 构建 — Emscripten WASM 多线程版
# 用法: platforms/web/build.sh [build-dir]   (默认 build/web)
# 前置: source ~/emsdk/emsdk_env.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${1:-$ROOT/build/web}"

source ~/emsdk/emsdk_env.sh >/dev/null 2>&1 || true
command -v emcc >/dev/null || { echo "emcc not found — source ~/emsdk/emsdk_env.sh first"; exit 1; }

mkdir -p "$BUILD"
cd "$BUILD"
emcmake cmake -DTAB5_PLATFORM=web -DCMAKE_BUILD_TYPE=Release "$ROOT"
emmake make -j"$(sysctl -n hw.ncpu)" tab5web

# 组装 bin/: 外壳 + 运行时 (index.html 注入构建版本号 → wasm 缓存键)
BIN="$BUILD/bin"
cp "$ROOT/platforms/web/shell/index.html" "$ROOT/platforms/web/shell/shell.js" "$BIN/"
BUILD_TS=$(date +%Y%m%d%H%M%S)
sed -i '' "s|<script src=\"shell.js\"></script>|<script>window.TAB5_BUILD=\"$BUILD_TS\";</script>\n<script src=\"shell.js\"></script>|" "$BIN/index.html"
grep -q "TAB5_BUILD" "$BIN/index.html" || { echo "version inject failed"; exit 1; }
echo "---- build ok: $BIN/ ($(du -sh "$BIN" | cut -f1)) ----"
ls -la "$BIN"
