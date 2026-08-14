import unittest
from pathlib import Path


SOURCE = (Path(__file__).resolve().parents[1] / "app/apps/app_reachy/app_reachy.cpp").read_text()


class ReachyBackendSwitchTests(unittest.TestCase):
    def test_switch_saves_restarts_then_polls_status(self):
        body = SOURCE.split("AppReachy::_switchBackend", 1)[1]
        body = body.split("void AppReachy::", 1)[0]
        save = body.index("setBackend(target)")
        restart = body.index("restartYRobot()")
        poll = body.index("fetchStatus()")
        self.assertLess(save, restart)
        self.assertLess(restart, poll)
        self.assertIn("configured_backend == target", body)
        self.assertIn("runtime_backend == target", body)

    def test_switch_has_30_second_timeout_and_no_fallback(self):
        body = SOURCE.split("AppReachy::_switchBackend", 1)[1]
        body = body.split("void AppReachy::", 1)[0]
        self.assertIn("30", body)
        self.assertNotIn("setBackend(\"xiaozhi\")", body)
        self.assertNotIn("setBackend(\"qwen\")", body)


if __name__ == "__main__":
    unittest.main()
