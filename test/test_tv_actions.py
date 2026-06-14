import unittest
import re
from pathlib import Path

SRC = Path("app/apps/app_ha/app_ha.cpp").read_text()

class TvActionTests(unittest.TestCase):
    def test_tv_actions_are_handled(self):
        for action in ["tv_power", "tv_vol_down", "tv_vol_up", "tv_mute", "tv_source"]:
            self.assertIn(action, SRC)

    def test_tv_services_are_mapped_to_media_player(self):
        for service in ["turn_on", "turn_off", "volume_down", "volume_up", "volume_mute", "select_source"]:
            self.assertIn(service, SRC)

    def test_tv_card_renders_sources(self):
        view = Path("app/apps/app_ha/view/view.cpp").read_text()
        tv_section = view.split("_build_tv_card", 1)[1].split("// ─── Build tab content", 1)[0]
        for src in ["HDMI 1", "HDMI 2", "HDMI 3"]:
            self.assertIn(src, view)
        for removed in ['"TV"', '"DTMB"', '"AV"']:
            self.assertNotIn(removed, tv_section)

    def test_media_cards_fit_single_view(self):
        view = Path("app/apps/app_ha/view/view.cpp").read_text()
        sonos_h = int(re.search(r"static constexpr int SONOS_H = (\d+);", view).group(1))
        tv_h = int(re.search(r"static constexpr int TV_H\s+= (\d+);", view).group(1))
        gap = int(re.search(r"static constexpr int GAP\s+= (\d+);", view).group(1))
        cont_h = int(re.search(r"static constexpr int CONT_H\s+= H - HDR_H - TAB_H - GAP \* 2;", view) and 720 - 80 - 68 - gap * 2)
        self.assertLessEqual(sonos_h + tv_h + gap, cont_h)

    def test_tv_power_button_avoids_missing_symbol(self):
        view = Path("app/apps/app_ha/view/view.cpp").read_text()
        tv_section = view.split("_build_tv_card", 1)[1].split("// ─── Build tab content", 1)[0]
        self.assertNotIn("LV_SYMBOL_POWER", tv_section)

    def test_tv_card_avoids_missing_chinese_glyphs(self):
        view = Path("app/apps/app_ha/view/view.cpp").read_text()
        tv_section = view.split("_build_tv_card", 1)[1].split("// ─── Build tab content", 1)[0]
        for text in ["已开启", "已关闭", "开启", "关闭"]:
            self.assertNotIn(text, tv_section)

    def test_media_cards_share_button_typography(self):
        view = Path("app/apps/app_ha/view/view.cpp").read_text()
        self.assertIn("_add_media_button_content", view)
        sonos_section = view.split("_build_sonos_card", 1)[1].split("// ─── TV card", 1)[0]
        tv_section = view.split("_build_tv_card", 1)[1].split("// ─── Build tab content", 1)[0]
        self.assertNotIn("lv_obj_set_style_text_font(txt", sonos_section)
        self.assertNotIn("lv_obj_set_style_text_font(txt", tv_section)

    def test_media_status_lines_are_smaller(self):
        view = Path("app/apps/app_ha/view/view.cpp").read_text()
        sonos_section = view.split("_build_sonos_card", 1)[1].split("// ─── TV card", 1)[0]
        tv_section = view.split("_build_tv_card", 1)[1].split("// ─── Build tab content", 1)[0]
        self.assertRegex(sonos_section, r"lv_obj_set_style_text_font\(cl, &font_zh_18, 0\)")
        self.assertRegex(tv_section, r"lv_obj_set_style_text_font\(source_lbl, &font_zh_18, 0\)")

    def test_tv_power_and_volume_labels(self):
        view = Path("app/apps/app_ha/view/view.cpp").read_text()
        sonos_section = view.split("_build_sonos_card", 1)[1].split("// ─── TV card", 1)[0]
        tv_section = view.split("_build_tv_card", 1)[1].split("// ─── Build tab content", 1)[0]
        for text in ['"音量-"', '"音量+"']:
            self.assertIn(text, sonos_section)
        for text in ['"开"', '"关"', '"音量-"', '"音量+"']:
            self.assertIn(text, tv_section)
        for text in ['"ON"', '"OFF"', '"音-"', '"音+"']:
            self.assertNotIn(text, sonos_section)
            self.assertNotIn(text, tv_section)

    def test_media_fonts_include_chinese_control_glyphs(self):
        for font_path in [
            Path("app/apps/app_ha/view/font_zh_18.c"),
            Path("app/apps/app_ha/view/font_zh_36.c"),
        ]:
            font = font_path.read_text()
            for glyph in ["开", "关", "音", "量", "源"]:
                self.assertIn(glyph, font, f"{font_path} is missing {glyph}")

if __name__ == "__main__":
    unittest.main()
