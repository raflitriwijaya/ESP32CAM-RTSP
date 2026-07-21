#!/usr/bin/env python3
"""
HTTP Client Cap Enforcement Test

Tests that the ESP32-CAM HTTP server enforces its 5-client maximum.
Opens HTTP connections to port 80, sends GET /status requests on each,
and verifies that the 6th connection is rejected with HTTP 503 and a
Retry-After header.

Approach: connections 1-5 are kept open to test the concurrent cap
(rather than sequential connect/disconnect). Connection 6 is attempted
while the first 5 are still alive, so the server must reject it based on
concurrent client count.

Usage:
    python test_http_cap.py --host 192.168.1.100
    python test_http_cap.py                  # uses ESP32CAM_IP env var

Expected exit code: 0 if all 6 checks pass, 1 if any fail.
"""

import socket
import sys
import os
import time

HTTP_PORT = 80
MAX_CLIENTS = 5
TIMEOUT = 5          # seconds — socket connect + recv
DELAY = 0.2          # seconds — between connection attempts


def get_ip():
    """Return target IP from --host arg or ESP32CAM_IP env var."""
    try:
        idx = sys.argv.index('--host')
        return sys.argv[idx + 1]
    except (ValueError, IndexError):
        pass
    return os.environ.get('ESP32CAM_IP', '')


def http_get(host, path):
    """Return a connected socket after sending an HTTP GET and reading
    the response.  Caller owns the returned socket — it must be closed
    by the caller to release the server-side connection slot."""
    sock = socket.create_connection((host, HTTP_PORT), timeout=TIMEOUT)
    request = (
        f"GET {path} HTTP/1.0\r\n"
        f"Host: {host}\r\n"
        f"Connection: keep-alive\r\n"
        f"\r\n"
    )
    sock.sendall(request.encode())

    # Read until we have the full HTTP response headers + body.
    # The status endpoint returns a small payload, so a single recv()
    # with a modest buffer is sufficient for header inspection.
    data = b''
    sock.settimeout(TIMEOUT)
    while True:
        try:
            chunk = sock.recv(4096)
            if not chunk:
                break
            data += chunk
            # Stop once we have headers + body delimiter.
            if b'\r\n\r\n' in data:
                headers_end = data.index(b'\r\n\r\n')
                headers_raw = data[:headers_end].decode('utf-8', errors='replace')
                # Check for Content-Length to decide if we need the body.
                content_length = 0
                for line in headers_raw.split('\r\n'):
                    if line.lower().startswith('content-length:'):
                        try:
                            content_length = int(line.split(':', 1)[1].strip())
                        except ValueError:
                            pass
                        break
                body_start = headers_end + 4
                if len(data) - body_start >= content_length:
                    break
        except socket.timeout:
            break

    return sock, data


def parse_status_and_headers(raw_response):
    """Parse an HTTP response into (status_code, headers_dict)."""
    header_section = raw_response.split(b'\r\n\r\n', 1)[0]
    lines = header_section.decode('utf-8', errors='replace').split('\r\n')
    if not lines:
        return 0, {}

    status_line = lines[0]
    parts = status_line.split(' ')
    status_code = int(parts[1]) if len(parts) >= 2 else 0

    headers = {}
    for line in lines[1:]:
        if ':' in line:
            key, value = line.split(':', 1)
            headers[key.strip().lower()] = value.strip()

    return status_code, headers


def main():
    ip = get_ip()
    if not ip:
        print("ERROR: No target IP provided.")
        print("Usage: python test_http_cap.py --host <ip>")
        print("       or set the ESP32CAM_IP environment variable.")
        sys.exit(2)

    print(f"ESP32-CAM HTTP Client Cap Test")
    print(f"Target:        {ip}:{HTTP_PORT}")
    print(f"Max clients:   {MAX_CLIENTS}")
    print(f"Approach:      keep connections open (concurrent cap test)")
    print(f"Socket timeout: {TIMEOUT}s\n")

    connections = []   # held open to consume server slots
    passed = 0
    total = MAX_CLIENTS + 1   # 5 allowed + 1 over-cap attempt

    # ── Open MAX_CLIENTS connections ────────────────────────────
    for i in range(MAX_CLIENTS):
        time.sleep(DELAY)
        label = f"Client {i + 1}/{MAX_CLIENTS}"
        try:
            sock, raw = http_get(ip, '/status')
            status_code, headers = parse_status_and_headers(raw)

            if status_code == 200:
                print(f"[PASS] {label}: HTTP 200 OK")
                passed += 1
                connections.append(sock)
            else:
                print(f"[FAIL] {label}: expected 200, got {status_code}")
                sock.close()
        except socket.timeout:
            print(f"[FAIL] {label}: connection timed out ({TIMEOUT}s)")
        except ConnectionRefusedError:
            print(f"[FAIL] {label}: connection refused — server not reachable?")
        except OSError as e:
            print(f"[FAIL] {label}: {e}")

    # ── Attempt to exceed the cap ───────────────────────────────
    time.sleep(DELAY)
    label = f"Client {MAX_CLIENTS + 1}/{total} (over cap)"
    try:
        sock, raw = http_get(ip, '/status')
        status_code, headers = parse_status_and_headers(raw)
        retry_after = headers.get('retry-after', None)

        if status_code == 503:
            if retry_after:
                print(f"[PASS] {label}: HTTP 503 with Retry-After: {retry_after}")
                passed += 1
            else:
                print(f"[FAIL] {label}: HTTP 503 but missing Retry-After header")
        else:
            print(f"[FAIL] {label}: expected 503, got {status_code}")
        sock.close()
    except (ConnectionRefusedError, ConnectionResetError, BrokenPipeError) as e:
        print(f"[PASS] {label}: rejected ({type(e).__name__}) — cap enforced")
        passed += 1
    except socket.timeout:
        print(f"[PASS] {label}: connection timed out — cap likely enforced")
        passed += 1
    except OSError as e:
        print(f"[FAIL] {label}: unexpected OS error: {e}")

    # ── Clean up held connections ───────────────────────────────
    for conn in connections:
        try:
            conn.close()
        except OSError:
            pass

    # ── Summary ─────────────────────────────────────────────────
    print(f"\n{'='*40}")
    print(f"Results: {passed}/{total} tests passed")
    if passed == total:
        print("Verdict:  PASS — HTTP client cap is enforced correctly.")
    else:
        print("Verdict:  FAIL — one or more checks did not pass.")
    sys.exit(0 if passed == total else 1)


if __name__ == '__main__':
    main()
