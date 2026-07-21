# Integration Tests — Client Cap Enforcement

Tests that the ESP32-CAM firmware enforces its client connection caps before
the 72-hour soak test (per CLAUDE.md §10.3).

| Cap | Limit | Rejection Signal |
|-----|-------|-----------------|
| RTSP (port 554) | 3 concurrent clients | 503 Service Unavailable, or connection refused/reset |
| HTTP (port 80)  | 5 concurrent clients | 503 Service Unavailable + `Retry-After` header |

Both tests keep connections open to stress the **concurrent** cap — they do
not test sequential accept/close patterns.

---

## Requirements

- Python 3.6+ (standard library only — no `pip install` needed)
- Host computer on the same network as the ESP32-CAM
- ESP32-CAM firmware running with RTSP server on port 554 and HTTP on port 80

---

## Running the Tests

### Set the target IP

Either pass `--host` on the command line or export the `ESP32CAM_IP`
environment variable:

```bash
# Option A: command-line argument
python test_rtsp_cap.py --host 192.168.1.100
python test_http_cap.py --host 192.168.1.100

# Option B: environment variable
export ESP32CAM_IP=192.168.1.100      # Linux/macOS
set ESP32CAM_IP=192.168.1.100         # Windows cmd
$env:ESP32CAM_IP="192.168.1.100"      # PowerShell

python test_rtsp_cap.py
python test_http_cap.py
```

### Run both tests back-to-back

```bash
python test_rtsp_cap.py --host 192.168.1.100 && python test_http_cap.py --host 192.168.1.100
```

---

## Expected Output (Passing Run)

### test_rtsp_cap.py

```
ESP32-CAM RTSP Client Cap Test
Target:        192.168.1.100:554
Max clients:   3
Approach:      keep connections open (concurrent cap test)
Socket timeout: 5s

[PASS] Client 1/3: accepted  (RTSP/1.0 401 Unauthorized)
[PASS] Client 2/3: accepted  (RTSP/1.0 401 Unauthorized)
[PASS] Client 3/3: accepted  (RTSP/1.0 401 Unauthorized)
[PASS] Client 4/4 (over cap): rejected with 503 (cap enforced)

========================================
Results: 4/4 tests passed
Verdict:  PASS — RTSP client cap is enforced correctly.
```

> **Note:** `401 Unauthorized` is a valid acceptance signal — the server
> processed the RTSP request and responded; only `503` (or a transport-level
> rejection) means the cap was hit.  A real RTSP client would then
> authenticate and proceed.

### test_http_cap.py

```
ESP32-CAM HTTP Client Cap Test
Target:        192.168.1.100:80
Max clients:   5
Approach:      keep connections open (concurrent cap test)
Socket timeout: 5s

[PASS] Client 1/5: HTTP 200 OK
[PASS] Client 2/5: HTTP 200 OK
[PASS] Client 3/5: HTTP 200 OK
[PASS] Client 4/5: HTTP 200 OK
[PASS] Client 5/5: HTTP 200 OK
[PASS] Client 6/6 (over cap): HTTP 503 with Retry-After: 30

========================================
Results: 6/6 tests passed
Verdict:  PASS — HTTP client cap is enforced correctly.
```

---

## Troubleshooting

### "connection refused" on all connections

- The ESP32-CAM is offline or on a different IP.  Verify with `ping`.
- The firmware may not have started the RTSP/HTTP server yet — wait 10-15
  seconds after boot and retry.

### Client 1 fails but the device is reachable

- A previous test run may have left connections in `TIME_WAIT` on the
  ESP32-CAM.  Wait 60 seconds for them to drain, then retry.
- The `/status` endpoint may not exist on your firmware build.  Check that
  `web_ui_service` is registered and serving the route.

### Client 6 passes when it should have been rejected

- The server may be closing idle connections before the 6th attempt arrives.
  The total time to open 5 connections is ~1 second (5 × 200 ms).  If the
  server's idle timeout is shorter than this, reduce `DELAY` in the script
  or increase the server's keep-alive timeout.
- The firmware may not have the client cap feature merged.  Verify the build
  includes the latest `rtsp_server` and `web_ui_service` changes.

### "HTTP 503 but missing Retry-After header"

- The cap is being enforced but the `Retry-After` header is not set.
  Check the HTTP 503 response path in `web_ui_service.cpp`.

### Timeout errors

- The 5-second socket timeout may be too short for a heavily loaded device.
  Increase `TIMEOUT` in the script if the device is under concurrent load
  from other sources.
- Check WiFi signal strength — high latency can cause timeouts on the
  initial TCP handshake.

---

## Script Design Notes

- **Standard library only**: `socket`, `sys`, `os`, `time`.  No `pip install`
  needed — the test environment stays minimal.
- **Concurrent cap test**: connections 1–N are kept open (not closed after
  each request) so the server sees N concurrent clients.  The over-cap
  attempt (N+1) is made while all N are still connected.
- **200 ms delay** between connections avoids tripping any rate limiting
  that may exist in the TCP accept path.
- **5-second timeout** on all socket operations — prevents the script from
  hanging indefinitely if the device is unresponsive.
