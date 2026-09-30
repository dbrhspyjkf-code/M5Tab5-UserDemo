import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "app/apps/app_reachy/app_reachy.h").read_text()
SOURCE = (ROOT / "app/apps/app_reachy/app_reachy.cpp").read_text()
UI = (ROOT / "app/apps/app_reachy/reachy_ui.h").read_text()


class ReachyVisualDesignTests(unittest.TestCase):
    def test_ios_style_tokens_live_in_shared_component_factory(self):
        for token in ("kBg = 0x061629", "kSurface = 0x102642", "kAccent = 0x32D7E6",
                      "kAccentSoft = 0x1F5A84", "kTextMuted = 0xA2B5CA"):
            self.assertIn(token, UI)
        for factory in ("makeSurface", "makeLabel", "makeButton", "makeStatusPill"):
            self.assertIn(factory, UI)

    def test_five_destinations_replace_old_eight_tab_navigation(self):
        self.assertIn("Overview, Voice, Interaction, Camera, Maintenance, Count", HEADER)
        self.assertIn('{"概览", "语音", "互动", "相机", "维护"}', SOURCE)
        self.assertIn("_buildBottomDock", SOURCE)
        self.assertNotIn('{"状态", "运动", "音频", "控制", "聊天", "视频", "系统", "日志"}', SOURCE)

    def test_landscape_shell_has_header_content_and_bottom_dock(self):
        self.assertIn("HEADER_H = 64", HEADER)
        self.assertIn("DOCK_H = 82", HEADER)
        self.assertIn("makeStatusPill", SOURCE)
        self.assertIn("H - DOCK_H + 8", SOURCE)

    def test_overview_prioritizes_health_and_three_metrics(self):
        overview = SOURCE.split("void AppReachy::_buildOverviewPage()", 1)[1]
        overview = overview.split("void AppReachy::_buildVoicePage()", 1)[0]
        for text in ("YRobot 状态", "服务", "摄像头", "语音", "最近对话"):
            self.assertIn(text, overview)
        self.assertIn("服务正常", SOURCE)
        self.assertIn("无法连接 YRobot", SOURCE)

    def test_entry_uses_yrobot_console_name(self):
        self.assertIn('setAppInfo().name = "YRobot 控制台"', SOURCE)

    def test_no_generated_full_screen_visual_is_embedded(self):
        self.assertNotIn("generated-images", SOURCE)
        self.assertNotIn("tab5-yrobot-ios-cockpit-direction", SOURCE)


if __name__ == "__main__":
    unittest.main()
