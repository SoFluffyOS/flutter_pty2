#!/usr/bin/env python3

import base64
import hashlib
import json
import os
import re
import socket
import struct
import subprocess
import sys
import time
from urllib.parse import urlparse
from urllib.request import urlopen


SERVICE_URL_PATTERN = re.compile(
    r"Dart VM service is listening on (https?://\S+)"
)
WEBSOCKET_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def read_exact(connection, length):
    chunks = []
    received = 0
    while received < length:
        chunk = connection.recv(length - received)
        if not chunk:
            raise RuntimeError("VM service closed the WebSocket")
        chunks.append(chunk)
        received += len(chunk)
    return b"".join(chunks)


def send_websocket_text(connection, message):
    payload = message.encode("utf-8")
    mask = os.urandom(4)
    header = bytearray([0x81])
    if len(payload) < 126:
        header.append(0x80 | len(payload))
    elif len(payload) < 65536:
        header.append(0x80 | 126)
        header.extend(struct.pack(">H", len(payload)))
    else:
        header.append(0x80 | 127)
        header.extend(struct.pack(">Q", len(payload)))
    header.extend(mask)
    masked = bytes(
        value ^ mask[index % len(mask)]
        for index, value in enumerate(payload)
    )
    connection.sendall(header + masked)


def read_websocket_frame(connection):
    first, second = read_exact(connection, 2)
    opcode = first & 0x0F
    length = second & 0x7F
    if length == 126:
        length = struct.unpack(">H", read_exact(connection, 2))[0]
    elif length == 127:
        length = struct.unpack(">Q", read_exact(connection, 8))[0]
    if second & 0x80:
        mask = read_exact(connection, 4)
        payload = bytes(
            value ^ mask[index % len(mask)]
            for index, value in enumerate(read_exact(connection, length))
        )
    else:
        payload = read_exact(connection, length)
    return opcode, payload


def resume_isolate(service_url):
    parsed = urlparse(service_url)
    base_url = service_url.rstrip("/")
    isolates = []
    for _ in range(50):
        with urlopen(f"{base_url}/getVM", timeout=2) as response:
            response_value = json.load(response)
        vm = response_value.get("result", response_value)
        isolates = vm.get("isolates", [])
        if isolates:
            break
        time.sleep(0.2)
    if not isolates:
        raise RuntimeError("VM service did not report a test isolate")
    isolate_id = isolates[0]["id"]

    websocket_path = parsed.path.rstrip("/") + "/ws"
    key = base64.b64encode(os.urandom(16)).decode("ascii")
    connection = socket.create_connection((parsed.hostname, parsed.port), 10)
    try:
        request = (
            f"GET {websocket_path} HTTP/1.1\r\n"
            f"Host: {parsed.hostname}:{parsed.port}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
        )
        connection.sendall(request.encode("ascii"))
        response_headers = b""
        while b"\r\n\r\n" not in response_headers:
            response_headers += connection.recv(4096)
        status_line = response_headers.split(b"\r\n", 1)[0]
        if b" 101 " not in status_line:
            raise RuntimeError(
                f"VM service WebSocket handshake failed: {status_line!r}"
            )
        expected_accept = base64.b64encode(
            hashlib.sha1((key + WEBSOCKET_GUID).encode("ascii")).digest()
        )
        if expected_accept not in response_headers:
            raise RuntimeError("VM service WebSocket accept key was invalid")
        send_websocket_text(
            connection,
            json.dumps(
                {
                    "jsonrpc": "2.0",
                    "id": 1,
                    "method": "resume",
                    "params": {"isolateId": isolate_id},
                }
            ),
        )
        opcode, payload = read_websocket_frame(connection)
        if opcode != 1:
            raise RuntimeError("VM service did not acknowledge isolate resume")
        result = json.loads(payload.decode("utf-8"))
        if result.get("error") is not None:
            raise RuntimeError(f"VM service resume failed: {result['error']}")
    finally:
        connection.close()


def main():
    if len(sys.argv) < 2:
        raise SystemExit(
            "usage: run_flutter_test_with_vm_service.py TEST [TEST ...]"
        )
    process = subprocess.Popen(
        ["flutter", "test", "--start-paused", *sys.argv[1:]],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    try:
        service_url = None
        for line in process.stdout:
            sys.stdout.write(line)
            sys.stdout.flush()
            match = SERVICE_URL_PATTERN.search(line)
            if match is not None:
                service_url = match.group(1)
            if service_url is not None and "test process has been started" in line:
                break
        if service_url is None:
            return process.wait()
        resume_isolate(service_url)
        for line in process.stdout:
            sys.stdout.write(line)
            sys.stdout.flush()
        return process.wait()
    except BaseException:
        process.terminate()
        process.wait()
        raise


if __name__ == "__main__":
    raise SystemExit(main())
