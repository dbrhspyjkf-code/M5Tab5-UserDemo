#!/bin/bash
# tab5web 构建并发布到 OrangePi
#   platforms/web/deploy.sh          # 构建 + rsync 到 Pi
#   platforms/web/deploy.sh --build-only
# 产物: build/web/bin/{index.html,shell.js,tab5web.js,tab5web.wasm,tab5web.worker.js,assets.img}
# Pi 侧: ~/tab5-web/www/ (正式网关 tab5_web_gateway 托管)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build/web"
BIN="$BUILD/bin"
PI="${TAB5_PI:-orangepi@192.168.1.200}"
PI_DIR="${TAB5_PI_DIR:-tab5-web/www}"

source ~/emsdk/emsdk_env.sh >/dev/null 2>&1 || true
command -v emcc >/dev/null || { echo "emcc not found — source ~/emsdk/emsdk_env.sh"; exit 1; }

# 1. 编译
mkdir -p "$BIN"
cmake -S "$ROOT" -B "$BUILD" -DTAB5_PLATFORM=web -DCMAKE_BUILD_TYPE=Release 2>&1 \
  | tail -2  # 已 emcmake 配置过的目录 cmake 可直接增量
cmake --build "$BUILD" -j "$(sysctl -n hw.ncpu)" --target tab5web 2>&1 | tail -3

# 2. 组装 bin/
cp "$ROOT/platforms/web/shell/index.html" "$ROOT/platforms/web/shell/shell.js" "$BIN/"
python3 "$ROOT/platforms/web/tools/pack_web_assets.py" "$BIN/assets.img"

# 3. 预压缩 brotli (ECS 3Mbps — 首包必须压)
command -v brotli >/dev/null && for f in "$BIN"/*.js "$BIN"/*.wasm "$BIN"/*.html "$BIN"/*.img; do
  [ -f "$f" ] && brotli -q 11 -f -k "$f"
done || echo "(brotli 未安装, 跳过预压缩 — brew install brotli)"

ls -la "$BIN"
echo "---- wasm 产物密钥扫描 ----"
if grep -l -r "x-c3-bearer\|HA_TOKEN\|secrets" "$BIN" 2>/dev/null; then echo "!! 可疑内容, 人工确认"; fi

# 4. 发布
if [ "${1:-}" != "--build-only" ]; then
  echo "---- rsync -> $PI:$PI_DIR ----"
  ssh "$PI" "mkdir -p ~/$PI_DIR"
  rsync -av --chmod=D755,F644 "$BIN/" "$PI:$PI_DIR/"
  echo "发布完成: https://$(hostname -s | xargs echo)/  (由 Pi tab5_web_gateway 托管)"
fi
