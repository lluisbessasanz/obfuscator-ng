#!/usr/bin/env python3
import http.server
import json
import random
import string
import threading
from queue import Queue, Empty
from pathlib import Path
import ssl
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

next_responses = Queue()
output = 0

def input_thread():

    while True:
        value = input(" ")
        if value.strip():
            next_responses.put(value)


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        pass  # disable logging

    def do_POST(self):
        global output
        length = int(self.headers.get("Content-Length", "0"))
        raw_body = self.rfile.read(length).decode("utf-8", errors="replace")

        try:
            request_data = json.loads(raw_body)
        except json.JSONDecodeError:
            request_data = {}

        if output and "message" in request_data.keys():
            print(base64.b64decode(request_data["message"]).decode(), end="")

        try:
            message = next_responses.get_nowait()
            output = 1
        except Empty:
            output = 0
            message = ""

        response = message + "\n"

        body = response.encode("utf-8")

        self.send_response(200)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main():

    if not CERT_FILE.exists() or not KEY_FILE.exists(): 
        raise FileNotFoundError( "Missing cert.pem or key.pem. Generate them with openssl first." )

    print("> ", end="")
    threading.Thread(target=input_thread, daemon=True).start()

    server = http.server.ThreadingHTTPServer((HOST, PORT), Handler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER) 
    context.load_cert_chain(certfile=str(CERT_FILE), keyfile=str(KEY_FILE)) 
    server.socket = context.wrap_socket(server.socket, server_side=True)

    server.serve_forever()


if __name__ == "__main__":
    main()
