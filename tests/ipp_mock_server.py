#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

from __future__ import annotations

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import struct
import sys
from typing import Iterable


def encode_value(tag: int, name: str, value: bytes) -> bytes:
    encoded_name = name.encode("ascii")
    return (
        bytes([tag])
        + struct.pack(">H", len(encoded_name))
        + encoded_name
        + struct.pack(">H", len(value))
        + value
    )


def encode_values(tag: int, name: str, values: Iterable[bytes]) -> bytes:
    values = list(values)
    if not values:
        return b""

    encoded_name = name.encode("ascii")
    result = bytearray()
    for index, value in enumerate(values):
        result.append(tag)
        if index == 0:
            result.extend(struct.pack(">H", len(encoded_name)))
            result.extend(encoded_name)
        else:
            result.extend(b"\x00\x00")
        result.extend(struct.pack(">H", len(value)))
        result.extend(value)
    return bytes(result)


def keyword_values(name: str, *values: str) -> bytes:
    return encode_values(0x44, name, (value.encode("utf-8") for value in values))


def integer_values(tag: int, name: str, *values: int) -> bytes:
    return encode_values(tag, name, (struct.pack(">i", value) for value in values))


def ipp_response(request: bytes) -> bytes:
    if len(request) < 8:
        raise ValueError("truncated IPP request")

    version = request[0:2]
    request_id = request[4:8]
    result = bytearray(version + b"\x00\x00" + request_id)

    # Operation attributes required in a normal IPP response.
    result.append(0x01)
    result.extend(encode_value(0x47, "attributes-charset", b"utf-8"))
    result.extend(encode_value(0x48, "attributes-natural-language", b"en"))

    # Printer attributes. The values deliberately mirror the important
    # normalized capabilities of the Canon TS5300-series reference device.
    result.append(0x04)
    result.extend(keyword_values(
        "print-color-mode-supported",
        "color",
        "monochrome",
        "auto",
        "auto-monochrome",
    ))
    result.extend(keyword_values(
        "media-supported",
        "iso_a4_210x297mm",
        "na_letter_8.5x11in",
    ))
    result.extend(keyword_values(
        "media-type-supported",
        "stationery",
        "photographic-glossy",
    ))
    result.extend(keyword_values("media-source-supported", "main", "rear"))
    result.extend(keyword_values(
        "sides-supported",
        "one-sided",
        "two-sided-long-edge",
        "two-sided-short-edge",
    ))
    result.extend(integer_values(0x23, "print-quality-supported", 3, 4, 5))
    result.extend(encode_values(
        0x32,
        "printer-resolution-supported",
        [struct.pack(">iiB", 600, 600, 3)],
    ))
    result.extend(encode_values(
        0x49,
        "document-format-supported",
        [b"image/jpeg", b"image/urf", b"image/pwg-raster"],
    ))
    result.extend(encode_value(
        0x33,
        "copies-supported",
        struct.pack(">ii", 1, 99),
    ))

    result.extend(integer_values(0x23, "printer-state", 3))
    result.extend(encode_value(0x22, "printer-is-accepting-jobs", b"\x01"))
    result.extend(keyword_values("printer-state-reasons", "none"))
    result.extend(encode_values(0x42, "marker-names", [b"Color", b"Black"]))
    result.extend(keyword_values(
        "marker-types",
        "ink-cartridge",
        "ink-cartridge",
    ))
    result.extend(integer_values(0x21, "marker-levels", -2, 10))
    result.extend(integer_values(0x21, "marker-low-levels", 15, 15))

    result.append(0x03)
    return bytes(result)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, _format: str, *args: object) -> None:
        del args

    def _read_chunked(self) -> bytes:
        body = bytearray()
        while True:
            line = self.rfile.readline()
            if not line:
                raise ValueError("unexpected EOF in chunked request")
            size_text = line.split(b";", 1)[0].strip()
            size = int(size_text, 16)
            if size == 0:
                while True:
                    trailer = self.rfile.readline()
                    if trailer in (b"\r\n", b"\n", b""):
                        break
                return bytes(body)
            body.extend(self.rfile.read(size))
            if self.rfile.read(2) != b"\r\n":
                raise ValueError("invalid chunk terminator")

    def _read_body(self) -> bytes:
        transfer_encoding = self.headers.get("Transfer-Encoding", "").lower()
        if "chunked" in transfer_encoding:
            return self._read_chunked()
        length = int(self.headers.get("Content-Length", "0"))
        return self.rfile.read(length)

    def do_POST(self) -> None:
        if self.path != "/ipp/print":
            self.send_error(404)
            return

        try:
            request = self._read_body()
            response = ipp_response(request)
        except Exception as error:  # test server: turn malformed input into HTTP 400
            payload = str(error).encode("utf-8", errors="replace")
            self.send_response(400)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Content-Length", str(len(payload)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(payload)
            return

        self.send_response(200)
        self.send_header("Content-Type", "application/ipp")
        self.send_header("Content-Length", str(len(response)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(response)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: ipp_mock_server.py <port>")

    server = ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), Handler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
