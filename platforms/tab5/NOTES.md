# Tab5 HA 面板 - 开发备忘

## 构建（本地 ESP-IDF 即可，无需 Docker）

2026-10-25 实测：本地 `idf.py build` 可行，只需补 `export ESP_IDF_VERSION=v5.5.2`
（esp_wifi_remote 的 Kconfig 展开需要；Docker 镜像自带该变量，所以容器内不报错）。
macOS 上另需 `SDKROOT=$(xcrun --sdk macosx --show-sdk-path)`（host 工具链编译探测用）。

Docker 备用：

```bash
docker run --rm \
  -v "$(pwd)":/project \
  -w /project/platforms/tab5 \
  espressif/idf:v5.5.2 \
  bash -c "idf.py build 2>&1"
```

> 注意：仓库根目录的 `build/` 是失效的桌面 configure 产物，固件工程在本目录 `platforms/tab5/`。

## 刷机

> 2026-10-25 起 Tab5 枚举为 `/dev/cu.usbmodem1301`（12401 上挂的是另一块 ESP32-C3，
> esptool 会报 "wrong chip"——刷不动先换口）。USB 枚举名不保证稳定，以 `ls /dev/cu.usbmodem*` 实查为准。

### 全量刷机（2026-10-25 重分区后，见下方分区布局）

```bash
cd platforms/tab5
python3 tools/pack_assets.py          # 生成 build/assets.img（改了 home_bg/字体后必跑）
esptool --chip esp32p4 -p /dev/cu.usbmodem1301 -b 460800 \
  --before default-reset --after hard-reset write-flash \
  --flash-mode dio --flash-size 16MB --flash-freq 80m \
  0x8000  build/partition_table/partition-table.bin \
  0x10000 build/m5stack_tab5.bin \
  0x910000 build/assets.img \
  0xe90000 build/srmodels/srmodels.bin
```

只刷 app（日常迭代，不动分区表/资源）：
```bash
esptool --chip esp32p4 -p /dev/cu.usbmodem1301 -b 460800 \
  --before default-reset --after hard-reset write-flash \
  --flash-mode dio --flash-size 16MB --flash-freq 80m \
  0x10000 build/m5stack_tab5.bin
```

### 分区布局（partitions.csv，2026-10-25 起）

| 分区 | 偏移 | 大小 | 内容 |
|---|---|---|---|
| nvs | 0x9000 | 24K | 配置（ha_token/reachy_host 等）——偏移勿动，动了丢配置 |
| factory | 0x10000 | 9M | app（瘦身后 ~6.9M，余 26%） |
| res | 0x910000 | 5.5M | 只读资源：home_bg + 20/24px 中文字体（pack_assets.py 打包，`app/apps/utils/assets/` 经 esp_partition_mmap 原址读） |
| model | 0xd10000 | 512K | xiaozhi srmodels 语音模型 |
| （尾部预留） | 0xf10000 | ~0.9M | 未来扩 factory / OTA / 恢复 xiaozhi assets 分区用 |

注意：
- `res` 分区标签刻意避开 `assets`——xiaozhi 的 Assets 系统（MCP 表情包下载）
  认领 `assets` 标签；原 2M 空 assets 分区已删除，xiaozhi 干净降级为 disabled。
  若以后要用其表情包下载，从尾部预留切一块 `assets` 分区即可。
- NVS 备份（重分区前做的）：`~/Projects/M5Tab5/nvs_backup_tab5_2026-10-25.bin`
  （含 HA token，放在 git 仓库外）。
- 20px 字体源文件在 managed_components/78__xiaozhi-fonts/cbin/
  font_puhui_common_20_4.bin（component manager 管理，fullclean 后会重新下载）。

## 关键坑

### 1. WiFi SDIO 引脚
esp_hosted Kconfig 默认值是错的。Tab5 实际引脚（已写入 sdkconfig）：
- CLK=12, CMD=13, D0=11, D1=10, D2=9, D3=8, RST=15
- 参考：`m5tab5_getbingwallpaper_clock` Arduino 示例

### 2. 音频 codec 跳过
`bsp_codec_init()` 用旧 I2C API，与 BSP 新 API 在 ESP-IDF v5.5.2 冲突。
已在 `main/hal/hal_esp32.cpp` 注释掉，所有音频函数加了 `_codec_ready()` 守护。

### 3. LVGL 锁
`GetMooncake().update()` 必须在 LVGL 锁内调用，否则 `lv_inv_area` 断言触发死循环。
已在 `app/app.cpp::Update()` 里加 `LvglLockGuard lock`。

### 4. esp_hosted 放在 components/
不要放回 managed_components/，否则 fullclean 会还原对 `esp_hosted_config.h` 的修改。

### 5. pthread 栈
默认 3KB 不够 HTTP+JSON，已改为 16KB。

## 配置文件

关键配置都在 `sdkconfig.defaults`，`sdkconfig` 由 Docker build 自动生成后手动 patch 引脚值。

## WiFi / HA 配置

通过设备 Settings App 配置，运行时存入 NVS，无需硬编码。
- WiFi SSID / 密码：开机后在 Settings → WiFi 扫描连接
- HA 地址（ha_host）：Settings → HA 填写，如 `192.168.1.xxx`
- HA Token：在 HA → Profile → Long-Lived Access Tokens 生成，填入 Settings

## 待办

- [ ] 孩子房灯加入卧室 tab（`app/apps/app_ha/app_ha.cpp` 的 bedroom vector）
- [ ] 门锁 operationID 用户名（`app_ha.cpp` LOCK_USERS 表，ID 从 HA 日志获取后填入）
- [ ] Sonos 媒体播放器（延后）

### 中文字体三档（2026-10-25）

- 30px `zh_font_lg`：cbin 内嵌固件（xiaozhi 组件，对话框/状态栏/屏保）
- 24px `zh_font_md`：res 分区 `font_zh_24`，**自产**——`tools/gen_cbin_font.py`
  从 `78__xiaozhi-fonts/ttf/puhei-common.ttf` 生成（lv_font_conv 出 C 数组再平铺成 cbin 格式），
  源 .bin 存 `app/assets/fonts/`。主页仪表卡正文用。
- 20px `zh_font_sm`：res 分区 `font_zh_20`（组件原装 cbin）。OPi 卡模块小字用。
- 坑：cbin 格式里 `lv_font_t.dsc` 指针在 **offset 24**（20-23 是 subpx/underline 位域+padding），
  序列化时写错位置字体整体查不到字形（gen_cbin_font.py 已带解码自检 + ASCII 渲染验证）。
