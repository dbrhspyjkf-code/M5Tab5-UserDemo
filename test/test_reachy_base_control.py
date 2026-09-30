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

    def test_twenty_hz_pipelined_frame_loop(self):
        # Each HAL HTTP call opens a fresh TCP connection (90-170 ms RTT), so
        # a request-response loop starves the 250 ms server lease TTL. The
        # loop therefore pipelines: fire every 50 ms with up to three frames
        # in flight, renewing the server lease at a steady 50 ms cadence.
        self.assertIn("BASE_FRAME_MS = 50", HEADER)
        self.assertIn("BASE_MAX_INFLIGHT = 3", HEADER)
        self.assertIn("_base_frames_inflight", HEADER)
        running = SOURCE.split("void AppReachy::onRunning()", 1)[1]
        running = running.split("void AppReachy::_buildUI", 1)[0]
        self.assertIn("_base_frames_inflight.load() < BASE_MAX_INFLIGHT", running)
        self.assertIn("BASE_FRAME_MS", running)

    def test_deadman_release_zeroes_and_releases(self):
        body = function_body(SOURCE, "_baseEndDrive")
        self.assertIn("baseFrame(session, seq, 0.f, 0.f, false)", body)
        self.assertIn("baseStop()", body)
        self.assertIn("baseRelease(session)", body)
        self.assertIn("sendStop", body)
        # Touch-to-drive: the joystick press is the deadman and its release
        # must park immediately (single-pointer LVGL cannot do two-finger).
        joy = function_body(SOURCE, "_baseJoyCb")
        self.assertIn("LV_EVENT_PRESSED", joy)
        self.assertIn("_base_deadman = true", joy)
        self.assertIn("_baseStartAcquire(true)", joy)
        self.assertIn("LV_EVENT_PRESS_LOST", joy)
        self.assertIn("_baseEndDrive(false)", joy)

    def test_lease_loss_reacquires_while_deadman_held(self):
        # The server lease TTL is 250 ms while proxied frame RTT is 90-170 ms,
        # so jitter drops the lease; a held deadman must re-acquire (skipping
        # interlocks — the server is authoritative) instead of parking.
        running = SOURCE.split("void AppReachy::onRunning()", 1)[1]
        running = running.split("void AppReachy::_buildUI", 1)[0]
        self.assertIn("_base_frame_error_ready", running)
        self.assertIn("_base_deadman && _base_armed && _base_reacquire_fails < 3", running)
        self.assertIn("_baseStartAcquire(false)", running)
        acquire = function_body(SOURCE, "_baseStartAcquire")
        self.assertIn("if (interlocks)", acquire)
        self.assertIn("status.goto_on", acquire)

    def test_three_strikes_park_with_server_error(self):
        running = SOURCE.split("void AppReachy::onRunning()", 1)[1]
        running = running.split("void AppReachy::_buildUI", 1)[0]
        self.assertIn("已停车：「", running)
        tick = function_body(SOURCE, "_baseTickFrame")
        self.assertIn("_base_frame_error = result.error", tick)
        self.assertIn("_base_frame_scheduled_ms > 1200", running)
        self.assertIn("帧响应超时", running)

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

    def test_base_ops_use_keep_alive_transport(self):
        # Per-request TCP handshakes dominated the frame RTT and dropped the
        # short lease; the lease protocol must ride the persistent connection.
        for fn in ("baseAcquire", "baseFrame", "baseRelease", "baseStop"):
            body = function_body(CLIENT, fn)
            self.assertIn("httpPostKeepAlive", body)
        hal = (ROOT / "platforms/tab5/main/hal/components/hal_http.cpp").read_text()
        self.assertIn("httpPostKeepAlive", hal)
        self.assertIn("keep_alive_enable = true", hal)
        header = (ROOT / "app/hal/hal.h").read_text()
        self.assertIn("httpPostKeepAlive", header)

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

    def test_knob_does_not_eat_pad_touches(self):
        # LVGL9 base objects are CLICKABLE by default; the knob must let
        # presses fall through to the pad that owns the joystick events.
        build = SOURCE.split("void AppReachy::_buildBasePage()", 1)[1]
        build = build.split("void AppReachy::_buildMaintenancePage", 1)[0]
        self.assertIn("lv_obj_clear_flag(_ba_joy_knob, LV_OBJ_FLAG_CLICKABLE)", build)
        self.assertNotIn("_ba_deadman_btn", SOURCE)

    def test_driving_suppresses_swipe_up_exit(self):
        # Joystick-forward is a fast upward drag; LVGL reports it as a
        # gesture identical to the exit swipe — the app must not close while
        # the base page is driving.
        gesture = function_body(SOURCE, "_gestureCb")
        self.assertIn("Tab::Base", gesture)
        self.assertIn("_base_deadman || !self->_base_session.empty()", gesture)

    def test_background_tools_swipe_hook_ignores_foreign_screens(self):
        # AppSettings keeps its swipe-up hook installed while the cockpit is
        # foreground; its handler must not act unless its own screen shows.
        settings = (ROOT / "app/apps/app_settings/app_settings.cpp").read_text()
        self.assertIn("lv_screen_active() != app->_scr", settings)


if __name__ == "__main__":
    unittest.main()
