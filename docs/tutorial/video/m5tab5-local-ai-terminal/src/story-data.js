(() => {
  const duration = 420;

  const scenes = [
    { id: "s1", title: "Hook", start: 0, duration: 18 },
    { id: "s2", title: "System Map", start: 18, duration: 42 },
    { id: "s3", title: "Maker Feature Tour", start: 60, duration: 125 },
    { id: "s4", title: "Local AI Loop", start: 185, duration: 90 },
    { id: "s5", title: "Embedded Pitfalls", start: 275, duration: 75 },
    { id: "s6", title: "Build And Flash", start: 350, duration: 55 },
    { id: "s7", title: "Closing", start: 405, duration: 15 },
  ];

  const featureCards = [
    ["Home Assistant", "直连家里的真实实体", "app/apps/app_ha"],
    ["Xiaozhi", "固件内的语音助手", "app/apps/app_xiaozhi"],
    ["Claude Assistant", "项目请求与结果回读", "app/apps/app_project_assistant"],
    ["Voice Input", "录音波形与键盘输入", "app/apps/app_voice_input"],
    ["Tools", "计算器 / 汇率 / 单位 / 邮件", "app/apps/app_settings"],
    ["Email LED", "未读邮件外接提示", "app/apps/app_email_led"],
    ["Unit-Puzzle LED", "40x8 灯阵与 ticker", "app/apps/app_unit_puzzle"],
    ["LoRa Chat", "本地消息气泡", "app/apps/app_lora_chat"],
    ["Stocks", "自选股行情与结论", "app/apps/app_stocks"],
  ];

  const aiLoopSteps = [
    "Request from Tab5",
    "LAN HTTP JSON",
    "claude_project_gateway.py",
    "Pending request",
    "Desktop approve / reject",
    "Markdown execution packet",
    "Latest result back to Tab5",
  ];

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

  window.m5tab5StoryData = {
    duration,
    scenes,
    featureCards,
    aiLoopSteps,
    pitfalls,
    buildCommands,
  };
})();
