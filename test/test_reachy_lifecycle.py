import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "app/apps/app_reachy/app_reachy.h"
SOURCE = ROOT / "app/apps/app_reachy/app_reachy.cpp"


class ReachyLifecycleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.header = HEADER.read_text()
        cls.source = SOURCE.read_text()

    def test_background_app_does_not_poll_or_render(self):
        on_running = self.source.split("void AppReachy::onRunning()", 1)[1]
        on_running = on_running.split("// ── UI build", 1)[0]
        foreground_guard = on_running.index("lv_screen_active() != _scr")
        render = on_running.index("_renderStatus()")
        poll = on_running.index("_refreshActiveTab()")
        self.assertLess(foreground_guard, render)
        self.assertLess(foreground_guard, poll)

    def test_each_completed_fetch_is_rendered_once(self):
        self.assertIn("_rendered_at_ms", self.header)
        on_running = self.source.split("void AppReachy::onRunning()", 1)[1]
        on_running = on_running.split("// ── UI build", 1)[0]
        self.assertIn("_rendered_at_ms != fetched_at", on_running)
        self.assertIn("_rendered_at_ms = fetched_at", on_running)

    def test_reachy_owns_swipe_up_exit(self):
        self.assertIn("_installSwipeGesture", self.header)
        self.assertIn("_requestClose", self.header)
        self.assertIn("LV_DIR_TOP", self.source)
        self.assertIn("self->_requestClose()", self.source)


if __name__ == "__main__":
    unittest.main()
