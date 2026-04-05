#!/usr/bin/env python3
"""Simple HTTP server with COEP/COOP headers for WASM SharedArrayBuffer."""
import sys
import http.server

class CORSHandler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        super().end_headers()

port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
print(f"Serving on http://localhost:{port} with COEP/COOP headers")
http.server.HTTPServer(('', port), CORSHandler).serve_forever()
