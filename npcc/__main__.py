import sys
from pathlib import Path


def main(argv):
    if len(argv) >= 2 and argv[1] == "serve":
        host = argv[2] if len(argv) > 2 else "127.0.0.1"
        port = int(argv[3]) if len(argv) > 3 else 8080
        from .server import main as serve
        serve(host, port)
        return 0
    if len(argv) < 3 or argv[1] not in {"c", "d"}:
        print(
            "usage: python -m npcc c|d INPUT [OUTPUT]\n"
            "       python -m npcc serve [host] [port]",
            file=sys.stderr,
        )
        return 2
    from .api import compress, decompress
    data = Path(argv[2]).read_bytes()
    out = compress(data) if argv[1] == "c" else decompress(data)
    if len(argv) >= 4:
        Path(argv[3]).write_bytes(out)
    else:
        sys.stdout.buffer.write(out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
