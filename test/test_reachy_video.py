import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "app/apps/app_reachy/app_reachy.cpp").read_text()
HEADER = (ROOT / "app/apps/app_reachy/app_reachy.h").read_text()
CLIENT = (ROOT / "app/apps/app_reachy/reachy_client.h").read_text()
SDKCONFIG = (ROOT / "platforms/tab5/sdkconfig").read_text()
DEFAULTS = (ROOT / "platforms/tab5/sdkconfig.defaults").read_text()


class ReachyVideoTests(unittest.TestCase):
    def test_camera_destination_is_present(self):
        self.assertIn("Camera", HEADER)
        self.assertIn('"相机"', SOURCE)
        self.assertIn("_buildCameraPage()", SOURCE)

    def test_camera_frame_uses_existing_yrobot_cache_endpoint(self):
        self.assertIn('"/api/camera/frame"', CLIENT)
        self.assertIn("fetchCameraFrame", CLIENT)

    def test_preview_fetches_off_the_lvgl_thread_at_two_fps(self):
        self.assertIn("VIDEO_POLL_MS = 500", HEADER)
        self.assertIn("_video_fetch_inflight", HEADER)
        poll = SOURCE.split("void AppReachy::_pollVideoFrame", 1)[1]
        self.assertIn("tryRunDetached", poll)
        self.assertIn("fetchCameraFrame", poll)

    def test_preview_stops_when_leaving_destination_or_closing_app(self):
        select = SOURCE.split("void AppReachy::_selectDestination", 1)[1]
        select = select.split("void AppReachy::_refreshActiveTab", 1)[0]
        close = SOURCE.split("void AppReachy::onClose", 1)[1]
        close = close.split("void AppReachy::onRunning", 1)[0]
        self.assertIn("_stopVideoPreview", select)
        self.assertIn("_stopVideoPreview", close)

    def test_jpeg_decoder_supports_in_memory_frames(self):
        for config in (SDKCONFIG, DEFAULTS):
            self.assertIn("CONFIG_LV_USE_FS_MEMFS=y", config)
            self.assertIn("CONFIG_LV_USE_TJPGD=y", config)
            self.assertIn("CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192", config)


if __name__ == "__main__":
    unittest.main()
