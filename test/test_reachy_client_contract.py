import re
import unittest
from pathlib import Path


SOURCE = (Path(__file__).resolve().parents[1] / "app/apps/app_reachy/reachy_client.h")


def function_body(source: str, name: str) -> str:
    match = re.search(rf"inline\s+[^\n]+\s+{name}\s*\([^)]*\)\s*\{{", source)
    if not match:
        return ""
    start = match.end()
    depth = 1
    pos = start
    while pos < len(source) and depth:
        depth += (source[pos] == "{") - (source[pos] == "}")
        pos += 1
    return source[start:pos - 1]


class ReachyClientContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = SOURCE.read_text()

    def test_typed_control_and_chat_state_exist(self):
        for struct in ("ControlState", "ChatMessage", "OperationResult"):
            self.assertIn(f"struct {struct}", self.source)

    def test_fixed_routes_and_payload_fields(self):
        expected = {
            "setVolume": ("/api/volume", "percent", "httpPut"),
            "setMicEnabled": ("/api/audio/input", "enabled", "httpPut"),
            "setVad": ("/api/audio/vad", "rms_min", "httpPut"),
            "setBackend": ("/api/conversation/backend", "backend", "httpPut"),
            "setVideoEnabled": ("/api/conversation/video", "enabled", "httpPut"),
            "setVoice": ("/api/conversation/voice", "voice", "httpPut"),
            "setCameraRunning": ("/api/camera/state", "running", "httpPut"),
            "daemonAction": ("/api/reachy-daemon/action", "action", "httpPost"),
        }
        for name, (route, field, method) in expected.items():
            with self.subTest(name=name):
                body = function_body(self.source, name)
                self.assertTrue(body, f"missing {name}")
                self.assertIn(route, body)
                self.assertIn(field, body)
                self.assertIn(method, body)
                self.assertIn("Content-Type", body)
                self.assertIn("application/json", body)

    def test_daemon_action_is_allowlisted(self):
        body = function_body(self.source, "daemonAction")
        for action in ('"wake"', '"sleep"', '"restart"'):
            self.assertIn(action, body)
        self.assertIn("invalid daemon action", body)

    def test_restart_route_is_explicit(self):
        body = function_body(self.source, "restartYRobot")
        self.assertIn("/api/system/restart", body)
        self.assertIn("httpPost", body)


if __name__ == "__main__":
    unittest.main()
