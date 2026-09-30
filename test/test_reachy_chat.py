import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "app/apps/app_reachy/reachy_client.h").read_text()
UI_HEADER = (ROOT / "app/apps/app_reachy/app_reachy.h").read_text()
UI_SOURCE = (ROOT / "app/apps/app_reachy/app_reachy.cpp").read_text()
SDKCONFIG = (ROOT / "platforms/tab5/sdkconfig").read_text()
DEFAULTS = (ROOT / "platforms/tab5/sdkconfig.defaults").read_text()


class ReachyChatContractTests(unittest.TestCase):
    def test_chat_uses_filtered_bounded_endpoint(self):
        self.assertIn('/api/logs?filter=chat&limit=', SOURCE)
        self.assertIn('fetchChat(int limit = 200, size_t max_user_turns = 6)', SOURCE)

    def test_only_approved_chat_markers_are_parsed(self):
        for marker in ("xz stt:", "xz tts text:", "qwen stt:", "qwen response:"):
            self.assertIn(marker, SOURCE)

    def test_interaction_combines_motion_and_chat(self):
        build = UI_SOURCE.split("void AppReachy::_buildInteractionPage()", 1)[1]
        build = build.split("void AppReachy::_buildCameraPage()", 1)[0]
        self.assertIn("动作与注视", build)
        self.assertIn("最近对话", build)
        self.assertIn("_chat_container", UI_HEADER)

    def test_chat_keeps_left_and_right_bubbles_and_skips_duplicates(self):
        render = UI_SOURCE.split("void AppReachy::_renderChat", 1)[1]
        render = render.split("void AppReachy::_renderMaintenance", 1)[0]
        self.assertIn("LV_ALIGN_TOP_RIGHT", render)
        self.assertIn("LV_ALIGN_TOP_LEFT", render)
        self.assertIn("if (signature == _chat_signature) return", render)
        self.assertIn("lv_obj_scroll_to_y", render)

    def test_chat_bubble_width_is_large_but_bounded_for_tab5(self):
        render = UI_SOURCE.split("void AppReachy::_renderChat", 1)[1]
        match = re.search(r"makeSurface\(_chat_container, 0, 0, (\d+)", render)
        self.assertIsNotNone(match)
        self.assertEqual(int(match.group(1)), 520)

    def test_interaction_refreshes_chat_every_three_seconds(self):
        self.assertIn("CHAT_POLL_MS = 3000", UI_HEADER)
        self.assertIn("_active == Tab::Interaction ? CHAT_POLL_MS : POLL_MS", UI_SOURCE)

    def test_tab5_keeps_lvgl_heap_in_psram(self):
        self.assertIn("ensure_reachy_lvgl_pool();", UI_SOURCE)
        self.assertIn("MALLOC_CAP_SPIRAM", UI_SOURCE)
        self.assertIn("lv_mem_add_pool", UI_SOURCE)
        setting = "CONFIG_LV_MEM_POOL_EXPAND_SIZE_KILOBYTES=128"
        self.assertIn(setting, SDKCONFIG)
        self.assertIn(setting, DEFAULTS)


if __name__ == "__main__":
    unittest.main()
