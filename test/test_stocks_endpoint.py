"""Source-level tests for hermes stocks endpoint. Mock aiohttp to avoid hitting the real API."""
import asyncio
import json
import os
import sys
import time
from unittest.mock import patch, MagicMock, AsyncMock

# Make hermes_mcp_server importable as a package
sys.path.insert(0, os.path.expanduser("~/hermes-mcp-xiaozhi"))

from hermes_mcp_server import main as hermes_main  # noqa: E402


def _east_money_payload(items):
    return {
        "status": 0,
        "code": 0,
        "data": {
            "allResults": {
                "result": {
                    "columns": [{"key": k, "title": k} for k in [
                        "SECURITY_CODE", "SECURITY_SHORT_NAME", "NEWEST_PRICE",
                        "CHG", "PCHG", "010000_TURNOVER_RATE", "010000_LIANGBI"]],
                    "dataList": items,
                }
            }
        }
    }


def _row(code="600519", name="贵州茅台", price=1850.0, chg=2.78, pchg=50.0,
         turnover=0.35, liangbi=1.2):
    return {
        "SECURITY_CODE": code, "SECURITY_SHORT_NAME": name,
        "NEWEST_PRICE": price, "CHG": chg, "PCHG": pchg,
        "010000_TURNOVER_RATE": turnover, "010000_LIANGBI": liangbi,
    }


def _reset_cache():
    hermes_main._STOCKS_CACHE = {"ts": 0.0, "data": None}


def _conclusions(date="", items=None):
    return patch.object(
        hermes_main,
        "_load_latest_stock_conclusions",
        return_value=(date, items or {}),
        create=True,
    )


def test_normalize_two_rows():
    """Mock upstream with 2 rows; verify response has count=2 and flat items."""
    async def run():
        _reset_cache()
        payload = _east_money_payload([_row(), _row(code="300750", name="宁德时代",
                                                     price=380.0, chg=-1.25)])
        mock_resp = MagicMock()
        mock_resp.json = AsyncMock(return_value=payload)
        mock_session = MagicMock()
        mock_session.post = MagicMock()
        mock_session.post.return_value.__aenter__ = AsyncMock(return_value=mock_resp)
        mock_session.post.return_value.__aexit__ = AsyncMock(return_value=False)
        mock_session.__aenter__ = AsyncMock(return_value=mock_session)
        mock_session.__aexit__ = AsyncMock(return_value=False)
        with patch.dict(os.environ, {"MX_APIKEY": "test_key"}), \
             patch.object(hermes_main.aiohttp, "ClientSession", lambda: mock_session), \
             _conclusions():
            resp = await hermes_main.handle_stocks_portfolio(MagicMock())
            body = json.loads(resp.body)
            assert body["count"] == 2, f"expected 2 got {body['count']}"
            assert body["items"][0]["code"] == "600519"
            assert body["items"][0]["name"] == "贵州茅台"
            assert body["items"][0]["price"] == 1850.0
            assert body["items"][0]["chg"] == 2.78
            assert body["items"][1]["chg"] == -1.25
    asyncio.run(run())


def test_cache_hit_skips_upstream():
    """If cache is fresh, second call must not hit aiohttp."""
    async def run():
        _reset_cache()
        hermes_main._STOCKS_CACHE = {"ts": time.time(), "data":
            {"count": 1, "items": [{"code": "TEST", "name": "x", "price": 1,
                                     "chg": 0, "pchg": 0, "turnover": 0, "liangbi": 0}],
             "ts": int(time.time())}}
        # aiohttp must not be touched
        def fail(*a, **kw): raise AssertionError("upstream should not be called")
        with patch.object(hermes_main.aiohttp, "ClientSession", side_effect=fail):
            resp = await hermes_main.handle_stocks_portfolio(MagicMock())
            body = json.loads(resp.body)
            assert body["items"][0]["code"] == "TEST"
    asyncio.run(run())


def test_mx_quota_exhausted_uses_daily_stock_fallback():
    """MX quota error must fall back to daily_stock_analysis watchlist quotes."""
    async def run():
        _reset_cache()
        quota_payload = {
            "success": False,
            "status": 113,
            "code": 113,
            "message": "daily quota exhausted",
            "data": None,
        }
        watchlist_payload = {"stock_codes": ["688018", "601985"]}
        quote_payloads = {
            "688018": {
                "stock_code": "688018", "stock_name": "乐鑫科技",
                "current_price": 122.5, "change": -0.61,
                "change_percent": -0.5,
            },
            "601985": {
                "stock_code": "601985", "stock_name": "中国核电",
                "current_price": 9.03, "change": -0.05,
                "change_percent": -0.55,
            },
        }

        def response_cm(payload):
            response = MagicMock()
            response.raise_for_status = MagicMock()
            response.json = AsyncMock(return_value=payload)
            cm = MagicMock()
            cm.__aenter__ = AsyncMock(return_value=response)
            cm.__aexit__ = AsyncMock(return_value=False)
            return cm

        mock_session = MagicMock()
        mock_session.__aenter__ = AsyncMock(return_value=mock_session)
        mock_session.__aexit__ = AsyncMock(return_value=False)
        mock_session.post.return_value = response_cm(quota_payload)

        def mock_get(url, **kwargs):
            if url.endswith("/api/v1/stocks/watchlist"):
                return response_cm(watchlist_payload)
            code = url.rsplit("/", 2)[-2]
            return response_cm(quote_payloads[code])

        mock_session.get.side_effect = mock_get

        with patch.dict(os.environ, {"MX_APIKEY": "test_key"}), \
             patch.object(hermes_main.aiohttp, "ClientSession", lambda: mock_session), \
             _conclusions():
            resp = await hermes_main.handle_stocks_portfolio(MagicMock())
            body = json.loads(resp.body)
            assert resp.status == 200
            assert body["count"] == 2
            assert body["source"] == "daily_stock_analysis"
            assert body["items"][0] == {
                "code": "688018", "name": "乐鑫科技", "price": 122.5,
                "chg": -0.5, "pchg": -0.61, "turnover": 0.0, "liangbi": 0.0,
                "one_sentence": "", "analysis_date": "",
            }
    asyncio.run(run())


def test_missing_apikey_and_fallback_error_returns_502():
    """If MX is unconfigured and fallback is down, report both failures."""
    async def run():
        _reset_cache()
        mock_session = MagicMock()
        mock_session.__aenter__ = AsyncMock(return_value=mock_session)
        mock_session.__aexit__ = AsyncMock(return_value=False)
        mock_session.get.side_effect = Exception("fallback connection refused")
        with patch.dict(os.environ, {}, clear=True), \
             patch.object(hermes_main.aiohttp, "ClientSession", lambda: mock_session), \
             _conclusions():
            resp = await hermes_main.handle_stocks_portfolio(MagicMock())
            assert resp.status == 502
            body = json.loads(resp.body)
            assert "MX_APIKEY" in body["error"]
            assert "fallback connection refused" in body["error"]
            assert body["items"] == []
    asyncio.run(run())


def test_upstream_error_returns_502():
    """Both sources unavailable → 502 with empty items."""
    async def run():
        _reset_cache()
        mock_session = MagicMock()
        mock_session.__aenter__ = AsyncMock(return_value=mock_session)
        mock_session.__aexit__ = AsyncMock(return_value=False)
        mock_session.post.side_effect = Exception("connection refused")
        mock_session.get.side_effect = Exception("fallback connection refused")
        with patch.dict(os.environ, {"MX_APIKEY": "test_key"}), \
             patch.object(hermes_main.aiohttp, "ClientSession", lambda: mock_session), \
             _conclusions():
            resp = await hermes_main.handle_stocks_portfolio(MagicMock())
            assert resp.status == 502
            body = json.loads(resp.body)
            assert "mx: connection refused" in body["error"]
            assert "daily_stock_analysis: fallback connection refused" in body["error"]
    asyncio.run(run())


def test_latest_conclusions_are_merged_by_code():
    """Matching stocks get the latest sentence/date; unmatched stocks stay empty."""
    async def run():
        _reset_cache()
        payload = _east_money_payload([
            _row(),
            _row(code="300750", name="宁德时代", price=380.0, chg=-1.25),
        ])
        mock_resp = MagicMock()
        mock_resp.json = AsyncMock(return_value=payload)
        mock_session = MagicMock()
        mock_session.post.return_value.__aenter__ = AsyncMock(return_value=mock_resp)
        mock_session.post.return_value.__aexit__ = AsyncMock(return_value=False)
        mock_session.__aenter__ = AsyncMock(return_value=mock_session)
        mock_session.__aexit__ = AsyncMock(return_value=False)

        with patch.dict(os.environ, {"MX_APIKEY": "test_key"}), \
             patch.object(hermes_main.aiohttp, "ClientSession", lambda: mock_session), \
             _conclusions("2026-06-23", {
                 "600519": "多头结构保持，等待放量确认。",
             }):
            resp = await hermes_main.handle_stocks_portfolio(MagicMock())
            body = json.loads(resp.body)
            assert resp.status == 200
            assert body["analysis_date"] == "2026-06-23"
            assert body["items"][0]["one_sentence"] == "多头结构保持，等待放量确认。"
            assert body["items"][0]["analysis_date"] == "2026-06-23"
            assert body["items"][1]["one_sentence"] == ""
            assert body["items"][1]["analysis_date"] == ""
    asyncio.run(run())


def test_latest_conclusion_loader_preserves_leading_zero_codes():
    """The real loader uses the service Python and normalizes numeric codes."""
    assert hasattr(hermes_main, "_load_latest_stock_conclusions"), \
        "latest conclusion loader is missing"
    completed = MagicMock()
    completed.stdout = json.dumps({
        "date": "2026-06-23",
        "items": [{
            "code": 2050,
            "one_sentence": "等待趋势修复信号。",
        }],
    })
    with patch.object(hermes_main.subprocess, "run", return_value=completed) as run:
        date, conclusions = hermes_main._load_latest_stock_conclusions()
        assert date == "2026-06-23"
        assert conclusions == {"002050": "等待趋势修复信号。"}
        assert run.call_args.args[0][0] == hermes_main.sys.executable


def test_latest_conclusion_failure_keeps_quotes_available():
    """Analysis failures degrade to empty conclusions without failing quotes."""
    async def run():
        _reset_cache()
        payload = _east_money_payload([_row()])
        mock_resp = MagicMock()
        mock_resp.json = AsyncMock(return_value=payload)
        mock_session = MagicMock()
        mock_session.post.return_value.__aenter__ = AsyncMock(return_value=mock_resp)
        mock_session.post.return_value.__aexit__ = AsyncMock(return_value=False)
        mock_session.__aenter__ = AsyncMock(return_value=mock_session)
        mock_session.__aexit__ = AsyncMock(return_value=False)

        with patch.dict(os.environ, {"MX_APIKEY": "test_key"}), \
             patch.object(hermes_main.aiohttp, "ClientSession", lambda: mock_session), \
             patch.object(
                 hermes_main,
                 "_load_latest_stock_conclusions",
                 side_effect=RuntimeError("analysis unavailable"),
                 create=True,
             ):
            resp = await hermes_main.handle_stocks_portfolio(MagicMock())
            body = json.loads(resp.body)
            assert resp.status == 200
            assert body["items"][0]["price"] == 1850.0
            assert body["items"][0]["one_sentence"] == ""
            assert body["items"][0]["analysis_date"] == ""
    asyncio.run(run())


if __name__ == "__main__":
    test_normalize_two_rows()
    print("test_normalize_two_rows OK")
    test_cache_hit_skips_upstream()
    print("test_cache_hit_skips_upstream OK")
    test_mx_quota_exhausted_uses_daily_stock_fallback()
    print("test_mx_quota_exhausted_uses_daily_stock_fallback OK")
    test_missing_apikey_and_fallback_error_returns_502()
    print("test_missing_apikey_and_fallback_error_returns_502 OK")
    test_upstream_error_returns_502()
    print("test_upstream_error_returns_502 OK")
    test_latest_conclusions_are_merged_by_code()
    print("test_latest_conclusions_are_merged_by_code OK")
    test_latest_conclusion_loader_preserves_leading_zero_codes()
    print("test_latest_conclusion_loader_preserves_leading_zero_codes OK")
    test_latest_conclusion_failure_keeps_quotes_available()
    print("test_latest_conclusion_failure_keeps_quotes_available OK")
    print("\nAll 8 tests passed.")
