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
             patch.object(hermes_main.aiohttp, "ClientSession", lambda: mock_session):
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


def test_missing_apikey_returns_500():
    """No MX_APIKEY → 500 with empty items."""
    async def run():
        _reset_cache()
        with patch.dict(os.environ, {}, clear=True):
            resp = await hermes_main.handle_stocks_portfolio(MagicMock())
            assert resp.status == 500
            body = json.loads(resp.body)
            assert "MX_APIKEY" in body["error"]
            assert body["items"] == []
    asyncio.run(run())


def test_upstream_error_returns_502():
    """aiohttp raises → 502 with empty items."""
    async def run():
        _reset_cache()
        mock_session = MagicMock()
        mock_session.post.side_effect = Exception("connection refused")
        with patch.dict(os.environ, {"MX_APIKEY": "test_key"}), \
             patch.object(hermes_main.aiohttp, "ClientSession", lambda: mock_session):
            resp = await hermes_main.handle_stocks_portfolio(MagicMock())
            assert resp.status == 502
            body = json.loads(resp.body)
            assert "upstream" in body["error"]
    asyncio.run(run())


if __name__ == "__main__":
    test_normalize_two_rows()
    print("test_normalize_two_rows OK")
    test_cache_hit_skips_upstream()
    print("test_cache_hit_skips_upstream OK")
    test_missing_apikey_returns_500()
    print("test_missing_apikey_returns_500 OK")
    test_upstream_error_returns_502()
    print("test_upstream_error_returns_502 OK")
    print("\nAll 4 tests passed.")

