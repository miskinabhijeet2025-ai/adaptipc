#!/usr/bin/env python3
"""
serve_dashboard.py -- zero-dependency static HTTP server for the AdaptIPC
research showcase dashboard.

Usage:
    python3 showcase/scripts/serve_dashboard.py [--port 8000]

Serves files from the project root so that showcase/dashboard/index.html can
load ../assets/architecture.png, ../assets/decision_pipeline.png,
../assets/figures/*.png, and ../outputs/decision_log.jsonl via fetch().
"""
import argparse
import http.server
import socketserver
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[2]


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(PROJECT_ROOT), **kwargs)

    def end_headers(self):
        # Disable caching for showcase assets so freshly regenerated
        # figures / telemetry are immediately visible.
        self.send_header("Cache-Control", "no-store, no-cache, must-revalidate")
        super().end_headers()

    def log_message(self, fmt, *args):  # quieter logs
        sys.stderr.write("[serve] %s - %s\n" % (self.address_string(), fmt % args))


def main() -> int:
    parser = argparse.ArgumentParser(description="AdaptIPC showcase static server")
    parser.add_argument("--port", type=int, default=8000)
    args = parser.parse_args()

    socketserver.TCPServer.allow_reuse_address = True
    try:
        with socketserver.TCPServer(("", args.port), Handler) as httpd:
            url = f"http://localhost:{args.port}/showcase/dashboard/index.html"
            print("=" * 60)
            print(" AdaptIPC Showcase Dashboard")
            print("=" * 60)
            print(f" Serving:  {PROJECT_ROOT}")
            print(f" Open:     {url}")
            print(f" Stop:     Ctrl-C")
            print("=" * 60)
            httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n[serve] Shutdown requested.")
    except OSError as exc:
        print(f"[serve] ERROR: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
