#!/bin/bash
# tab5web 构建 — Emscripten WASM 多线程版
# 用法: platforms/web/build.sh [build-dir]   (默认 build/web)
# 前置: source ~/emsdk/emsdk_env.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${1:-$ROOT/build/web}"

source ~/emsdk/emsdk_env.sh >/dev/null 2>&1 || true
command -v emcc >/dev/null || { echo "emcc not found — source ~/emsdk/emsdk_env.sh first"; exit 1; }

# 资产镜像 (三档 cbin 字体) → C 数组 (assets_partition web 分支解析)
BUILD_TS_PRE=$(date +%Y%m%d%H%M%S)
python3 "$ROOT/platforms/web/tools/pack_web_assets.py" "$BUILD/assets.img" >/dev/null
python3 "$ROOT/platforms/web/tools/gen_assets_name.py" "assets-$BUILD_TS_PRE.img" "$ROOT/platforms/web/assets_name.cc"

mkdir -p "$BUILD"
cd "$BUILD"
emcmake cmake -DTAB5_PLATFORM=web -DCMAKE_BUILD_TYPE=Release "$ROOT"
emmake make -j"$(sysctl -n hw.ncpu)" tab5web

# 组装 bin/: 外壳 + 运行时 (index.html 注入构建版本号 → wasm 缓存键)
BIN="$BUILD/bin"
cp "$ROOT/platforms/web/shell/index.html" "$ROOT/platforms/web/shell/shell.js" "$BIN/"
cp "$BUILD/assets.img" "$BIN/assets-$BUILD_TS_PRE.img"

BUILD_TS=$(date +%Y%m%d%H%M%S)
python3 - "$BIN/index.html" "$BUILD_TS" <<'PYEOF'
import sys
p, ts = sys.argv[1], sys.argv[2]
s = open(p).read()
tag = '<script>window.TAB5_BUILD="%s";</script>' % ts
if "TAB5_BUILD" not in s:
    s = s.replace('<script src="shell.js"></script>', tag + '\n<script src="shell.js"></script>')
    open(p, "w").write(s)
print("version:", ts)
PYEOF
grep -q "TAB5_BUILD" "$BIN/index.html" || { echo "version inject FAILED"; exit 1; }
ls "$BIN"/assets-*.img 2>/dev/null | grep -v "assets-$BUILD_TS_PRE.img" | xargs rm -f 2>/dev/null
echo "---- build ok: $BIN (v$BUILD_TS) ----"
