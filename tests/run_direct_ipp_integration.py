#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

from __future__ import annotations

from pathlib import Path
import socket
import subprocess
import sys
import time


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def wait_for_server(port: int, process: subprocess.Popen[bytes]) -> None:
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(
                f"IPP mock exited before becoming ready (status {process.returncode})"
            )
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("IPP mock did not become ready within 5 seconds")


def run_probe(executable: Path, uri: str) -> int:
    result = subprocess.run(
        [str(executable), uri],
        check=False,
        capture_output=True,
        text=True,
        timeout=15,
    )
    if result.stdout:
        print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)
    return result.returncode


def main() -> int:
    if len(sys.argv) != 2:
        print(
            "usage: run_direct_ipp_integration.py <probe-test-executable>",
            file=sys.stderr,
        )
        return 2

    executable = Path(sys.argv[1]).resolve()
    if not executable.is_file():
        raise RuntimeError(f"probe test executable does not exist: {executable}")

    port = free_port()
    server_script = Path(__file__).with_name("ipp_mock_server.py")
    server = subprocess.Popen(
        [sys.executable, str(server_script), str(port)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )

    try:
        wait_for_server(port, server)

        # First cover the normal IPP Everywhere resource.
        if run_probe(executable, f"ipp://127.0.0.1:{port}/ipp/print") != 0:
            return 1

        # Then lock in the libcups URI-warning behavior that previously caused
        # DocSuite to reject a valid host-only URI as if it were malformed.
        if run_probe(executable, f"ipp://127.0.0.1:{port}") != 0:
            return 1

        print("Direct IPP resource + host-only URI integration: OK")
        return 0
    finally:
        server.terminate()
        try:
            server.wait(timeout=2)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait(timeout=2)


if __name__ == "__main__":
    raise SystemExit(main())
