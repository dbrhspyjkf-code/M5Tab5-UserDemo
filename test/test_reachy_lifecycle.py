import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "app/apps/app_reachy/app_reachy.h").read_text()
SOURCE = (ROOT / "app/apps/app_reachy/app_reachy.cpp").read_text()


class ReachyLifecycleTests(unittest.TestCase):
    def test_background_app_does_not_poll_or_render(self):
        running = SOURCE.split("void AppReachy::onRunning()", 1)[1]
        running = running.split("void AppReachy::_buildUI", 1)[0]
        guard = running.index("lv_screen_active() != _scr")
        render = running.index("_renderOverview()")
        poll = running.index("_refreshActiveTab()")
        self.assertLess(guard, render)
        self.assertLess(guard, poll)

    def test_completed_fetches_are_rendered_once_and_generation_guarded(self):
        self.assertIn("_rendered_at_ms", HEADER)
        self.assertIn("_open_generation", HEADER)
        self.assertIn("_completed_generation", HEADER)
        running = SOURCE.split("void AppReachy::onRunning()", 1)[1]
        self.assertIn("_rendered_at_ms != fetched_at", running)
        self.assertIn("_rendered_at_ms = fetched_at", running)
        refresh = SOURCE.split("void AppReachy::_refreshActiveTab", 1)[1]
        self.assertIn("generation == _open_generation.load()", refresh)

    def test_only_active_destination_is_fetched(self):
        refresh = SOURCE.split("void AppReachy::_refreshActiveTab", 1)[1]
        self.assertIn("const Tab tab = _active", refresh)
        self.assertIn("switch (tab)", refresh)
        self.assertIn("case Tab::Interaction", refresh)

    def test_camera_stops_on_destination_change_and_close(self):
        select = SOURCE.split("void AppReachy::_selectDestination", 1)[1]
        select = select.split("void AppReachy::_refreshActiveTab", 1)[0]
        close = SOURCE.split("void AppReachy::onClose", 1)[1]
        close = close.split("void AppReachy::onRunning", 1)[0]
        self.assertIn("_stopVideoPreview", select)
        self.assertIn("_stopVideoPreview", close)

    def test_logs_are_explicitly_requested_and_never_update_lvgl_in_worker(self):
        refresh = SOURCE.split("void AppReachy::_refreshActiveTab", 1)[1]
        refresh = refresh.split("void AppReachy::_refreshHeader", 1)[0]
        self.assertIn("_logs_requested.exchange(false)", refresh)
        self.assertNotIn("lv_label_set_text", refresh)
        self.assertIn("_logs_requested.store(true)", SOURCE)

    def test_swipe_up_exit_is_retained(self):
        self.assertIn("LV_DIR_TOP", SOURCE)
        self.assertIn("_requestClose", SOURCE)


if __name__ == "__main__":
    unittest.main()
