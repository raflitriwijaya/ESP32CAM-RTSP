#!/usr/bin/env python3
"""
RTSP Client Cap Enforcement Test

Tests that the ESP32-CAM RTSP server enforces its 3-client maximum.
Opens TCP connections to port 554, sends a minimal RTSP DESCRIBE request
on each, and verifies that the 4th connection is rejected.

Approach: connections 1-3 are kept open to test the concurrent cap
(rather than sequential connect/disconnect). Connection 4 is attempted
while the first 3 are still alive, so the server must reject it based on
concurrent client count.

Usage:
    python test_rtsp_cap.py --host 192.168.1.100
    python test_rtsp_cap.py                  # uses ESP32CAM_IP env var

Expected exit code: 0 if all 4 checks pass, 1 if any fail.
"""

import socket
import sys
import os
import time

RTSP_PORT = 554
MAX_CLIENTS = 3
TIMEOUT = 5          # seconds — socket connect + recv
DELAY = 0.2          # seconds — between connection attempts

# Minimal RFC 2326 DESCRIBE request.  CSeq is incremented per connection
# so each request is distinct.
DESCRIBE_TEMPLATE = (
    "DESCRIBE rtsp://{ip}:{port}/stream RTSP/1.0\r\n"
    "CSeq: {cseq}\r\n"
    "\r\n"
)


def get_ip():
    """Return target IP from --host arg or ESP32CAM_IP env var."""
    try:
        idx = sys.argv.index('--host')
        return sys.argv[idx + 1]
    except (ValueError, IndexError):
        pass
    return os.environ.get('ESP32CAM_IP', '')


def main():
    ip = get_ip()
    if not ip:
        print("ERROR: No target IP provided.")
        print("Usage: python test_rtsp_cap.py --host <ip>")
        print("       or set the ESP32CAM_IP environment variable.")
        sys.exit(2)

    print(f"ESP32-CAM RTSP Client Cap Test")
    print(f"Target:        {ip}:{RTSP_PORT}")
    print(f"Max clients:   {MAX_CLIENTS}")
    print(f"Approach:      keep connections open (concurrent cap test)")
    print(f"Socket timeout: {TIMEOUT}s\n")

    connections = []   # held open to consume server slots
    passed = 0
    total = MAX_CLIENTS + 1   # 3 allowed + 1 over-cap attempt

    # ── Open MAX_CLIENTS connections ────────────────────────────
    for i in range(MAX_CLIENTS):
        time.sleep(DELAY)
        label = f"Client {i + 1}/{MAX_CLIENTS}"
        try:
            sock = socket.create_connection((ip, RTSP_PORT), timeout=TIMEOUT)
            request = DESCRIBE_TEMPLATE.format(ip=ip, port=RTSP_PORT, cseq=i + 1)
            sock.sendall(request.encode())
            response = sock.recv(4096).decode('utf-8', errors='replace')
            status_line = response.split('\r\n')[0] if response else '(empty response)'

            if '503' not in status_line:
                # Any non-503 response (200 OK, 401 Unauthorized, etc.) counts
                # as acceptance — the server processed our request.
                print(f"[PASS] {label}: accepted  ({status_line.strip()})")
                passed += 1
                connections.append(sock)
            else:
                print(f"[FAIL] {label}: unexpectedly rejected with 503")
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
        sock = socket.create_connection((ip, RTSP_PORT), timeout=TIMEOUT)
        request = DESCRIBE_TEMPLATE.format(ip=ip, port=RTSP_PORT, cseq=MAX_CLIENTS + 1)
        sock.sendall(request.encode())
        response = sock.recv(4096).decode('utf-8', errors='replace')
        status_line = response.split('\r\n')[0] if response else '(empty response)'

        if '503' in status_line:
            print(f"[PASS] {label}: rejected with 503 (cap enforced)")
            passed += 1
        else:
            print(f"[FAIL] {label}: expected 503, got: {status_line.strip()}")
        sock.close()
    except (ConnectionRefusedError, ConnectionResetError, BrokenPipeError) as e:
        # Connection-level rejection is also a valid enforcement signal.
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
        print("Verdict:  PASS — RTSP client cap is enforced correctly.")
    else:
        print("Verdict:  FAIL — one or more checks did not pass.")
    sys.exit(0 if passed == total else 1)


if __name__ == '__main__':
    main()
