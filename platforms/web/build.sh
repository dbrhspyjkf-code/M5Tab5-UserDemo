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
# car.html 轻量车机页 (纯 HTML 零构建, L9/iPad/Mac 通用; 方案见项目 memory/car-html-plan.md)
# css/js 必须外置: 生产网关 CSP default-src 'self' 禁内联 <style>/<script> (2026-11-03 实锤)
cp "$ROOT/platforms/web/shell/car.html" "$BIN/"
cp "$ROOT/platforms/web/shell/car.css"  "$BIN/"
cp "$ROOT/platforms/web/shell/car.js"   "$BIN/"
rm -rf "$STAGE"
# glue 内裸名 wasm 字面量自版本化: blob 上下文的 worker (scriptDir 失效)
# 会按字面量拉 "tab5web.wasm" —— 网关裸名 302 永远指最新版, glue 一旦
# 陈旧(用户缓存)就 新旧 import 表错配 → LinkError (2026-11-03 iPad 实锤)。
# 改成字面量带 TS: 每版 glue 永远拉自己的 wasm, 陈旧缓存只会跑旧版不会崩。
python3 - "$BIN/tab5web-$TS.js" "$TS" <<'PYEOF'
import sys
p, ts = sys.argv[1], sys.argv[2]
s = open(p).read()
old = 'locateFile("tab5web.wasm")'
new = f'locateFile("tab5web-{ts}.wasm")'
assert s.count(old) == 1, f"bare wasm literal count={s.count(old)}, expect 1"
open(p, "w").write(s.replace(old, new))
print("glue wasm literal versioned ->", ts)
PYEOF
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
