# Tab5 HA 面板 - 开发备忘

## 构建（必须用 Docker）

本地 ESP-IDF 缺少 `ESP_IDF_VERSION` 环境变量，`esp_wifi_remote` Kconfig 展开失败，必须用 Docker：

```bash
cd ~/Desktop/Projects/M5Tab5/M5Tab5-UserDemo
docker run --rm \
  -v "$(pwd)":/project \
  -w /project/platforms/tab5 \
  espressif/idf:v5.5.2 \
  bash -c "idf.py build 2>&1"
```

## 刷机

```bash
cd platforms/tab5
esptool --chip esp32p4 -p /dev/cu.usbmodem12401 -b 460800 \
  --before default-reset --after hard-reset write-flash \
  --flash-mode dio --flash-size 16MB --flash-freq 80m \
  0x2000  build/bootloader/bootloader.bin \
  0x8000  build/partition_table/partition-table.bin \
  0x10000 build/m5stack_tab5.bin
```

只刷 app（不刷 bootloader/partition）：
```bash
esptool --chip esp32p4 -p /dev/cu.usbmodem12401 -b 460800 \
  --before default-reset --after hard-reset write-flash \
  --flash-mode dio --flash-size 16MB --flash-freq 80m \
  0x10000 build/m5stack_tab5.bin
```

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
