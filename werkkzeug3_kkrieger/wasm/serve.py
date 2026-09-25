#!/usr/bin/env python3
"""Dev server for wasm/dist: like `python -m http.server` but never lets the
browser cache anything, so every reload picks up the freshly linked build."""
import http.server, sys, os

class H(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header('Cache-Control', 'no-store, must-revalidate')
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin')      # for a future pthread build
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        super().end_headers()
    def log_message(self, fmt, *args):
        pass

port = int(sys.argv[1]) if len(sys.argv) > 1 else 8766
# Resolve dist/ per request instead of chdir-ing into it: build.sh clean
# recreates the directory, which would otherwise leave us with a dead cwd.
# Second argument picks another output directory (e.g. dist_release).
which = sys.argv[2] if len(sys.argv) > 2 else 'dist'
DIST = os.path.join(os.path.dirname(os.path.abspath(__file__)), which)
import functools
http.server.ThreadingHTTPServer(('127.0.0.1', port), functools.partial(H, directory=DIST)).serve_forever()
