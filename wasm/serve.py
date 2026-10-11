#!/usr/bin/env python3
"""
Multi-threaded HTTP server for the Mirror's Edge WebAssembly port.
Serves build-wasm/ static files plus:
  - GET /api/tree     -> JSON array of [rel_path_under_TdGame, file_size]
  - GET /game/<path>  -> Streams retail assets from <game_root>/TdGame/<path>
"""

import argparse
import json
import mimetypes
import os
import posixpath
import sys
import urllib.parse
from http import HTTPStatus
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

mimetypes.add_type("application/wasm", ".wasm")
mimetypes.add_type("application/javascript", ".js")


def build_tdgame_manifest(tdgame_dir: str) -> bytes:
    entries = []
    for sub in ("Config", "Localization", "CookedPC"):
        root_sub = os.path.join(tdgame_dir, sub)
        if not os.path.isdir(root_sub):
            continue
        for dirpath, _, filenames in os.walk(root_sub):
            for fn in filenames:
                if fn.startswith("."):
                    continue
                full = os.path.join(dirpath, fn)
                rel = os.path.relpath(full, tdgame_dir).replace(os.sep, "/")
                try:
                    sz = os.path.getsize(full)
                except OSError:
                    sz = 0
                entries.append([rel, sz])
    entries.sort(key=lambda item: item[0])
    return json.dumps(entries, separators=(",", ":")).encode("utf-8")


class MirrorsEdgeWasmHandler(SimpleHTTPRequestHandler):
    build_dir: str = ""
    tdgame_dir: str = ""
    manifest_bytes: bytes = b"[]"

    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=self.build_dir, **kwargs)

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        super().end_headers()

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        req_path = parsed.path

        if req_path in ("/", "/index.html"):
            self.path = "/mirrorsedge_wasm.html" + ("?" + parsed.query if parsed.query else "")
            return super().do_GET()

        if req_path == "/api/tree":
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(self.manifest_bytes)))
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            self.wfile.write(self.manifest_bytes)
            return

        if req_path.startswith("/game/"):
            rel = urllib.parse.unquote(req_path[len("/game/"):])
            clean_rel = posixpath.normpath("/" + rel).lstrip("/")
            full_path = os.path.realpath(os.path.join(self.tdgame_dir, clean_rel))
            td_real = os.path.realpath(self.tdgame_dir)
            if not full_path.startswith(td_real + os.sep) or not os.path.isfile(full_path):
                self.send_error(HTTPStatus.NOT_FOUND, "Asset not found")
                return
            try:
                file_size = os.path.getsize(full_path)
                self.send_response(HTTPStatus.OK)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(file_size))
                self.send_header("Cache-Control", "public, max-age=86400")
                self.end_headers()
                with open(full_path, "rb") as f:
                    while True:
                        chunk = f.read(1024 * 1024)
                        if not chunk:
                            break
                        self.wfile.write(chunk)
            except (BrokenPipeError, ConnectionResetError):
                pass
            return

        return super().do_GET()

    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path == "/api/screenshot":
            length = int(self.headers.get("Content-Length", "0"))
            data = self.rfile.read(length) if length > 0 else b""
            out_path = os.environ.get("ME_WASM_SCREENSHOT_OUT", "/tmp/wasm_gl_frame.png")
            with open(out_path, "wb") as f:
                f.write(data)
            self.send_response(HTTPStatus.OK)
            self.end_headers()
            return
        self.send_error(HTTPStatus.NOT_FOUND)


def main():
    parser = argparse.ArgumentParser(description="Serve Mirror's Edge WebAssembly build with live asset streaming")
    parser.add_argument("--build-dir", default="build-wasm", help="Directory containing mirrorsedge_wasm.html/.js/.wasm")
    parser.add_argument(
        "--game-root",
        default=os.environ.get("MEDGE_ME_INSTALL", "/Users/tomnom/mirrorsedge"),
        help="Path to retail Mirror's Edge install containing TdGame/CookedPC",
    )
    parser.add_argument("--host", default="127.0.0.1", help="Bind host (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=8080, help="Bind port (default: 8080)")
    args = parser.parse_args()

    tdgame_dir = os.path.join(args.game_root, "TdGame")
    if not os.path.isdir(os.path.join(tdgame_dir, "CookedPC")):
        print(f"[wasm/serve.py] WARNING: '{args.game_root}' does not contain TdGame/CookedPC (browser folder picker will be used).")
        manifest = b"[]"
    else:
        print(f"[wasm/serve.py] Indexing retail assets in {tdgame_dir} ...")
        manifest = build_tdgame_manifest(tdgame_dir)
        count = len(json.loads(manifest))
        print(f"[wasm/serve.py] Indexed {count} files ready for zero-copy streaming.")

    MirrorsEdgeWasmHandler.build_dir = os.path.abspath(args.build_dir)
    MirrorsEdgeWasmHandler.tdgame_dir = os.path.abspath(tdgame_dir)
    MirrorsEdgeWasmHandler.manifest_bytes = manifest

    class HighBacklogThreadingServer(ThreadingHTTPServer):
        allow_reuse_address = True
        request_queue_size = 256

    server = HighBacklogThreadingServer((args.host, args.port), MirrorsEdgeWasmHandler)
    print(f"[wasm/serve.py] Serving Mirror's Edge WASM at http://{args.host}:{args.port}/")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[wasm/serve.py] Shutting down.")


if __name__ == "__main__":
    main()
