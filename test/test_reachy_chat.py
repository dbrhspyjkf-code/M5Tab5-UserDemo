import unittest
from pathlib import Path


SOURCE = (Path(__file__).resolve().parents[1] / "app/apps/app_reachy/reachy_client.h")


class ReachyChatContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = SOURCE.read_text()

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


if __name__ == "__main__":
    unittest.main()
