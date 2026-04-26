#!/usr/bin/env python3
import http.server
import ssl
from pathlib import Path
import json
import base64

HOST = "0.0.0.0"
PORT = 443

## You need to build them with:
"""
openssl req -x509 -newkey rsa:2048 \
  -keyout key.pem \
  -out cert.pem \
  -days 365 \
  -nodes \
  -subj "/CN=localhost"
"""

CERT_FILE = Path("cert.pem")
KEY_FILE = Path("key.pem")


class Handler(http.server.SimpleHTTPRequestHandler):
    def do_POST(self):
        if self.path == "/":
            length = int(self.headers.get("Content-Length", "0"))
            raw_body = self.rfile.read(length).decode("utf-8", errors="replace")
            print(raw_body)
            json_body = json.loads(raw_body)
            print(base64.b64decode(json_body["message"]).decode(),end="")
            body = input()
            body += "\n"
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body.encode())))
            self.end_headers()
            self.wfile.write(body.encode())
            return

        super().do_POST()


def main():
    if not CERT_FILE.exists() or not KEY_FILE.exists():
        raise FileNotFoundError(
            "Missing cert.pem or key.pem. Generate them with openssl first."
        )

    server = http.server.ThreadingHTTPServer((HOST, PORT), Handler)

    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(certfile=str(CERT_FILE), keyfile=str(KEY_FILE))

    server.socket = context.wrap_socket(server.socket, server_side=True)

    server.serve_forever()


if __name__ == "__main__":
    main()
