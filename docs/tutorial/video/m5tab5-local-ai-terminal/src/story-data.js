(() => {
  const duration = 420;

  const scenes = [
    {
      id: "s1",
      title: "Hook",
      start: 0,
      duration: 18,
      eyebrow: "M5Tab5 Local AI Terminal",
      heading: "把 M5Tab5 变成本地 AI 智能终端",
      subtitle: "一块 5 寸触摸屏, 连接 Home Assistant、Hermes、Claude 和真实设备。",
      timeline: "00:00-00:18",
    },
    {
      id: "s2",
      title: "System Map",
      start: 18,
      duration: 42,
      eyebrow: "System Map",
      heading: "固件、App 与本地服务的真实边界",
      subtitle: "设备负责触摸和展示, Mac 负责密钥、状态、审批和结果。",
      timeline: "00:18-01:00",
    },
    {
      id: "s3",
      title: "Maker Feature Tour",
      start: 60,
      duration: 125,
      eyebrow: "Maker Feature Tour",
      heading: "先看能直接用的九个功能位",
      subtitle: "智能家居、语音、Claude 工作流、工具、邮件、灯阵、LoRa 和自选股。",
      timeline: "01:00-03:05",
    },
    {
      id: "s4",
      title: "Local AI Loop",
      start: 185,
      duration: 90,
      eyebrow: "Local AI Loop",
      heading: "Tab5 发意图, Mac 守住审批和执行边界",
      subtitle: "设备不直接跑 shell; 它把请求交给本地网关, 再读回 latest result。",
      timeline: "03:05-04:35",
    },
    {
      id: "s5",
      title: "Embedded Pitfalls",
      start: 275,
      duration: 75,
      eyebrow: "Embedded Pitfalls",
      heading: "真实嵌入式项目靠证据收敛",
      subtitle: "端口、内存、字体、刷新、串口和服务边界, 每个坑都要挨着它的修复。",
      timeline: "04:35-05:50",
    },
    {
      id: "s6",
      title: "Build And Flash",
      start: 350,
      duration: 55,
      eyebrow: "Build And Flash",
      heading: "复现路线: 模拟器、构建、刷机、真实验证",
      subtitle: "先看 UI, 再构建 Tab5 固件, 最后用设备行为和服务状态证明它真的通。",
      timeline: "05:50-06:45",
    },
    {
      id: "s7",
      title: "Closing",
      start: 405,
      duration: 15,
      eyebrow: "Closing",
      heading: "小屏幕变成本地 AI 和真实设备之间的触摸入口",
      subtitle: "M5Tab5 一边连着家里的灯、邮件、股票和外设, 一边连着 Mac 上的 Hermes 和 Claude。",
      timeline: "06:45-07:00",
    },
  ];

  const appFlashes = [
    ["HA", "家里实体控制"],
    ["Xiaozhi", "语音入口"],
    ["Claude", "项目请求"],
    ["Mail", "未读与 LED"],
    ["Stocks", "行情与结论"],
  ];

  const systemLayers = [
    {
      title: "M5Tab5 Firmware",
      accent: "hardware",
      items: ["ESP-IDF", "Mooncake lifecycle", "LVGL UI", "AppHome launcher"],
    },
    {
      title: "App Layer",
      accent: "home",
      items: ["HA", "Xiaozhi", "Claude", "Tools", "LED", "LoRa", "Stocks"],
    },
    {
      title: "Mac Local Services",
      accent: "service",
      items: ["Hermes", "Mail :8768", "Stocks :8766", "Claude Gateway"],
    },
  ];

  const featureCards = [
    ["Home Assistant", "直连灯光、门锁、鱼缸、扫地机、打印机", "app/apps/app_ha", "home"],
    ["Xiaozhi", "真实音频、在线模式、唤醒词、tap-to-talk", "app/apps/app_xiaozhi", "ai"],
    ["Claude Assistant", "围绕项目提问、发请求、读最新结果", "app/apps/app_project_assistant", "ai"],
    ["Voice Input", "录音波形与键盘输入", "app/apps/app_voice_input", "hardware"],
    ["Tools", "计算器、汇率、单位换算、邮件未读列表", "app/apps/app_settings", "service"],
    ["Email LED", "状态栏图标与外接 NEW EMAIL 提示", "app/apps/app_email_led", "service"],
    ["Unit-Puzzle LED", "40x8 WS2812 图案与自定义 ticker", "app/apps/app_unit_puzzle", "hardware"],
    ["LoRa Chat", "Unit C6L 收发聊天气泡", "app/apps/app_lora_chat", "hardware"],
    ["Stocks", "7 列行情、一句话结论、交易时段刷新", "app/apps/app_stocks", "home"],
  ];

  const aiLoopSteps = [
    ["01", "Request from Tab5", "小屏幕只发送意图"],
    ["02", "LAN HTTP JSON", "局域网内传输结构化请求"],
    ["03", "claude_project_gateway.py", "Mac 侧接住项目上下文"],
    ["04", "Pending request", "进入待审队列"],
    ["05", "Approve / Reject", "桌面侧明确批准或拒绝"],
    ["06", "Markdown execution packet", "生成给人或 Codex 看的执行说明"],
    ["07", "Latest result", "Tab5 只读取最新结果回显"],
  ];

  const guardrails = ["no secrets on device", "no direct shell execution", "local approval required"];

  const pitfalls = [
    ["PORT A 抢占", "setPortAOwnedByApp 仲裁"],
    ["P4 内存压力", "App 挂起 / 恢复"],
    ["USB Host 不稳", "HA 验证禁用无用路径"],
    ["LVGL 缺字", "字体覆盖与安全标签"],
    ["LED 双刷闪烁", "只清缓冲, 末尾刷新"],
    ["行情字符串数字", "Hermes 端转 float"],
    ["PSRAM 字形风险", "小点阵字体进 rodata"],
  ];

  const buildCommands = [
    "source ~/.local/bin/idf_env.sh",
    "cd ~/Projects/M5Tab5/M5Tab5-UserDemo/platforms/tab5",
    "ninja -C build",
    "python -m esptool --chip esp32p4 ... write_flash ...",
  ];

  const verificationChecks = ["Boot", "WiFi", "HA", "Hermes", "Stocks", "LED"];

  const evidence = [
    "docs/TUTORIAL-outline.md",
    "git history",
    "HA stability / flashing memory",
    "Tab5-to-local-Claude approval memory",
  ];

  window.m5tab5StoryData = {
    duration,
    scenes,
    appFlashes,
    systemLayers,
    featureCards,
    aiLoopSteps,
    guardrails,
    pitfalls,
    buildCommands,
    verificationChecks,
    evidence,
  };
})();
