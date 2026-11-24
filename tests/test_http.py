import contextlib
import email.utils
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from test_tcp_echo import HOST, connect_when_ready

GET_KEEP_ALIVE = b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"
POST_KEEP_ALIVE = b"POST /api/echo HTTP/1.1\r\nHost: localhost\r\n"
GET = GET_KEEP_ALIVE[:-2] + b"Connection: close\r\n\r\n"
POST = POST_KEEP_ALIVE + b"Connection: close\r\n"
HEADER_LIMIT = 16 * 1024
LARGE_FILE_SIZE = 8 * 1024 * 1024


def read_message(stream, expected, closing=False):
    start = stream.readline()
    assert start.startswith(f"HTTP/1.1 {expected} ".encode()), start
    headers = {}
    while (line := stream.readline()) != b"\r\n":
        assert line.endswith(b"\r\n"), line
        name, value = line.split(b":", 1)
        headers[name.lower()] = value.strip()
    if closing is not None:
        assert (headers.get(b"connection", b"").lower() == b"close") == closing, headers
    date = email.utils.parsedate_to_datetime(headers[b"date"].decode("ascii"))
    assert date.utcoffset().total_seconds() == 0
    length = int(headers[b"content-length"])
    body = stream.read(length)
    assert len(body) == length, (len(body), length)
    return headers, body


def read_response(client, expected):
    with client.makefile("rb") as stream:
        result = read_message(stream, expected, closing=True)
        assert stream.read(1) == b"", "expected one response followed by EOF"
        return result


def connect(address, timeout=5):
    return socket.create_connection(address, timeout=timeout)


def request(address, raw, status):
    with connect(address) as client:
        client.sendall(raw)
        return read_response(client, status)


def incomplete_request_is_waiting(client):
    client.settimeout(0.15)
    try:
        client.recv(1)
    except socket.timeout:
        pass
    else:
        raise AssertionError("server responded before the request was complete")
    finally:
        client.settimeout(5)


def check_headers(address):
    headers, body = request(address, GET, 200)
    assert body == b"<h1>Hello</h1>" and headers[b"content-type"] == b"text/html"
    for raw, status in [
        (b"GET / HTTP/1.1\r\n\r\n", 400),
        (b"GET / HTTP/1.1\r\nHost: localhost\r\nhost: localhost\r\n\r\n", 400),
        (GET[:-2] + b"Content-Length: abc\r\n\r\n", 400),
        (GET[:-2] + b"Content-Length: 0\r\nContent-Length: 0\r\n\r\n", 400),
        (GET[:-2] + b"Content-Length: 2\r\n\r\n", 400),
        (GET[:-2] + b"Transfer-Encoding: chunked\r\n\r\n", 501),
        (GET[:-2] + b"Transfer-Encoding: gzip\r\n\r\n", 400),
        (GET[:-2] + b"Transfer-Encoding: chunked\r\nContent-Length: 0\r\n\r\n", 400),
        (GET.replace(b"GET ", b"PATCH "), 501),
        (GET.replace(b"HTTP/1.1", b"HTTP/2.0"), 505),
    ]:
        request(address, raw, status)

    assert request(address, GET[:-2] + b"Content-Length: 0\r\n\r\n", 200)[1] == body

    with connect(address) as client:
        client.sendall(GET[:-1])
        incomplete_request_is_waiting(client)
        assert request(address, GET, 200)[1] == body
        client.sendall(GET[-1:])
        assert read_response(client, 200)[1] == body

    prefix = GET[:-2] + b"X-Padding: "
    exact = prefix + b"x" * (HEADER_LIMIT - len(prefix) - 4) + b"\r\n\r\n"
    assert len(exact) == HEADER_LIMIT
    assert request(address, exact, 200)[1] == body
    request(address, exact[:-1] + b"x", 431)

    with connect(address) as client:
        client.sendall(GET[:-1])
        client.shutdown(socket.SHUT_WR)
        read_response(client, 400)
    with connect(address) as client:
        client.shutdown(socket.SHUT_WR)
        assert client.recv(1) == b""
    print("PASS: HTTP header fragmentation, framing, concurrent GET, 16 KiB boundary, and EOF")


def check_post(address):
    for raw, status in [
        (POST + b"\r\n", 411),
        (POST + b"Content-Length: 1048577\r\n\r\n", 413),
        (POST + b"Content-Length: 999999999999999999999999999999\r\n\r\n", 413),
    ]:
        request(address, raw, status)
    assert request(address, POST + b"Content-Length: 0\r\n\r\n", 200)[1] == b""

    payload = bytes(range(256)) * 20
    headers = POST + f"Content-Length: {len(payload)}\r\n\r\n".encode()
    assert request(address, headers + payload, 200)[1] == payload
    with connect(address) as client:
        client.sendall(headers + payload[:3])
        incomplete_request_is_waiting(client)
        client.sendall(payload[3:])
        assert read_response(client, 200)[1] == payload
    for body in (b"", payload[:3]):
        with connect(address) as client:
            client.sendall(headers + body)
            client.shutdown(socket.SHUT_WR)
            read_response(client, 400)

    exact_limit = b"x" * (1024 * 1024)
    with connect(address) as client:
        client.sendall(POST + b"Content-Length: 1048576\r\n\r\n")
        client.sendall(exact_limit)
        assert read_response(client, 200)[1] == exact_limit

    headers, _ = request(address, GET.replace(b" / ", b" /api/echo "), 405)
    assert headers[b"allow"] == b"POST"
    print("PASS: POST binary echo, length errors, fragmented body, 1 MiB boundary, and method routing")


def check_files(address, files):
    for name, (payload, mime) in files.items():
        target = b"/" + name.encode() + b"?ignored=1"
        headers, body = request(address, GET.replace(b" / ", b" " + target + b" "), 200)
        assert headers[b"content-type"] == mime, headers
        assert body == payload, name
    request(address, GET.replace(b" / ", b" /missing "), 404)
    request(address, GET.replace(b" / ", b" /symlink "), 403)
    request(address, GET.replace(b" / ", b" /symlink-dir/index.html "), 403)
    print("PASS: static file bytes, MIME, missing file, and symlink policy")


def check_slow_reader(address):
    with socket.socket() as slow:
        slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 16384)
        slow.settimeout(5)
        slow.connect(address)
        slow.sendall(GET.replace(b" / ", b" /large.bin "))
        assert slow.recv(1, socket.MSG_PEEK) == b"H"
        assert request(address, GET, 200)[1] == b"<h1>Hello</h1>"
        assert read_response(slow, 200)[1] == bytes(LARGE_FILE_SIZE)
    print("PASS: another HTTP response completes while a large response is unread")


def check_timeouts(address):
    with contextlib.ExitStack() as connections:
        idle = connections.enter_context(connect(address, timeout=8))
        idle_stream = connections.enter_context(idle.makefile("rb"))
        reused = connections.enter_context(connect(address, timeout=14))
        reused_stream = connections.enter_context(reused.makefile("rb"))
        header = connections.enter_context(connect(address, timeout=14))
        body = connections.enter_context(connect(address, timeout=14))

        idle.sendall(GET_KEEP_ALIVE)
        idle_start = time.monotonic()
        read_message(idle_stream, 200)
        reused.sendall(GET_KEEP_ALIVE)
        read_message(reused_stream, 200)
        reused.sendall(GET_KEEP_ALIVE[:-1])
        header.sendall(GET[:-1])
        body.sendall(POST + b"Content-Length: 4\r\n\r\na")

        assert idle_stream.read(1) == b"", "expected silent EOF on an idle connection"
        assert time.monotonic() - idle_start >= 4, "idle connection closed too early"
        read_message(reused_stream, 408, closing=True)
        assert reused_stream.read(1) == b""
        read_response(header, 408)
        read_response(body, 408)
        request(address, GET, 200)
    print("PASS: silent keep-alive idle timeout, reused and initial request timeouts")


def check_keep_alive(address):
    css = GET_KEEP_ALIVE.replace(b" / ", b" /site.css ")
    last = css[:-2] + b"CoNnEcTiOn: Keep-Alive, ClOsE\r\n\r\n"
    with connect(address) as client, client.makefile("rb") as stream:
        client.sendall(GET_KEEP_ALIVE)
        assert read_message(stream, 200)[1] == b"<h1>Hello</h1>"
        client.sendall(css[:-2] + b"Connection: keep-alive\r\n\r\n")
        assert read_message(stream, 200)[1] == b"body {}"
        client.sendall(POST_KEEP_ALIVE + b"Content-Length: 0\r\n\r\n")
        assert read_message(stream, 200)[1] == b""
        client.sendall(last)
        assert read_message(stream, 200, closing=True)[1] == b"body {}"
        assert stream.read(1) == b""

    large = GET_KEEP_ALIVE.replace(b" / ", b" /large.bin ")
    with connect(address) as client, client.makefile("rb") as stream:
        client.sendall(large + last)
        assert read_message(stream, 200)[1] == bytes(LARGE_FILE_SIZE)
        assert read_message(stream, 200, closing=True)[1] == b"body {}"
        assert stream.read(1) == b""

    post = POST_KEEP_ALIVE + b"Content-Length: 5\r\n\r\n"
    with connect(address) as client, client.makefile("rb") as stream:
        client.sendall(post + b"hello" + css[:-1])
        assert read_message(stream, 200)[1] == b"hello"
        client.sendall(css[-1:])
        assert read_message(stream, 200)[1] == b"body {}"
        client.sendall(post + b"he")
        incomplete_request_is_waiting(client)
        client.sendall(b"llo" + last)
        assert read_message(stream, 200)[1] == b"hello"
        assert read_message(stream, 200, closing=True)[1] == b"body {}"
        assert stream.read(1) == b""

    with connect(address) as client, client.makefile("rb") as stream:
        client.sendall(GET_KEEP_ALIVE + css)
        client.shutdown(socket.SHUT_WR)
        assert read_message(stream, 200)[1] == b"<h1>Hello</h1>"
        assert read_message(stream, 200, closing=None)[1] == b"body {}"
        assert stream.read(1) == b""

    with connect(address) as client, client.makefile("rb") as stream:
        client.sendall(last + GET_KEEP_ALIVE * 256)
        client.shutdown(socket.SHUT_WR)
        assert read_message(stream, 200, closing=True)[1] == b"body {}"
        assert stream.read(1) == b""

    with connect(address) as client, client.makefile("rb") as stream:
        client.sendall(GET_KEEP_ALIVE + b"GET / HTTP/1.1\r\n\r\n" + css)
        assert read_message(stream, 200)[1] == b"<h1>Hello</h1>"
        read_message(stream, 400, closing=True)
        assert stream.read(1) == b""
    print("PASS: keep-alive reuse, ordered GET/POST pipelines, close, half-close, and errors")


def check_connection_limit(address):
    with contextlib.ExitStack() as connections:
        for _ in range(128):
            connections.enter_context(connect(address))
        with connect(address) as client:
            read_response(client, 503)
    request(address, GET, 200)
    print("PASS: connection limit and slot reuse")


def run_checks(binary):
    with socket.socket() as probe:
        probe.bind((HOST, 0))
        address = probe.getsockname()

    files = {
        "index.html": (b"<h1>Hello</h1>", b"text/html"),
        "site.css": (b"body {}", b"text/css"),
        "site.js": (b"void 0;", b"text/javascript"),
        "pixel.png": (bytes(range(256)) * 35, b"image/png"),
        "photo.jpg": (b"\xff\xd8\0\xff\xd9", b"image/jpeg"),
        "unmapped.bin": (b"\0\xff", b"application/octet-stream"),
    }
    with tempfile.TemporaryDirectory() as directory, tempfile.TemporaryFile() as log:
        root = Path(directory)
        for name, (payload, _) in files.items():
            (root / name).write_bytes(payload)
        with (root / "large.bin").open("wb") as large_file:
            large_file.truncate(LARGE_FILE_SIZE)
        (root / "symlink").symlink_to(root / "index.html")
        (root / "symlink-dir").symlink_to(root, target_is_directory=True)

        with subprocess.Popen([binary, str(address[1]), directory], stderr=log) as server:
            try:
                with connect_when_ready(address, server):
                    pass
                conflict = subprocess.run(
                    [binary, str(address[1]), directory], capture_output=True, timeout=5
                )
                assert conflict.returncode == 1 and b"bind:" in conflict.stderr, conflict
                check_headers(address)
                check_post(address)
                check_files(address, files)
                check_keep_alive(address)
                check_slow_reader(address)
                check_connection_limit(address)
                check_timeouts(address)
                with connect(address) as client:
                    client.sendall(GET[:-1])
                    incomplete_request_is_waiting(client)
                    server.send_signal(signal.SIGTERM)
                    assert server.wait(timeout=5) == 0
                    assert client.recv(1) == b""
                print("PASS: SIGTERM while HTTP header is incomplete")
            except Exception:
                log.seek(0)
                sys.stderr.write(log.read().decode(errors="replace"))
                raise
            finally:
                if server.poll() is None:
                    server.kill()


if __name__ == "__main__":
    run_checks(str(Path(sys.argv[1]).resolve()))
