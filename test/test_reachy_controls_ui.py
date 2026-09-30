import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "app/apps/app_reachy/app_reachy.h").read_text()
SOURCE = (ROOT / "app/apps/app_reachy/app_reachy.cpp").read_text()


class ReachyAudioControlTests(unittest.TestCase):
    def test_voice_page_groups_volume_mic_and_backend_as_cards(self):
        voice = SOURCE.split("void AppReachy::_buildVoicePage()", 1)[1]
        voice = voice.split("void AppReachy::_buildInteractionPage()", 1)[0]
        for text in ("音量", "麦克风", "后端与音色", "VAD 灵敏度"):
            self.assertIn(text, voice)
        self.assertGreaterEqual(voice.count("makeSurface"), 5)

    def test_audio_uses_two_sliders(self):
        voice = SOURCE.split("void AppReachy::_buildVoicePage()", 1)[1]
        voice = voice.split("void AppReachy::_buildInteractionPage()", 1)[0]
        self.assertGreaterEqual(voice.count("lv_slider_create"), 2)
        self.assertIn("lv_slider_set_range(_au_volume_slider, 0, 100)", voice)
        self.assertIn("lv_slider_set_range(_au_vad_slider, 1, 500)", voice)

    def test_sliders_submit_only_on_release(self):
        callback = SOURCE.split("void AppReachy::_audioEventCb", 1)[1]
        callback = callback.split("void AppReachy::_controlEventCb", 1)[0]
        self.assertIn("LV_EVENT_RELEASED", callback)
        self.assertIn("_queueOperation", callback)
        self.assertNotIn("reachy_client::", callback)

    def test_mic_and_mute_remain_explicit_buttons(self):
        for handle in ("_au_mic_btn", "_au_mute_btn"):
            self.assertIn(handle, HEADER)
            self.assertIn(f"lv_obj_add_event_cb({handle}", SOURCE)

    def test_network_writes_run_in_worker(self):
        queue = SOURCE.split("bool AppReachy::_queueOperation", 1)[1]
        queue = queue.split("reachy_client::OperationResult AppReachy::_executeOperation", 1)[0]
        self.assertNotIn("reachy_client::", queue)
        refresh = SOURCE.split("void AppReachy::_refreshActiveTab", 1)[1]
        refresh = refresh.split("void AppReachy::_refreshHeader", 1)[0]
        self.assertIn("_executeOperation", refresh)
        self.assertIn("tryRunDetached", refresh)

    def test_dangerous_actions_still_require_confirmation(self):
        callback = SOURCE.split("void AppReachy::_controlEventCb", 1)[1]
        callback = callback.split("void AppReachy::_videoToggleCb", 1)[0]
        self.assertIn("_confirmOperation", callback)
        for action in ("DaemonSleep", "DaemonRestart", "SetBackend"):
            self.assertIn(action, callback)
        confirm = SOURCE.split("void AppReachy::_confirmOperation", 1)[1]
        confirm = confirm.split("void AppReachy::_tabCb", 1)[0]
        self.assertRegex(confirm, r"makeButton\(dlg, \"确认\".*kError")

    def test_voice_dropdown_draw_layer_is_bounded(self):
        match = re.search(r"lv_obj_set_size\(_ct_voice_dropdown,\s*(\d+),\s*48\)", SOURCE)
        self.assertIsNotNone(match)
        self.assertLessEqual(int(match.group(1)) * 10 * 4, 16_000)


if __name__ == "__main__":
    unittest.main()
