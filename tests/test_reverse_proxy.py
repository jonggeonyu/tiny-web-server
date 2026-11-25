#!/usr/bin/env python3
"""Loopback integration tests for tiny-web-server's optional /api reverse proxy.

Run with: make test-proxy
"""

import http.server
import http.client
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path


CONNECT_TIMEOUT = 2.0
RESPONSE_TIMEOUT = 10.0


def unused_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class BackendHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "FixtureBackend/1.0"

    def log_message(self, _format, *_args):
        pass

    def do_GET(self):
        self._respond()

    def do_HEAD(self):
        self._respond(head=True)

    def do_POST(self):
        self._respond()

    def _respond(self, head=False):
        if self.path == "/api/silent":
            # Leave the connection open without sending response bytes. The proxy
            # should apply its upstream response deadline and return a gateway error.
            time.sleep(8)
            return
        if self.path == "/api/malformed":
            self.connection.sendall(b"not an HTTP response\r\n\r\n")
            self.close_connection = True
            return
        if self.path == "/api/truncated":
            self.connection.sendall(
                b"HTTP/1.1 200 OK\r\nContent-Length: 80\r\n"
                b"Content-Type: application/octet-stream\r\nConnection: close\r\n\r\nshort"
            )
            self.close_connection = True
            return
        if self.path == "/api/close-delimited":
            self.connection.sendall(b"HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nclosed")
            self.close_connection = True
            return

        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        status = 201 if self.path.startswith("/api/status") else 200
        if self.path == "/api/missing":
            status = 404
        response_body = ("backend=%s\npath=%s\n" % (self.server.backend_name, self.path)).encode()
        response_body += body
        if self.path in ("/api/no-content", "/api/not-modified"):
            self.send_response_only(304 if self.path == "/api/not-modified" else 204)
            self.send_header("X-Backend", self.server.backend_name)
            self.end_headers()
            return
        self.send_response(status)
        self.send_header("X-Backend", self.server.backend_name)
        self.send_header("X-Request-Target", self.path)
        self.send_header("X-Observed-Host", self.headers["Host"])
        self.send_header("X-Observed-Local", self.headers.get("X-Request-Only", "absent"))
        self.send_header("Connection", "close, X-Backend-Only")
        self.send_header("X-Backend-Only", "upstream")
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(response_body)))
        self.end_headers()
        if not head:
            self.wfile.write(response_body)


class Backend(http.server.ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, name):
        self.backend_name = name
        super().__init__(("127.0.0.1", 0), BackendHandler)
        self.thread = threading.Thread(target=self.serve_forever, daemon=True)
        self.thread.start()

    @property
    def port(self):
        return self.server_address[1]

    def stop(self):
        self.shutdown()
        self.server_close()
        self.thread.join(timeout=2)


def request(port, method, target, body=b""):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=RESPONSE_TIMEOUT)
    try:
        headers = {"Host": "localhost", "Connection": "close"}
        if body:
            headers["Content-Type"] = "application/octet-stream"
        conn.request(method, target, body=body or None, headers=headers)
        response = conn.getresponse()
        response_body = response.read()
        response_headers = {key.lower().encode("ascii"): value.encode("latin-1")
                            for key, value in response.getheaders()}
        return response.status, response_headers, response_body
    finally:
        conn.close()


class ReverseProxyIntegration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if len(sys.argv) < 2:
            raise RuntimeError("usage: python3 test_reverse_proxy.py /path/to/build/tiny")
        cls.binary = str(Path(sys.argv[1]).resolve())
        if not os.path.isfile(cls.binary):
            raise RuntimeError("server executable not found: %s" % cls.binary)

        cls.backend_a = Backend("a")
        cls.addClassCleanup(cls.backend_a.stop)
        cls.backend_b = Backend("b")
        cls.addClassCleanup(cls.backend_b.stop)
        cls.tempdir = tempfile.TemporaryDirectory(prefix="tiny-proxy-e2e-")
        cls.addClassCleanup(cls.tempdir.cleanup)
        root = Path(cls.tempdir.name)
        (root / "hello.txt").write_bytes(b"static-file\x00bytes")
        cls.proxy_port = unused_port()
        cls.process = subprocess.Popen(
            [cls.binary, "-p", str(cls.proxy_port), "-r", str(root), "-t", "4", "-q", "16",
             "-b", "127.0.0.1:%d" % cls.backend_a.port,
             "-b", "127.0.0.1:%d" % cls.backend_b.port],
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
            cwd=str(root),
        )
        cls.addClassCleanup(cls._stop_process, cls.process)
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            if cls.process.poll() is not None:
                raise RuntimeError("tiny server exited during startup (status %s)" % cls.process.returncode)
            try:
                with socket.create_connection(("127.0.0.1", cls.proxy_port), timeout=0.1):
                    break
            except OSError:
                time.sleep(0.05)
        else:
            raise RuntimeError("tiny server did not start listening")

    @staticmethod
    def _stop_process(proc):
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=2)
        if proc.returncode != 0:
            raise AssertionError(f"tiny server shutdown exit status: {proc.returncode}")

    def test_proxy_forwards_binary_post_and_response_metadata(self):
        payload = bytes(range(256)) + b"\x00\xffend"
        status, headers, response = request(self.proxy_port, "POST", "/api/status?x=1&y=two", payload)
        self.assertEqual(status, 201)
        self.assertIn(headers[b"x-backend"], (b"a", b"b"))
        self.assertEqual(headers[b"x-request-target"], b"/api/status?x=1&y=two")
        backend = self.backend_a if headers[b"x-backend"] == b"a" else self.backend_b
        self.assertEqual(headers[b"x-observed-host"], f"127.0.0.1:{backend.port}".encode())
        self.assertNotIn(b"x-backend-only", headers)
        self.assertTrue(response.endswith(payload))
        self.assertTrue(response.startswith(b"backend=" + headers[b"x-backend"] + b"\n"))

    def test_backend_round_robin_is_sequential(self):
        first = request(self.proxy_port, "GET", "/api/rr-one")[1][b"x-backend"]
        second = request(self.proxy_port, "GET", "/api/rr-two")[1][b"x-backend"]
        third = request(self.proxy_port, "GET", "/api/rr-three")[1][b"x-backend"]
        self.assertNotEqual(first, second)
        self.assertEqual(first, third)

    def test_api_root_and_prefix_boundary(self):
        status, headers, body = request(self.proxy_port, "GET", "/api?raw=%2F")
        self.assertEqual(status, 200)
        self.assertEqual(headers[b"x-request-target"], b"/api?raw=%2F")
        self.assertIn(b"path=/api?raw=%2F\n", body)
        status, _headers, body = request(self.proxy_port, "GET", "/apix")
        self.assertEqual(status, 404)
        self.assertIn(b"404", body)

    def test_absolute_target_uses_configured_backend(self):
        status, headers, _ = request(self.proxy_port, "GET",
                                     "http://example.invalid/api/users?limit=2")
        self.assertEqual(status, 200)
        self.assertEqual(headers[b"x-request-target"], b"/api/users?limit=2")

    def test_existing_echo_and_regular_static_routes_remain_available(self):
        payload = b"echo\x00payload"
        status, _headers, body = request(self.proxy_port, "POST", "/echo", payload)
        self.assertEqual(status, 200)
        self.assertEqual(body, payload)
        status, _headers, body = request(self.proxy_port, "GET", "/hello.txt")
        self.assertEqual(status, 200)
        self.assertEqual(body, b"static-file\x00bytes")

    def test_head_and_bodyless_backend_statuses(self):
        status, headers, body = request(self.proxy_port, "HEAD", "/api/head")
        self.assertEqual(status, 200)
        self.assertIn(b"content-length", headers)
        self.assertEqual(body, b"")
        status, headers, body = request(self.proxy_port, "GET", "/api/no-content")
        self.assertEqual(status, 204)
        self.assertNotIn(b"content-length", headers)
        self.assertEqual(body, b"")
        status, _headers, body = request(self.proxy_port, "GET", "/api/not-modified")
        self.assertEqual(status, 304)
        self.assertEqual(body, b"")

    def test_close_delimited_body_and_backend_404(self):
        status, headers, body = request(self.proxy_port, "GET", "/api/close-delimited")
        self.assertEqual((status, body), (200, b"closed"))
        self.assertEqual(headers[b"content-length"], b"6")
        status, _headers, body = request(self.proxy_port, "GET", "/api/missing")
        self.assertEqual(status, 404)
        self.assertIn(b"path=/api/missing\n", body)

    def test_client_connection_is_reused(self):
        conn = http.client.HTTPConnection("127.0.0.1", self.proxy_port, timeout=RESPONSE_TIMEOUT)
        try:
            conn.request("GET", "/api/first",
                         headers={"Connection": "X-Request-Only", "X-Request-Only": "client"})
            response = conn.getresponse()
            self.assertEqual(response.status, 200)
            self.assertEqual(response.getheader("X-Observed-Local"), "absent")
            response.read()
            first_socket = conn.sock
            self.assertIsNotNone(first_socket, "proxy closed the client connection")
            conn.request("GET", "/api/second")
            response = conn.getresponse()
            self.assertEqual(response.status, 200)
            response.read()
            self.assertIs(conn.sock, first_socket)
        finally:
            conn.close()

    def test_unavailable_backend_returns_502(self):
        # A backend with a freshly released loopback port refuses connections.
        refused_port = unused_port()
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", refused_port))
        # This instance is intentionally not used in the running server's pool;
        # exercise refusal through a standalone one-backend server.
        self._assert_gateway_failure(["127.0.0.1:%d" % refused_port], "/api/refused", 502)

    def test_malformed_and_truncated_upstream_responses_return_502(self):
        # Keep pool selection deterministic by using the first backend in a
        # dedicated one-backend proxy for each malformed-response case.
        self._assert_gateway_failure(["127.0.0.1:%d" % self.backend_a.port], "/api/malformed", 502)
        self._assert_gateway_failure(["127.0.0.1:%d" % self.backend_a.port], "/api/truncated", 502)

    def test_silent_upstream_times_out_with_504(self):
        self._assert_gateway_failure(["127.0.0.1:%d" % self.backend_a.port], "/api/silent", 504,
                                     response_timeout=12.0)

    def _assert_gateway_failure(self, backends, target, expected, response_timeout=RESPONSE_TIMEOUT):
        # Launch an isolated proxy on an unused port; this avoids consuming a
        # backend selection from the round-robin server used by other tests.
        port = unused_port()
        with tempfile.TemporaryDirectory(prefix="tiny-proxy-case-") as root:
            proc = subprocess.Popen(
                [self.binary, "-p", str(port), "-r", root, "-t", "2", "-q", "8"]
                + sum((["-b", backend] for backend in backends), []),
                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                cwd=root,
            )
            try:
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    if proc.poll() is not None:
                        self.fail("isolated proxy exited during startup")
                    try:
                        with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                            break
                    except OSError:
                        time.sleep(0.03)
                else:
                    self.fail("isolated proxy did not start")
                conn = http.client.HTTPConnection("127.0.0.1", port, timeout=response_timeout)
                try:
                    conn.request("GET", target, headers={"Host": "localhost", "Connection": "close"})
                    response = conn.getresponse()
                    status = response.status
                    response.read()
                finally:
                    conn.close()
                self.assertEqual(status, expected)
            finally:
                self._stop_process(proc)


if __name__ == "__main__":
    # unittest interprets positional options itself; retain only its own flags.
    unittest.main(argv=[sys.argv[0]] + sys.argv[2:])
