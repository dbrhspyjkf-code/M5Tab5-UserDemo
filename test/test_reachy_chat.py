import unittest
from pathlib import Path


SOURCE = (Path(__file__).resolve().parents[1] / "app/apps/app_reachy/reachy_client.h")
UI_HEADER = (Path(__file__).resolve().parents[1] / "app/apps/app_reachy/app_reachy.h")
UI_SOURCE = (Path(__file__).resolve().parents[1] / "app/apps/app_reachy/app_reachy.cpp")


class ReachyChatContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = SOURCE.read_text()
        cls.ui_header = UI_HEADER.read_text()
        cls.ui_source = UI_SOURCE.read_text()

    def test_chat_uses_filtered_bounded_endpoint(self):
        self.assertIn('/api/logs?filter=chat&limit=', self.source)
        self.assertIn('fetchChat(int limit = 200, size_t max_user_turns = 6)', self.source)

    def test_only_approved_chat_markers_are_parsed(self):
        for marker in ("xz stt:", "xz tts text:",
                       "qwen stt:", "qwen response:"):
            self.assertIn(marker, self.source)
        self.assertIn('text.rfind("% tool", 0)', self.source)

    def test_history_keeps_last_user_led_turns_in_order(self):
        self.assertIn("user_starts", self.source)
        self.assertIn("max_user_turns", self.source)
        self.assertIn("messages.erase", self.source)
        self.assertIn("user_starts[user_starts.size() - max_user_turns]", self.source)

    def test_chat_page_uses_scrollable_left_and_right_bubbles(self):
        self.assertIn("_chat_container", self.ui_header)
        render = self.ui_source.split("void AppReachy::_renderChat", 1)[1]
        render = render.split("void AppReachy::", 1)[0]
        self.assertIn("LV_ALIGN_TOP_RIGHT", render)
        self.assertIn("LV_ALIGN_TOP_LEFT", render)
        self.assertIn("lv_obj_scroll_to_y", render)
        self.assertIn("LV_ANIM_OFF", render)

    def test_chat_avoids_rebuild_when_signature_is_unchanged(self):
        self.assertIn("_chat_signature", self.ui_header)
        self.assertIn("if (signature == _chat_signature) return", self.ui_source)

    def test_chat_polls_every_three_seconds(self):
        self.assertIn("CHAT_POLL_MS = 3000", self.ui_header)
        self.assertIn("_active == Tab::Chat ? CHAT_POLL_MS : POLL_MS", self.ui_source)


if __name__ == "__main__":
    unittest.main()
