"""POST /api/compress and POST /api/decompress.

Discovery: GET /, GET /service, GET /about, GET /endpoints (all JSON).
"""
from __future__ import annotations
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs
from .api import compress, decompress
from .selector import Budget

NPCC_API_VERSION = "0.1.0"  # mirrors NPCC_VERSION in src/main.c

SERVICE = {
    "service": "neural-pcc",
    "version": NPCC_API_VERSION,
    "license": "AGPL-3.0-or-later OR commercial (see LICENSE)",
}

ENDPOINTS = [
    {"method": "GET", "path": "/", "desc": "Service summary (JSON)"},
    {"method": "GET", "path": "/service", "desc": "Service summary (JSON)"},
    {"method": "GET", "path": "/about", "desc": "What this service is (JSON)"},
    {"method": "GET", "path": "/endpoints", "desc": "Machine-readable endpoint list (JSON)"},
    {"method": "GET", "path": "/health", "desc": "Liveness probe (text)"},
    {
        "method": "POST",
        "path": "/api/compress",
        "desc": "Compress raw bytes; ?budget=classical|ar|full (default full)",
    },
    {"method": "POST", "path": "/api/decompress", "desc": "Decompress an npcc blob"},
]

ABOUT = {
    **SERVICE,
    "description": (
        "Neural-PCC (TNSSRC) compression over HTTP. TriNeural Shared Spine Row "
        "Compression — lossless, CLI-first codec with a Python API. "
        "Pathway laws live in cuni/ (CuNi: same stdout on every catalog seat, or refuse)."
    ),
    "repository": "https://github.com/ceedot-rock/neural-pcc",
    "contact": "corey@slidphilabs.com",
}


def _json(obj) -> bytes:
    return json.dumps(obj, indent=2).encode()


def _budget(qs: dict) -> Budget:
    mode = (qs.get("budget") or ["full"])[0]
    if mode == "classical":
        return Budget(allow_neural=False, allow_search=False)
    if mode == "ar":
        return Budget(allow_neural=True, allow_search=False)
    return Budget()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        print("[npcc]", fmt % args)

    def _read_body(self) -> bytes:
        n = int(self.headers.get("Content-Length", "0"))
        return self.rfile.read(n)

    def _send(self, code: int, data: bytes, ctype: str):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        u = urlparse(self.path)
        qs = parse_qs(u.query)
        body = self._read_body()
        try:
            if u.path == "/api/compress":
                out = compress(body, _budget(qs))
                self._send(200, out, "application/octet-stream")
            elif u.path == "/api/decompress":
                out = decompress(body)
                self._send(200, out, "application/octet-stream")
            else:
                self._send(404, b"not found", "text/plain")
        except Exception as e:
            self._send(400, str(e).encode(), "text/plain")

    def do_GET(self):
        path = urlparse(self.path).path
        if path == "/health":
            self._send(200, b"ok", "text/plain")
        elif path in ("/", "/service"):
            self._send(200, _json(SERVICE), "application/json")
        elif path == "/about":
            self._send(200, _json(ABOUT), "application/json")
        elif path == "/endpoints":
            self._send(200, _json({"endpoints": ENDPOINTS}), "application/json")
        else:
            self._send(404, b"not found", "text/plain")


def main(host="127.0.0.1", port=8080):
    httpd = ThreadingHTTPServer((host, port), Handler)
    print(f"npcc listening on http://{host}:{port}")
    httpd.serve_forever()


if __name__ == "__main__":
    main()
