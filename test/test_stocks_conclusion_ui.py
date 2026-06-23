import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "app/apps/app_stocks/app_stocks.h").read_text()
SOURCE = (ROOT / "app/apps/app_stocks/app_stocks.cpp").read_text()


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
