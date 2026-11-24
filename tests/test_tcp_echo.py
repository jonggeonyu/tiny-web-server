import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path


HOST = "127.0.0.1"
TIMEOUT = 5


def check_invalid_port(server):
    result = subprocess.run(
        [server, "65536"], capture_output=True, timeout=TIMEOUT
    )
    if result.returncode != 1 or b"usage:" not in result.stderr:
        raise AssertionError(
            f"Expected a usage error for port 65536, got exit "
            f"{result.returncode} and stderr {result.stderr!r}"
        )


def connect_when_ready(address, process):
    deadline = time.monotonic() + TIMEOUT
    while True:
        if process.poll() is not None:
            raise RuntimeError(f"Server exited during startup with status {process.returncode}")
        try:
            return socket.create_connection(address, timeout=TIMEOUT)
        except ConnectionRefusedError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.02)


def check_echo(client, payload):
    client.sendall(payload)
    received = bytearray()
    while len(received) < len(payload):
        chunk = client.recv(len(payload) - len(received))
        if not chunk:
            break
        received.extend(chunk)
    if received != payload:
        raise AssertionError(
            f"Echo differs from {len(payload)} input bytes "
            f"({len(received)} bytes received)"
        )


def check_server(server):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as port_probe:
        port_probe.bind((HOST, 0))
        address = port_probe.getsockname()

    with tempfile.TemporaryFile() as log, subprocess.Popen(
        [server, str(address[1])], stdout=subprocess.DEVNULL, stderr=log
    ) as process:
        try:
            with connect_when_ready(address, process) as client:
                check_echo(client, b"first write\x00")
                check_echo(client, bytes(range(256)) * 32)
                client.shutdown(socket.SHUT_WR)
                if client.recv(1) != b"":
                    raise AssertionError("Expected EOF after the client closed its write side")

            with socket.create_connection(address, timeout=TIMEOUT) as client:
                check_echo(client, b"second connection")
                process.send_signal(signal.SIGINT)
                status = process.wait(timeout=TIMEOUT)
                if status != 0:
                    raise AssertionError(f"Expected exit status 0 after SIGINT, got {status}")
                if client.recv(1) != b"":
                    raise AssertionError("Expected the server to close the active connection")
        except Exception:
            log.seek(0)
            sys.stderr.write(log.read().decode(errors="replace"))
            raise
        finally:
            if process.poll() is None:
                process.kill()


if __name__ == "__main__":
    server = str(Path(sys.argv[1]).resolve())
    check_invalid_port(server)
    check_server(server)
    print("PASS: invalid port, byte-for-byte echo, EOF, reconnect, and SIGINT")
