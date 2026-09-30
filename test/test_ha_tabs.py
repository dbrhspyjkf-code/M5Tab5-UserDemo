import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
APP = (ROOT / "app/apps/app_ha/app_ha.cpp").read_text()
VIEW = (ROOT / "app/apps/app_ha/view/view.cpp").read_text()
HEADER = (ROOT / "app/apps/app_ha/view/view.h").read_text()


class HaTabsTests(unittest.TestCase):
    def test_only_lighting_page_remains(self):
        for label in ("设备", "影音", "家电"):
            self.assertNotIn(f'"{label}"', VIEW)
        self.assertNotIn("_build_tab_bar", VIEW)
        self.assertNotIn("_switch_tab", VIEW)
        self.assertNotIn("TabPage", HEADER)

    def test_removed_tab_components_are_deleted(self):
        for builder in (
            "_build_sensor_card", "_build_lock_card", "_build_fishtank_card",
            "_build_climate_card", "_build_vacuum_card", "_build_washer_card",
            "_build_printer_card", "_build_lenovo_printer_card", "_build_fan_card",
            "_build_sonos_card", "_build_tv_card", "_layout_device_tab",
            "_layout_appliance",
        ):
            self.assertNotIn(builder, VIEW)

        for field in ("is_sonos", "is_tv_player", "is_fan", "is_lock",
                      "is_fishtank", "is_climate", "is_vacuum", "is_washer",
                      "is_printer", "is_lenovo_printer"):
            self.assertNotIn(field, HEADER)

    def test_app_no_longer_builds_removed_tab_data(self):
        running = APP.split("void AppHA::onRunning()", 1)[1].split("void AppHA::onClose()", 1)[0]
        for vector in ("kitchen", "media", "sensors", "appliance"):
            self.assertNotIn(f"std::vector<ha_view::DeviceCard> {vector}", running)


if __name__ == "__main__":
    unittest.main()
