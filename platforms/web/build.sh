#!/bin/bash
# tab5web 构建 — Emscripten WASM 多线程版
# 用法: platforms/web/build.sh [build-dir]   (默认 build/web)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${1:-$ROOT/build/web}"
TS="$(date +%Y%m%d%H%M%S)"

source ~/emsdk/emsdk_env.sh >/dev/null 2>&1 || true
command -v emcc >/dev/null || { echo "emcc not found"; exit 1; }

# 1) 资产镜像 (版本化文件名)
mkdir -p "$BUILD"
python3 "$ROOT/platforms/web/tools/pack_web_assets.py" "$BUILD/assets.img" >/dev/null
python3 "$ROOT/platforms/web/tools/gen_assets_name.py" "assets-$TS.img" "$ROOT/platforms/web/assets_name.cc"

# 2) 编译
cd "$BUILD"
emcmake cmake -DTAB5_PLATFORM=web -DCMAKE_BUILD_TYPE=Release "$ROOT" >/dev/null
emmake make -j"$(sysctl -n hw.ncpu)" tab5web

# 3) 组装 bin/: 全静态资源版本化 (杜绝新旧缓存错配)
#    (make 产物在 $BUILD/bin, 先暂存再清空重建, 避免 rm 掉刚生成的产物)
STAGE="$BUILD/.stage"; rm -rf "$STAGE"; mkdir -p "$STAGE"
cp "$BUILD/bin/tab5web.js" "$BUILD/bin/tab5web.wasm" "$STAGE/" 2>/dev/null || { echo "make artifacts missing"; exit 1; }
BIN="$BUILD/bin"
rm -rf "$BIN"; mkdir -p "$BIN"
cp "$ROOT/platforms/web/shell/index.html" "$BIN/"
cp "$ROOT/platforms/web/shell/shell.js" "$BIN/shell-$TS.js"
cp "$STAGE/tab5web.js"   "$BIN/tab5web-$TS.js"
cp "$STAGE/tab5web.wasm" "$BIN/tab5web-$TS.wasm"
cp "$BUILD/assets.img"   "$BIN/assets-$TS.img"
rm -rf "$STAGE"
python3 - "$BIN" "$TS" <<'PYEOF'
import sys, os
bin_dir, ts = sys.argv[1], sys.argv[2]
p = os.path.join(bin_dir, "index.html")
s = open(p).read()
s = s.replace("<script src=\"shell.js\"></script>",
              f'<script>window.TAB5_BUILD="{ts}";</script>\n<script src="shell-{ts}.js"></script>')
s = s.replace("<script src=\"tab5web.js\"></script>",
              f'<script src="tab5web-{ts}.js"></script>')
open(p, "w").write(s)
assert f"shell-{ts}.js" in s and f"tab5web-{ts}.js" in s and f'TAB5_BUILD="{ts}"' in s
print("versioned:", ts)
PYEOF

# 4) brotli 预压缩
command -v brotli >/dev/null && for f in "$BIN"/shell-*.js "$BIN"/tab5web-*.js "$BIN"/tab5web-*.wasm "$BIN"/assets-*.img; do
  brotli -q 11 -f -k "$f"
done || true

echo "---- build ok: $BIN (v$TS) ----"
ls -la "$BIN" | awk '{print $5, $9}' | grep -vE "^ |^$"
