import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CLIENT = (ROOT / "app/apps/app_reachy/reachy_client.h").read_text()
HEADER = (ROOT / "app/apps/app_reachy/app_reachy.h").read_text()
SOURCE = (ROOT / "app/apps/app_reachy/app_reachy.cpp").read_text()


def function_body(source: str, name: str) -> str:
    match = re.search(rf"(?:inline\s+)?[^\n]*\b{name}\s*\([^)]*\)\s*\{{", source)
    if not match:
        return ""
    start = match.end()
    depth = 1
    pos = start
    while pos < len(source) and depth:
        depth += (source[pos] == "{") - (source[pos] == "}")
        pos += 1
    return source[start:pos - 1]


class ReachyBaseControlTests(unittest.TestCase):
    def test_fixed_proxy_routes_only(self):
        for route in ("/api/mobile-base/remote/status",
                      "/api/mobile-base/remote/acquire",
                      "/api/mobile-base/remote/frame",
                      "/api/mobile-base/remote/release",
                      "/api/mobile-base/remote/stop",
                      "/api/mobile-base/goto-mode",
                      "/api/mobile-base/gamepad-mode"):
            self.assertIn(route, CLIENT)

    def test_tab5_never_holds_gateway_credentials(self):
        for forbidden in (":8788", "/v1/mobile-base", "X-Auth-Signature",
                          "X-Auth-Nonce", "X-Auth-Timestamp"):
            self.assertNotIn(forbidden, CLIENT)
            self.assertNotIn(forbidden, SOURCE)

    def test_frame_schema_mirrors_server_contract(self):
        body = function_body(CLIENT, "baseFrame")
        for key in ('{"session_id", session_id', '{"sequence", sequence',
                    '{"linear_x", linear_x', '{"angular_z", angular_z',
                    '{"deadman", deadman'):
            self.assertIn(key, body)
        self.assertIn("sequence == 0", body)
        self.assertIn("!deadman && (linear_x != 0.f || angular_z != 0.f)", body)

    def test_velocity_caps_match_ios_joystick(self):
        self.assertIn("BASE_MAX_LINEAR_X  = 0.12f", CLIENT)
        self.assertIn("BASE_MAX_ANGULAR_Z = 0.45f", CLIENT)
        tick = function_body(SOURCE, "_baseTickFrame")
        self.assertIn("_base_lin * reachy_client::BASE_MAX_LINEAR_X", tick)
        self.assertIn("-_base_ang * reachy_client::BASE_MAX_ANGULAR_Z", tick)

    def test_twenty_hz_single_flight_frame_loop(self):
        self.assertIn("BASE_FRAME_MS = 50", HEADER)
        self.assertIn("_base_frame_inflight", HEADER)
        running = SOURCE.split("void AppReachy::onRunning()", 1)[1]
        running = running.split("void AppReachy::_buildUI", 1)[0]
        self.assertIn("_base_frame_inflight.load()", running)
        self.assertIn("BASE_FRAME_MS", running)

    def test_deadman_release_zeroes_and_releases(self):
        body = function_body(SOURCE, "_baseEndDrive")
        self.assertIn("baseFrame(session, seq, 0.f, 0.f, false)", body)
        self.assertIn("baseStop()", body)
        self.assertIn("baseRelease(session)", body)
        self.assertIn("sendStop", body)
        deadman = function_body(SOURCE, "_baseDeadmanCb")
        self.assertIn("LV_EVENT_PRESSED", deadman)
        self.assertIn("_baseEndDrive(false)", deadman)
        self.assertIn("LV_EVENT_PRESS_LOST", SOURCE)

    def test_leaving_page_or_closing_app_parks_the_base(self):
        select = SOURCE.split("void AppReachy::_selectDestination", 1)[1]
        select = select.split("void AppReachy::_refreshActiveTab", 1)[0]
        close = SOURCE.split("void AppReachy::onClose", 1)[1]
        close = close.split("void AppReachy::onRunning", 1)[0]
        self.assertIn("_baseEndDrive(false)", select)
        self.assertIn("_baseEndDrive(false)", close)

    def test_acquire_is_interlock_gated_and_fail_closed(self):
        body = function_body(SOURCE, "_baseStartAcquire")
        for gate in ("status.goto_on", "status.gamepad_on",
                     "!status.goto_known", "!status.gamepad_known"):
            self.assertIn(gate, body)
        self.assertIn("baseRelease(lease.session_id)", body)

    def test_frames_run_off_the_lvgl_thread(self):
        tick = function_body(SOURCE, "_baseTickFrame")
        self.assertIn("tryRunDetached", tick)
        self.assertIn("reachy_client::baseFrame", tick)

    def test_emergency_stop_is_stop_only(self):
        stop = function_body(CLIENT, "baseStop")
        self.assertIn("/api/mobile-base/remote/stop", stop)
        self.assertIn('"{}"', stop)
        ui_stop = function_body(SOURCE, "_baseStopCb")
        self.assertIn("_baseEndDrive(true)", ui_stop)

    def test_base_page_exists_in_dock(self):
        self.assertIn('"底盘"', SOURCE)
        self.assertIn("_buildBasePage()", SOURCE)
        self.assertIn("Camera, Base, Maintenance, Count", HEADER)
        self.assertIn("Tab::Base", SOURCE)


if __name__ == "__main__":
    unittest.main()
