import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "app/apps/app_stocks/app_stocks.h"
SOURCE = ROOT / "app/apps/app_stocks/app_stocks.cpp"


class StocksRefreshPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.header = HEADER.read_text()
        cls.source = SOURCE.read_text()

    def test_open_always_requests_one_fetch(self):
        on_open = self.source.split("void AppStocks::onOpen()", 1)[1]
        on_open = on_open.split("void AppStocks::onRunning()", 1)[0]
        self.assertIn("_fetchStocksAsync();", on_open)

    def test_periodic_fetch_is_gated_by_market_hours(self):
        on_running = self.source.split("void AppStocks::onRunning()", 1)[1]
        on_running = on_running.split("void AppStocks::onClose()", 1)[0]
        self.assertIn("stocks_refresh::isAshareTradingTime(std::time(nullptr))", on_running)
        market_gate = on_running.index("stocks_refresh::isAshareTradingTime")
        interval_gate = on_running.index("FETCH_INTERVAL_MS")
        self.assertLess(market_gate, interval_gate)

    def test_async_fetch_has_atomic_in_flight_guard(self):
        self.assertRegex(
            self.header,
            r"std::atomic<bool>\s+_fetch_in_flight\s*\{false\};",
        )
        fetch = self.source.split("void AppStocks::_fetchStocksAsync()", 1)[1]
        fetch = fetch.split("void AppStocks::_doFetch()", 1)[0]
        self.assertIn("compare_exchange_strong", fetch)
        self.assertGreaterEqual(fetch.count("_fetch_in_flight.store(false)"), 2)


if __name__ == "__main__":
    unittest.main()
