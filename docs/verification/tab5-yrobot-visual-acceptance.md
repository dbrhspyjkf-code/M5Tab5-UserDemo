# Tab5 YRobot 控制台视觉验收记录

**范围：** `feature/tab5-yrobot-cockpit` 的 iOS 风格 LVGL 界面重构。  
**提交：** `c2c1fa6 feat(reachy): redesign Tab5 cockpit for iOS-style use`  
**记录时间：** 2026-09-30（GMT+8）

## 已验证

| 验证项 | 方法 | 结果 |
| --- | --- | --- |
| API 与安全契约 | `test_reachy_client_contract.py` | 通过 |
| 视觉信息架构与 token | `test_reachy_visual_design.py` | 通过 |
| 语音控制与确认操作 | `test_reachy_controls_ui.py` | 通过 |
| 前后台生命周期与按需日志 | `test_reachy_lifecycle.py` | 通过 |
| 对话解析与气泡布局 | `test_reachy_chat.py` | 通过 |
| JPEG 相机预览生命周期 | `test_reachy_video.py` | 通过 |
| C++ 语法 | `clang++ -fsyntax-only app/apps/app_reachy/app_reachy.cpp` | 通过 |
| 桌面 CMake 构建 | Xcode 26.5 SDK + `cmake --build build-mac-desktop -j8` | 通过 |
| 桌面启动冒烟检查 | `SDL_VIDEODRIVER=dummy` 运行 5 秒 | 通过，进程稳定运行直至预期超时 |
| 空白与冲突检查 | `git diff --check` | 通过 |

## 已实现的视觉验收点

- 横屏布局采用 64 px Header、574 px 内容区、82 px 连续圆角底部 Dock。
- 导航目的地固定为：概览、语音、互动、相机、维护。
- 概览页将连接和服务健康置于首屏，配有服务、摄像头、语音三个可验证指标。
- 离线时显示“无法连接 YRobot”，并清除会产生误导的健康值。
- 语音页将音量、麦克风、后端/音色、VAD 与相机控制分组；滑块只在释放时提交。
- 互动页保留动作摘要与左右语义的聊天气泡；相机和维护操作与常规浏览分区。
- 相机预览在离开相机页或关闭应用时立即停止；日志只有点击“查看日志”后才请求。
- 未增加 WebView、WebRTC、WebSocket、HMAC、直接 daemon 运动写接口或新的后台服务。

## 桌面构建修复

桌面配置此前有两个与 worktree 相关的问题：

1. `platforms/desktop/CMakeLists.txt` 中的相对路径由仓库根解析，导致依赖、app 源码和 `lv_conf.h` 指向错误位置；现改为以该 CMake 文件所在目录为根的绝对 CMake 路径。
2. 桌面 source glob 曾把 Tab5 的 ESP-IDF 组件一并加入桌面目标；现只收集 `platforms/desktop` 源码，并显式补充 desktop 所需的 `device_state.h` include 目录。

使用完整 Xcode SDK 而不是陈旧 CommandLineTools sysroot 后，桌面模拟器已成功构建为 `build/desktop/app_desktop_build`。本轮未做屏幕级自动截图；冒烟检查确认 SDL dummy driver 下进程稳定运行 5 秒。

## 待设备验收

1. 在桌面窗口中人工查看 1280×720 的五个页面，记录中文显示、卡片不重叠、Dock 状态和离线提示截图。
2. 在已加载 ESP-IDF 环境中执行 `idf.py build`，检查固件大小、PSRAM 与链接告警。当前 shell 没有 `idf.py` 或 `IDF_PATH`。
3. 经操作者确认后烧录设备，仅做非破坏性检查：连接/断网状态、底部 Dock 点击、音量/Mic、聊天和相机启停。
4. 本视觉任务不执行重启、休眠、动作或移动底盘操作；这些操作仍需单独确认并在机器人旁验收。
