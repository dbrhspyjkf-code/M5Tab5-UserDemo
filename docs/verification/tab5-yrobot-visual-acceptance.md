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

## 当前构建环境限制

桌面 CMake 配置未能完成：本机编译器在 CMake 的 C 编译器探测阶段链接 `libSystem.tbd` 时出现 `tapi error: malformed file` / `unknown architecture arm64e.x1-macos`。该失败发生在项目源代码编译前。

因此，本记录将桌面视觉截图和 ESP-IDF 产物列为待完成项，而不将静态/语法验证误写为完整设备构建验证。

## 待人工/设备验收

1. 修复或切换到可工作的 Xcode/CommandLineTools SDK 后运行桌面模拟器，记录 1280×720 的五个页面截图。
2. 在已加载 ESP-IDF 环境中执行 `idf.py build`，检查固件大小、PSRAM 与链接告警。
3. 经操作者确认后烧录设备，仅做非破坏性检查：连接/断网状态、底部 Dock 点击、音量/Mic、聊天和相机启停。
4. 本视觉任务不执行重启、休眠、动作或移动底盘操作；这些操作仍需单独确认并在机器人旁验收。
