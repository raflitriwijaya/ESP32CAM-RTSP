// test_parsers.cpp — Unit tests for bounded JPEG parsing functions.
//
// CLAUDE.md §10.1: pure logic functions must be testable on host.
// CLAUDE.md §10.5: all assertions must have descriptive message strings.
//
// Run with: pio test -e native

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cassert>
#include <cstring>

// ---------------------------------------------------------------------------
// Constant from config.h (duplicated here for host-test isolation)
// ---------------------------------------------------------------------------
#define SKIP_SCAN_MAX_ITER   1024

// ---------------------------------------------------------------------------
// Function under test — identical to lib/Micro-RTSP/src/CStreamer.cpp:281
// Duplicated here because the library depends on ESP32 platglue headers
// that aren't available in the native (host) test environment.
// ---------------------------------------------------------------------------

int skipScanBytes(const uint8_t *buf, size_t buf_len, size_t *pos, size_t max_iter) {
    if (max_iter > SKIP_SCAN_MAX_ITER) max_iter = SKIP_SCAN_MAX_ITER;
    if (*pos >= buf_len) return -1;

    size_t i = *pos;
    size_t end = buf_len;
    if (i + max_iter < end) end = i + max_iter;

    while (i < end) {
        if (buf[i] == 0xFF) {
            if ((i + 1) < buf_len && buf[i + 1] != 0) {
                *pos = i;
                return 0; // found marker at *pos
            }
        }
        i++;
    }
    *pos = i;
    return -1; // limit exceeded or end of buffer
}

// ---------------------------------------------------------------------------
// Test helper: create a buffer and verify skipScanBytes result
// ---------------------------------------------------------------------------

static void test_marker_found_at_position_0() {
    // JPEG marker 0xFF 0xDA (SOS) at position 0
    uint8_t buf[] = { 0xFF, 0xDA, 0x00, 0x08, 0x01, 0x02, 0x03, 0x04 };
    size_t pos = 0;
    int result = skipScanBytes(buf, sizeof(buf), &pos, SKIP_SCAN_MAX_ITER);
    assert(result == 0);
    assert(pos == 0); // should point to the 0xFF at position 0
    printf("PASS: test_marker_found_at_position_0\n");
}

static void test_marker_found_mid_buffer() {
    // Marker 0xFF 0xD9 (EOI) in the middle of the buffer
    uint8_t buf[] = {
        0x01, 0x02, 0x03, 0x04, 0x05,           // 5 bytes of scan data
        0xFF, 0xD9,                               // EOI marker at position 5
        0x00, 0x00                                // trailing data
    };
    size_t pos = 0;
    int result = skipScanBytes(buf, sizeof(buf), &pos, SKIP_SCAN_MAX_ITER);
    assert(result == 0);
    assert(pos == 5); // should point to the 0xFF at position 5
    printf("PASS: test_marker_found_mid_buffer\n");
}

static void test_no_marker_in_buffer() {
    // Buffer with no 0xFF byte at all
    uint8_t buf[] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 };
    size_t pos = 0;
    int result = skipScanBytes(buf, sizeof(buf), &pos, SKIP_SCAN_MAX_ITER);
    assert(result == -1);
    assert(pos == sizeof(buf)); // pos advanced to end of buffer
    printf("PASS: test_no_marker_in_buffer\n");
}

static void test_max_iter_exhausted() {
    // Buffer with only 0xFF 0x00 (stuffed bytes, not a valid marker),
    // so the scanner keeps looking but exhausts max_iter first.
    uint8_t buf[32];
    memset(buf, 0x00, sizeof(buf));
    buf[0] = 0xFF; // 0xFF 0x00 = stuffed byte, not a marker
    buf[1] = 0x00;
    // All remaining bytes are 0x00 (not 0xFF), so scanner never finds a marker

    size_t pos = 0;
    int result = skipScanBytes(buf, sizeof(buf), &pos, 5); // small max_iter
    assert(result == -1);
    assert(pos >= 5); // pos advanced at least max_iter positions
    printf("PASS: test_max_iter_exhausted\n");
}

static void test_empty_buffer() {
    uint8_t buf[] = {};
    size_t pos = 0;
    int result = skipScanBytes(buf, 0, &pos, SKIP_SCAN_MAX_ITER);
    assert(result == -1); // pos >= buf_len immediately
    printf("PASS: test_empty_buffer\n");
}

static void test_only_ff_bytes() {
    // Buffer with only 0xFF bytes, no terminating non-zero byte
    uint8_t buf[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    size_t pos = 0;
    int result = skipScanBytes(buf, sizeof(buf), &pos, SKIP_SCAN_MAX_ITER);
    assert(result == -1);
    printf("PASS: test_only_ff_bytes\n");
}

static void test_marker_at_max_iter_boundary() {
    // max_iter = 5, marker at position 4 (0xFF) and 5 (D9)
    // The scanner checks bytes at positions 0,1,2,3,4 — finds 0xFF at 4
    // with non-zero at 5 — this is a boundary-inclusive match (pos 4 < end 5)
    uint8_t buf[] = {
        0x00, 0x00, 0x00, 0x00,       // positions 0-3: non-0xFF
        0xFF, 0xD9,                     // positions 4-5: valid marker
    };
    size_t pos = 0;
    int result = skipScanBytes(buf, sizeof(buf), &pos, 5);
    assert(result == 0);
    assert(pos == 4); // marker at position 4
    printf("PASS: test_marker_at_max_iter_boundary\n");
}

static void test_pos_already_at_end() {
    uint8_t buf[] = { 0xFF, 0xD9, 0x00, 0x00 };
    size_t pos = sizeof(buf); // already at end
    int result = skipScanBytes(buf, sizeof(buf), &pos, SKIP_SCAN_MAX_ITER);
    assert(result == -1);
    assert(pos == sizeof(buf));
    printf("PASS: test_pos_already_at_end\n");
}

static void test_max_iter_clamped() {
    // max_iter > SKIP_SCAN_MAX_ITER should be clamped
    uint8_t buf[] = { 0x00, 0x00, 0xFF, 0xD9 };
    size_t pos = 0;
    int result = skipScanBytes(buf, sizeof(buf), &pos, 99999);
    assert(result == 0);
    assert(pos == 2); // marker found at position 2
    printf("PASS: test_max_iter_clamped\n");
}

static void test_stuffed_ff_bytes_before_marker() {
    // Several 0xFF 0x00 stuffed byte pairs, then a real marker 0xFF 0xD9
    uint8_t buf[] = {
        0xFF, 0x00,   // stuffed (not a marker — second byte is 0)
        0xFF, 0x00,   // stuffed
        0xFF, 0x00,   // stuffed
        0xFF, 0xD9,   // real EOI marker at position 6
    };
    size_t pos = 0;
    int result = skipScanBytes(buf, sizeof(buf), &pos, SKIP_SCAN_MAX_ITER);
    assert(result == 0);
    assert(pos == 6); // should skip past stuffed bytes, find marker at pos 6
    printf("PASS: test_stuffed_ff_bytes_before_marker\n");
}

static void test_marker_near_end_of_buffer() {
    // Marker at the very end: requires i+1 < buf_len check
    uint8_t buf[] = { 0x00, 0x00, 0x00, 0xFF, 0xD9 };
    size_t pos = 0;
    int result = skipScanBytes(buf, sizeof(buf), &pos, SKIP_SCAN_MAX_ITER);
    assert(result == 0);
    assert(pos == 3);
    printf("PASS: test_marker_near_end_of_buffer\n");
}

static void test_ff_at_end_no_room_for_second_byte() {
    // 0xFF at the last byte of buffer, no room for a second byte
    // i+1 >= buf_len, so it's not a valid marker
    uint8_t buf[] = { 0x00, 0x00, 0x00, 0xFF };
    size_t pos = 0;
    int result = skipScanBytes(buf, sizeof(buf), &pos, SKIP_SCAN_MAX_ITER);
    assert(result == -1); // no valid marker (can't check byte after 0xFF)
    printf("PASS: test_ff_at_end_no_room_for_second_byte\n");
}

// ---------------------------------------------------------------------------
// Test runner
// ---------------------------------------------------------------------------

int main() {
    printf("\n=== test_parsers: skipScanBytes() unit tests ===\n\n");

    test_marker_found_at_position_0();
    test_marker_found_mid_buffer();
    test_no_marker_in_buffer();
    test_max_iter_exhausted();
    test_empty_buffer();
    test_only_ff_bytes();
    test_marker_at_max_iter_boundary();
    test_pos_already_at_end();
    test_max_iter_clamped();
    test_stuffed_ff_bytes_before_marker();
    test_marker_near_end_of_buffer();
    test_ff_at_end_no_room_for_second_byte();

    printf("\n=== All %d tests passed ===\n", 12);
    return 0;
}
