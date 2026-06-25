# M5Tab5 HyperFrames Tutorial Voiceover

## Voice Direction

中文口播。语气像项目作者在复盘一次真实改造: 直接、具体、不端着。少用宣传词, 多用场景词。

## S1 Hook

这块 5 寸屏幕, 一开始只是 M5Tab5 的用户 demo。

但我后来把它改成了一个本地 AI 智能终端。

它可以控制 Home Assistant, 可以跑小智语音, 可以把请求交给 Mac 上的 Hermes 和 Claude, 还能显示邮件、自选股、LoRa 消息, 甚至把股票和邮件状态滚到外接 LED 灯阵上。

所以这期不只是看功能, 我想讲的是: 这个小屏幕怎么变成家里本地系统的入口。

## S2 System Map

先看结构。

固件侧是 ESP-IDF, 上面跑 Mooncake 的多 App 生命周期, UI 用 LVGL 做。

每个功能都是一个 App: Home Assistant、小智、Claude 助手、工具页、灯阵、LoRa、自选股。

Mac 侧跑的是本地服务: Hermes、邮件接口、股票接口, 还有 Claude project gateway。

Tab5 和 Mac 之间只走局域网 HTTP JSON。设备负责触摸和展示, Mac 负责密钥、状态、审批和结果。

这个边界很重要, 后面讲 Claude 工作流的时候会用到。

## S3 Maker Feature Tour

先看一个普通玩家能直接感受到的部分。

Home Assistant 面板可以直连家里的实体, 灯光、家电、门锁、鱼缸、扫地机、打印机这些都能做专门卡片。

小智语音是嵌进固件里的, 不是网页套壳。它有真实音频、在线模式、本地唤醒词和 tap-to-talk。

Claude 助手是另一个方向: 它不是聊天玩具, 而是能围绕这个项目本身提问、看上下文、发请求、读最新结果。

工具页里放了计算器、汇率、单位换算和邮件未读列表。邮件还有第二条通道: 状态栏一个图标, 外接 LED 矩阵还能滚动 NEW EMAIL。

外设这边, Unit-Puzzle 灯阵是 40x8 的 WS2812, 可以跑图案, 也能滚动自定义文字。LoRa Chat 则是用 Unit C6L 做收发聊天气泡。

最后是自选股: 它不是只显示价格, 还有 7 列行情、一句话结论、交易时段刷新, 以及外接 LED ticker。

这些功能堆在一起, 它就不再像一个 demo, 更像桌面上的个人控制台。

## S4 Local AI Loop

这个项目最有意思的部分, 是 Tab5 和本地 AI 工作流的关系。

我没有让设备直接执行命令。Tab5 只负责把意图发给 Mac。

Mac 上的 `claude_project_gateway.py` 收到请求以后, 会把它变成 pending request。

桌面侧再决定是 approve 还是 reject。

批准以后, 它生成的是 Markdown execution packet, 也就是给人或给 Codex 看的执行说明, 不是让设备自己偷偷跑 shell。

执行完以后, 结果会被记录下来, 写进 latest result。

Tab5 再通过 Latest 读取回来。

所以这个闭环是: 小屏幕负责入口, Mac 负责判断和状态, Claude/Codex 负责真正的项目工作。这样既方便, 又不会把密钥和危险执行权塞进设备里。

## S5 Embedded Pitfalls

做这个项目最真实的部分, 不是加了多少 App, 而是每个 App 背后都有坑。

PORT A 会被灯阵、LoRa、邮件 LED 抢, 所以要做 ownership 仲裁。

ESP32-P4 的内存也不是无限的, 小智和多个 UI App 放在一起, 就要做挂起和恢复。

Home Assistant 调试时, 重启问题不能靠猜。真正有用的是串口证据: 什么时候断、是不是 panic、USB 设备有没有从 macOS 消失。

LVGL 字体也会暴露问题, 缺字、刷新、PSRAM 覆盖, 都会在真实设备上出来。

LED 灯阵闪烁, 最后也不是换动画解决, 而是发现 clear 里面已经 refresh 过, 双刷导致闪。

自选股那边, 后端还要把东方财富返回的字符串数字转成 float, 不然固件侧 JSON 解析会崩。

这些坑放在一起, 其实就是一句话: 嵌入式项目要相信硬件证据, 不要只相信代码看起来没问题。

## S6 Build And Flash

复现路线可以分成几层。

UI 改动先看桌面模拟器, 不用每次都烧设备。

真正上 Tab5 的时候, 进入 `platforms/tab5`, 用项目里的 ESP-IDF 环境构建。

如果 `idf.py` 卡住, 这个项目里用 ninja 更稳。

烧录时找 `/dev/cu.usbmodem*`, 然后用 esptool 写 bootloader、partition table、主固件和 srmodels。

最后不要只看烧录成功。要看设备能不能连 WiFi, HA 能不能刷新, Hermes 能不能通, 股票和 LED 有没有真实更新。

## S7 Closing

所以这个项目最有意思的, 不是某一个 App。

而是 M5Tab5 这块屏幕, 变成了本地 AI 和真实设备之间的触摸入口。

它一边连着家里的灯、邮件、股票和外设, 一边连着 Mac 上的 Hermes 和 Claude。

这就是我想要的本地 AI 智能终端。
