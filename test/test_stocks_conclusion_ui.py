import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "app/apps/app_stocks/app_stocks.h").read_text()
SOURCE = (ROOT / "app/apps/app_stocks/app_stocks.cpp").read_text()
ICON = (ROOT / "app/apps/app_settings/stocks_icon.c").read_text()


class StocksConclusionUiTests(unittest.TestCase):
    def test_stock_item_has_conclusion_fields(self):
        self.assertIn("std::string one_sentence;", HEADER)
        self.assertIn("std::string analysis_date;", HEADER)
        self.assertIn('it.value("one_sentence", std::string())', SOURCE)
        self.assertIn('it.value("analysis_date", std::string())', SOURCE)

    def test_rows_use_large_font_and_54px_height(self):
        self.assertRegex(HEADER, r"ROW_H\s*=\s*54")
        rows = SOURCE.split("// ── Data rows", 1)[1].split(
            "// ── Status bar", 1
        )[0]
        self.assertIn("zh_font_lg()", rows)
        self.assertIn("LV_STATE_PRESSED", rows)

    def test_title_headers_and_rows_use_larger_visual_sizes(self):
        title = SOURCE.split('lv_label_set_text(title, "自选股")', 1)[1]
        title = title.split("// ── Column header", 1)[0]
        self.assertIn("zh_font_lg()", title)
        self.assertIn("lv_obj_set_style_transform_scale(title, 307, 0)", title)

        columns = SOURCE.split("// ── Column header", 1)[1]
        columns = columns.split("// ── Data rows", 1)[0]
        self.assertIn("zh_font_lg()", columns)

        rows = SOURCE.split("// ── Data rows", 1)[1]
        rows = rows.split("// ── Status bar", 1)[0]
        self.assertIn("lv_obj_set_style_transform_scale(cell, 290, 0)", rows)

    def test_stocks_icon_uses_requested_png_asset(self):
        self.assertIn("Source: /Users/leenzhou/Downloads/ICONS/stock.png", ICON)
        self.assertIn(".w = 130, .h = 130", ICON)
        pixel_data = ICON.split("{", 1)[1]
        first_pixel = pixel_data.split(",", 4)[:4]
        self.assertEqual(
            [part.strip() for part in first_pixel],
            ["0xfe", "0xff", "0xff", "0xff"],
        )

    def test_detail_shows_only_conclusion_content(self):
        detail = SOURCE.split("void AppStocks::_showDetail", 1)[1]
        detail = detail.split("void AppStocks::_closeDetail", 1)[0]
        self.assertIn("s.one_sentence", detail)
        self.assertIn("s.analysis_date", detail)
        self.assertIn("暂无分析结论", detail)
        for forbidden in (
            "s.price", "s.chg", "s.pchg", "s.turnover", "s.liangbi"
        ):
            self.assertNotIn(forbidden, detail)


if __name__ == "__main__":
    unittest.main()
