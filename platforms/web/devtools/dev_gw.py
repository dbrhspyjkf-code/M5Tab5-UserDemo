#!/usr/bin/env python3
"""dev_gw — 本地开发网关 (仅开发用, 无鉴权, 只绑 127.0.0.1)。

正式版网关是 hermes 仓库的 tab5_web_gateway (M2), 本脚本只服务 M0.2 联调:
  - 静态托管 build/web/bin (带 COOP/COEP, SharedArrayBuffer 需要);
  - /gw/prefs 读写 dev_prefs.json (设备 NVS 的替身);
  - /gw/<port>/<path> 反代家里局域网真实服务 (192.168.1.200 / 192.168.1.14);
  - /gw/ext/qt|ifzq 代理腾讯行情 (CORS)。

用法: python3 dev_gw.py [port]     默认 8080;  Dev 上游可用环境变量 TAB5_DEV_UPSTREAM 覆盖。
"""
import json
import os
import sys
import urllib.request
import urllib.error
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.abspath(__file__))
BIN_DIR = os.path.normpath(os.path.join(ROOT, "..", "..", "..", "build", "web", "bin"))
PREFS_PATH = os.path.join(ROOT, "dev_prefs.json")

UPSTREAM_HOST = os.environ.get("TAB5_DEV_UPSTREAM", "192.168.1.200")
GW_PORTS = {
    "8766": f"http://{UPSTREAM_HOST}:8766",   # hermes
    "8768": f"http://{UPSTREAM_HOST}:8768",   # email-status
    "8769": f"http://{UPSTREAM_HOST}:8769",   # radio relay
    "8123": f"http://{UPSTREAM_HOST}:8123",   # Home Assistant
    "8042": "http://192.168.1.14:8042",       # Reachy
    "8900": f"http://{UPSTREAM_HOST}:8900",   # iOS API
}
EXT = {
    "qt": "https://qt.gtimg.cn",
    "ifzq": "https://web.ifzq.gtimg.cn",
}

DEFAULT_PREFS = {
    "svc_host": UPSTREAM_HOST,
    "ha_host": UPSTREAM_HOST,
    "ha_token": os.environ.get("DEV_HA_TOKEN", ""),
    "reachy_host": "192.168.1.14",
    "theme_idx": "0",
    "wifi_ssid": "web",
    "screensaver_en": "0",
}


def load_prefs():
    if not os.path.exists(PREFS_PATH):
        save_prefs(DEFAULT_PREFS)
    try:
        with open(PREFS_PATH) as f:
            return json.load(f)
    except Exception:
        return dict(DEFAULT_PREFS)


def save_prefs(p):
    with open(PREFS_PATH, "w") as f:
        json.dump(p, f, ensure_ascii=False, indent=1)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("[gw] " + fmt % args + "\n")

    # ---- helpers ----
    def _sec_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")

    def _send(self, code, body, ctype, cache=False):
        self.send_response(code)
        self._sec_headers()
        self.send_header("Content-Type", ctype)
        if not cache:
            self.send_header("Cache-Control", "no-cache")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _proxy(self, base, path):
        url = base + path
        body = None
        if self.command in ("POST", "PUT"):
            n = int(self.headers.get("Content-Length") or 0)
            body = self.rfile.read(n) if n else b""
        req = urllib.request.Request(url, data=body, method=self.command)
        for h in ("Authorization", "Content-Type", "Accept"):
            v = self.headers.get(h)
            if v:
                req.add_header(h, v)
        # HA 客户端 token 为空时剥离空 Authorization, 避免上游 401 混乱
        if req.get_header("Authorization") in ("Bearer", "Bearer ", ""):
            req.remove_header("Authorization")
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                data = r.read()
                ctype = r.headers.get("Content-Type", "application/octet-stream")
                self._send(r.status, data, ctype, cache=True)
        except urllib.error.HTTPError as e:
            self._send(e.code, e.read(), e.headers.get("Content-Type", "text/plain"))
        except Exception as e:
            self._send(502, str(e).encode(), "text/plain")

    # ---- routes ----
    def do_GET(self):
        self.route()

    def do_POST(self):
        self.route()

    def do_PUT(self):
        self.route()

    def route(self):
        from urllib.parse import urlsplit
        raw = self.path
        path = urlsplit(self.path).path   # 剥掉 query, 否则 /?v=x 落入静态分支 404

        if path == "/gw/prefs" or raw.startswith("/gw/prefs?"):
            if self.command == "GET":
                q = raw.split("key=")
                prefs = load_prefs()
                if len(q) > 1:
                    key = q[1].split("&")[0]
                    val = prefs.get(key)
                    if val is None:
                        self._send(404, b"", "text/plain")
                    else:
                        self._send(200, str(val).encode(), "text/plain")
                else:
                    self._send(200, json.dumps(prefs).encode(), "application/json")
            else:
                n = int(self.headers.get("Content-Length") or 0)
                data = json.loads(self.rfile.read(n) or b"{}")
                prefs = load_prefs()
                prefs[data["key"]] = str(data.get("value", ""))
                save_prefs(prefs)
                self._send(200, b"ok", "text/plain")
            return

        if path.startswith("/gw/ext/"):
            rest = raw[len("/gw/ext/"):]   # raw: 保留 query
            name = rest.split("/", 1)[0]
            if name in EXT:
                self._proxy(EXT[name], rest[len(name):])
            else:
                self._send(404, b"unknown ext", "text/plain")
            return

        if path.startswith("/gw/"):
            rest = raw[len("/gw/"):]      # raw: 保留 query (id/w/h 等)
            from urllib.parse import urlsplit
            port = urlsplit(rest).path.split("/", 1)[0]
            sub = rest[len(port):] if "/" in rest else "/"
            if port in GW_PORTS:
                self._proxy(GW_PORTS[port], sub)
            else:
                self._send(404, f"no upstream for port {port}".encode(), "text/plain")
            return

        # 静态文件
        name = "index.html" if path == "/" else path.lstrip("/")
        fpath = os.path.normpath(os.path.join(BIN_DIR, name))
        if not fpath.startswith(BIN_DIR) or not os.path.isfile(fpath):
            self._send(404, b"not found", "text/plain")
            return
        ctype = {
            ".html": "text/html; charset=utf-8",
            ".js": "text/javascript",
            ".wasm": "application/wasm",
            ".json": "application/json",
            ".data": "application/octet-stream",
        }.get(os.path.splitext(fpath)[1], "application/octet-stream")
        # 开发模式全部 no-cache (wasm 迭代频繁, max-age 会污染实验)
        with open(fpath, "rb") as f:
            self._send(200, f.read(), ctype, cache=False)


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    print(f"dev gateway on http://127.0.0.1:{port}  bin={BIN_DIR}  upstream={UPSTREAM_HOST}")
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()


if __name__ == "__main__":
    main()
