import unittest
from pathlib import Path


VIEW_H = Path("app/apps/app_ha/view/view.h").read_text()
VIEW = Path("app/apps/app_ha/view/view.cpp").read_text()
APP = Path("app/apps/app_ha/app_ha.cpp").read_text()


class HeaderAndLightsTests(unittest.TestCase):
    def test_light_cards_use_light_icon_not_home(self):
        living_section = APP.split("// ── Tab: 灯光", 1)[1].split("// ── Tab: 设备", 1)[0]
        self.assertNotIn("LV_SYMBOL_HOME", living_section)
        self.assertIn('"lightbulb"', living_section)
        self.assertNotIn('"灯"', living_section)

    def test_light_icon_is_drawn_as_bulb_not_text(self):
        self.assertIn("_add_lightbulb_icon", VIEW)
        self.assertIn('strcmp(icon_text, "lightbulb") == 0', VIEW)
        self.assertNotIn('strcmp(icon_text, "灯") == 0', VIEW)

    def test_weather_is_plain_header_text_without_background_card(self):
        header_section = VIEW.split("void HaView::_build_header()", 1)[1].split("void HaView::_build_tab_bar()", 1)[0]
        self.assertNotIn("_weather_card = _make_card", header_section)
        self.assertIn("_lbl_temp = lv_label_create(hdr)", header_section)
        self.assertIn("_lbl_weather = lv_label_create(hdr)", header_section)

    def test_header_accepts_tab5_battery_info(self):
        self.assertIn("struct BatteryInfo", VIEW_H)
        self.assertIn("const BatteryInfo& battery", VIEW_H)
        self.assertIn("updatePowerMonitorData()", APP)
        self.assertIn("_tab5_battery_from_power", APP)
        self.assertIn("LV_SYMBOL_BATTERY", VIEW)
        self.assertIn("_lbl_battery", VIEW_H)


if __name__ == "__main__":
    unittest.main()
