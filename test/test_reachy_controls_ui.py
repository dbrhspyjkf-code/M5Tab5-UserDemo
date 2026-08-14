import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "app/apps/app_reachy/app_reachy.h").read_text()
SOURCE = (ROOT / "app/apps/app_reachy/app_reachy.cpp").read_text()


class ReachyAudioControlTests(unittest.TestCase):
    def test_seven_tabs_have_exact_order(self):
        labels = '{"状态", "运动", "音频", "控制", "聊天", "系统", "日志"}'
        self.assertIn(labels, SOURCE)
        self.assertIn("Control", HEADER)
        self.assertIn("Chat", HEADER)
        self.assertIn("_tab_btns[(int)Tab::Count]", HEADER)

    def test_audio_uses_two_sliders(self):
        audio = SOURCE.split("void AppReachy::_buildAudioPage()", 1)[1]
        audio = audio.split("void AppReachy::_renderAudio()", 1)[0]
        self.assertGreaterEqual(audio.count("lv_slider_create"), 2)
        self.assertIn("lv_slider_set_range(_au_volume_slider, 0, 100)", audio)
        self.assertIn("lv_slider_set_range(_au_vad_slider, 1, 500)", audio)

    def test_sliders_submit_only_on_release(self):
        self.assertGreaterEqual(SOURCE.count("LV_EVENT_RELEASED"), 2)
        self.assertIn("_audioEventCb", HEADER)
        callback = SOURCE.split("void AppReachy::_audioEventCb", 1)[1]
        callback = callback.split("void AppReachy::", 1)[0]
        self.assertIn("_queueOperation", callback)
        self.assertNotIn("reachy_client::", callback)

    def test_mic_and_mute_are_explicit_buttons(self):
        for handle in ("_au_mic_btn", "_au_mute_btn"):
            self.assertIn(handle, HEADER)
            self.assertIn(f"lv_obj_add_event_cb({handle}", SOURCE)

    def test_network_writes_run_in_worker(self):
        queue = SOURCE.split("bool AppReachy::_queueOperation", 1)[1]
        queue = queue.split("reachy_client::OperationResult AppReachy::_executeOperation", 1)[0]
        self.assertNotIn("reachy_client::", queue)
        refresh = SOURCE.split("void AppReachy::_refreshActiveTab", 1)[1]
        refresh = refresh.split("void AppReachy::", 1)[0]
        self.assertIn("_executeOperation", refresh)

    def test_control_page_routes_dangerous_actions_to_confirmation(self):
        callback = SOURCE.split("void AppReachy::_controlEventCb", 1)[1]
        callback = callback.split("void AppReachy::", 1)[0]
        self.assertIn("_confirmOperation", callback)
        self.assertIn("DaemonSleep", callback)
        self.assertIn("DaemonRestart", callback)
        self.assertIn("SetBackend", callback)
        self.assertIn("DaemonWake", callback)


if __name__ == "__main__":
    unittest.main()
