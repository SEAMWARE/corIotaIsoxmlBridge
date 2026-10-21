#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# micsMock.py - the MICS for the tests: task data POSTed to it is appended to the log, a line each; a POST
# whose body has "REFUSE" in it is answered 400, the rest 200
#
#   micsMock.py --port <port> --log <file>
#
import argparse
from http.server import BaseHTTPRequestHandler, HTTPServer


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0))).decode()
        with open(self.server.log, "a") as f:
            f.write("POST %s %s %s\n" % (self.path, self.headers.get("Content-Type"), body.replace("\n", "\\n")))
        status = 400 if "REFUSE" in body else 200
        self.send_response(status)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--port", type=int, required=True)
    p.add_argument("--log", required=True)
    a = p.parse_args()
    s = HTTPServer(("127.0.0.1", a.port), Handler)
    s.log = a.log
    s.serve_forever()
