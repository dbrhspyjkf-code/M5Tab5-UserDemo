#pragma once
/*
 * rgb565_fetch — 从 hermes 拉 LVGL RGB565 裸像素图（服务端 PIL 已转好，设备零解码直显）。
 *
 * 缓冲布局 [lv_image_dsc_t][像素] 一体分配（PSRAM），可直接 lv_image_set_src。
 * 从 AppIdleScreen 的 ph_buf_alloc/ph_download 泛化而来（屏保自身未改用本助手）。
 * 设备: esp_http_client 流式直读 + 每跨 32KB 让 2ms —— esp_hosted SDIO RX 缓冲池需要
 * 喘息，一次灌 800KB 会触发 sdio_rx_get_buffer 断言重启（2026-10-26 640px 封面实锤）。
 * 桌面: hal httpGet 一步到位。download 是阻塞网络 IO，只在 worker 线程调用。
 */
#include <lvgl.h>
#include <string>

namespace rgb565 {

// 分配 w×h 的 [dsc][像素] 缓冲并填好 RGB565 头；失败返回 nullptr
void* alloc(int w, int h);
// 释放 alloc 的缓冲（nullptr 安全）；调用前须确保已无 lv_image 引用它
void release(void* buf);
// GET url → 写入 buf 像素区，响应体须恰为 w*h*2 字节；成功返回 true
bool download(const std::string& url, void* buf);

}  // namespace rgb565
