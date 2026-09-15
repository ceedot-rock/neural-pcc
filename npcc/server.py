"""POST /api/compress and POST /api/decompress."""
from __future__ import annotations
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs
from .api import compress, decompress
from .selector import Budget


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
        if urlparse(self.path).path == "/health":
            self._send(200, b"ok", "text/plain")
        else:
            self._send(404, b"not found", "text/plain")


def main(host="127.0.0.1", port=8080):
    httpd = ThreadingHTTPServer((host, port), Handler)
    print(f"npcc listening on http://{host}:{port}")
    httpd.serve_forever()


if __name__ == "__main__":
    main()
