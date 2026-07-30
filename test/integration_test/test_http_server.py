#!/usr/bin/env python3
# -*- coding: utf-8 -*-
import os
import logging
from multiprocessing import Process

from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import urlparse, parse_qs
from test_fixtures import TestFixtures


class SimpleHandler(BaseHTTPRequestHandler):
    """Serve a single page, echoing ?content=… if present."""

    def send_urandom(self, size: int) -> None:
        print("returning urandom")
        response = os.urandom(size)
        assert response
        assert isinstance(response, bytes)
        print(f"Server: sending {len(response)} bytes in response")
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(response)))
        self.end_headers()
        self.wfile.write(response)

    def do_GET(self) -> None:
        # Parse the request URL and its query string
        parsed = urlparse(self.path)
        query = parse_qs(parsed.query)

        print(f"Server: GET request received:{self.request}")
        print(f"Server: Path: {self.path}")
        print(f"Server: Query: {query}")
        print(f"Server: Headers: {self.headers}")
        print(f"Server: Client IP: {self.client_address}")

        if "content" in query:
            response_body = query["content"][0].encode("utf-8")
        elif "content_size" in query:
            self.send_urandom(int(query["content_size"][0]))
            return
        else:
            response_body = (
                TestFixtures.TEST_PAGE_BODY
                if isinstance(TestFixtures.TEST_PAGE_BODY, bytes)
                else TestFixtures.TEST_PAGE_BODY.encode("utf-8")
            )

        self.send_response(200)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(response_body)))
        self.end_headers()
        self.wfile.write(response_body)


def _run_server(port: int = 8080) -> None:
    """Start an HTTP server on the given port."""
    server_address = ("", port)  # bind to all interfaces
    httpd = HTTPServer(server_address, SimpleHandler)
    print(f"Serving HTTP on port {port} …")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down.")
        httpd.server_close()


def spawn_http_server(port: int = 8080) -> Process:
    """
    Launches our simple HTTP server in a separate process.
    """
    proc = Process(target=_run_server, args=(port,), daemon=False)
    proc.start()
    return proc


if __name__ == "__main__":
    _run_server(8080)
