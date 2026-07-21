# Audit Report: ESP32-Cam-RTSP (AI-Thinker ESP32-CAM)

**Scope:** Root cause analysis of 4 critical symptoms with evidence-based findings.
**Date:** 2026-07-06
**Board:** AI-Thinker ESP32-CAM (ESP32, PSRAM 4MB, OV2640)

---

## 1. Beban Sistem Berat Saat Runtime (CPU/Task Starvation)

### Root Cause (Evidence-Based)

**Single-threaded cooperative execution model with blocking I/O in the main loop.** Tidak ada FreeRTOS task yang dibuat secara eksplisit di seluruh codebase. Semua workload — WiFi state machine, web server, RTSP handshake, JPEG parsing, RTP fragmentation, dan camera capture — berjalan di Arduino `loop()` task (priority 1, core 1).

**Trigger utama starvation:**

1. **`handle_stream()` blocking loop** — `src/main.cpp:197`: `while (client.connected())` tanpa `yield()`/`delay()`. Saat client terhubung ke `/stream`, seluruh `loop()` tidak pernah return. RTSP server (`camera_server->doLoop()`) dan IotWebConf (`iotWebConf.doLoop()`) tidak pernah dipanggil — **RTSP stream mati total** selama ada MJPEG HTTP client.

2. **`socketread()` delay blocking** — `platglue-esp32.h:86-107`: Memblok dengan `delay(timeoutmsec)` saat tidak ada data. Dipanggil dari `CRtspSession::handleRequests(0)` dengan `timeoutmsec=0` (relatif aman), tapi desain fungsinya sendiri blocking — jika ada timeout > 0, seluruh loop task terblokir.

3. **`moustache_render()` di setiap request root** — `src/main.cpp:149`: Template rendering full HTML (~4.5KB) dengan 50+ variable substitutions di setiap `GET /`. Heap allocation untuk String result dilakukan setiap request. Tanpa caching.

4. **`CORE_DEBUG_LEVEL=ARDUHAL_LOG_LEVEL_VERBOSE`** — `platformio.ini:50`: ESP-IDF mengeluarkan semua log level di runtime via Serial. Ini path lambat yang signifikan — setiap WiFi event, TCP stack operation, dan camera frame ditulis ke UART 115200 bps.

5. **`-Ofast` build flag** — `platformio.ini:48`: Agresif optimisasi, berpotensi menghasilkan timing indeterministik yang mempengaruhi loop timing untuk RTSP frame scheduling.

### Evidence

| File | Line | Item |
|------|------|------|
| `src/main.cpp` | 410-416 | `loop()` — single cooperative loop, semua workload serialized |
| `src/main.cpp` | 197 | `while (client.connected())` — blocking tanpa yield |
| `platglue-esp32.h` | 86-107 | `socketread()` — blocking `delay()` call |
| `src/main.cpp` | 149 | `moustache_render()` — template parsing + String allocation setiap request |
| `platformio.ini` | 48,50 | `-Ofast` + `ARDUHAL_LOG_LEVEL_VERBOSE` |
| `lib/rtsp_server/rtsp_server.cpp` | 30-51 | `client_handler()` — iterates all RTSP clients sequentially, blocking frame dispatch |

### Fix Recommendation

1. **Pindahkan `handle_stream` ke FreeRTOS task terpisah** — buat task dengan stack 4096, priority 1 (sama dengan loop task), core 0 (biarkan WiFi di core 1). Gunakan `xTaskCreatePinnedToCore()`.
2. **Ganti `delay()` di `socketread` dengan select/poll non-blocking** atau setidaknya `delay(1)` dengan yield.
3. **Cache hasil `moustache_render`** — render ulang hanya saat nilai parameter berubah, simpan sebagai `static String`.
4. **Ganti `CORE_DEBUG_LEVEL` ke `ARDUHAL_LOG_LEVEL_ERROR`** untuk production, atau `ARDUHAL_LOG_LEVEL_INFO` untuk development.
5. **Ganti `-Ofast` ke `-Os`** atau `-O2` — ukuran firmware lebih kecil, timing lebih deterministik.

### Risk if Unfixed

Production-ready system tidak akan bisa handle >1 concurrent client. Satu MJPEG viewer memblokir RTSP untuk semua client lain. CPU starvation menyebabkan frame drop, latency spike, dan potensi missed RTSP keepalive → disconnect cascade.

---

## 2. Kualitas Gambar Buram (Sensor OV2640 / Kompresi JPEG / Frame Buffer)

### Root Cause (Evidence-Based)

**BUG KRITIS: Mismatch range JPEG quality antara web UI dan kamera driver.** Web UI `param_jpg_quality` didefinisikan dengan `.min(1).max(100)` (`src/main.cpp:25`), tetapi driver `esp_camera_init` menerima `jpeg_quality` dalam range **0-63 (lower = higher quality)**. Nilai 80 dari user akan diinterpretasikan driver sebagai out-of-range → undefined behavior. Bahkan nilai dalam range pun misleading: user mengira "80 = high quality" tapi driver menginterpretasikan "80 = extremely low quality".

Detail kode:
```cpp
// src/main.cpp:25 - Web UI: range 1-100
auto param_jpg_quality = iotwebconf::Builder<iotwebconf::UIntTParameter<byte>>("q")
    .label("JPG quality").defaultValue(DEFAULT_JPEG_QUALITY).min(1).max(100).build();

// src/main.cpp:243 - Langsung di-pass ke driver (expects 0-63)
.jpeg_quality = jpeg_quality,   // Quality of JPEG output. 0-63 lower means higher quality
```

**Kontributor lain:**

1. **Default JPEG quality tidak optimal** — `DEFAULT_JPEG_QUALITY (psramFound() ? 12 : 14)` (`include/settings.h:16`). Nilai 12 dalam range 0-63 adalah kualitas tinggi (cukup bagus), tapi karena ada PSRAM, tidak ada alasan untuk kompromi sebesar ini. Bisa diturunkan ke 8-10 untuk hasil lebih tajam.

2. **`CAMERA_GRAB_LATEST` tanpa timestamp check** — `src/main.cpp:246`. Mode ini melewatkan frame lama jika CPU lambat. Benar untuk RTSP, tapi saat CPU dibawah beban (lihat Gejala 1), frame bisa dilewati beberapa kali, menyebabkan "stuttering" visual.

3. **DCW (Downsize) enable by default** — `DEFAULT_DCW = true` (`settings.h:38`). Downscaling hardware dapat menurunkan ketajaman jika tidak diperlukan.

4. **`OV2640Streamer` constructor capture timing** — `OV2640Streamer.cpp:7`: Memanggil `cam.getWidth()` dan `cam.getHeight()` di initializer list sebelum `m_cam.run()` dipanggil. `getWidth()/getHeight()` memanggil `runIfNeeded()` yang hanya grab frame jika belum ada — jadi dimensi bisa akurat dari frame pertama, tapi timing-nya race-prone.

### Evidence

| File | Line | Item |
|------|------|------|
| `src/main.cpp` | 25 | `param_jpg_quality` range 1-100 |
| `src/main.cpp` | 243 | `jpeg_quality` langsung ke driver (range 0-63) |
| `include/settings.h` | 16 | `DEFAULT_JPEG_QUALITY` = 12 |
| `src/main.cpp` | 246 | `CAMERA_GRAB_LATEST` mode |
| `include/settings.h` | 38 | `DEFAULT_DCW = true` |
| `OV2640Streamer.cpp` | 7 | `cam.getWidth()`/`getHeight()` di construction |
| `CStreamer.cpp` | 84-85 | RTP JPEG header `m_width/8`, `m_height/8` — assumes dimensions divisible by 8 |

### Fix Recommendation

1. **Mapping JPEG quality yang benar:** Ubah web UI ke range 0-63, atau buat mapping: `actual_quality = map(ui_value, 1, 100, 63, 0)` — sehingga UI "100" menjadi driver "0" (kualitas terbaik).
2. **Set `DEFAULT_JPEG_QUALITY` ke 8** (PSRAM tersedia, tidak ada alasan kompromi).
3. **Set `DEFAULT_DCW = false`** — biarkan user yang memilih downscaling jika perlu.
4. **Defer `OV2640Streamer` dimension capture** — panggil `m_cam.run()` dulu sebelum getWidth/getHeight di constructor.

### Risk if Unfixed

User menyetel kualitas via web UI ke "80" (mengharapkan gambar tajam) dan mendapatkan gambar extremely compressed (hampir tidak bisa dikenali) karena driver membaca 80 sebagai "kualitas sangat rendah" dalam skala 0-63. Product review: "kamera buram meskipun setting kualitas tinggi".

---

## 3. Koneksi ke Access Point Lambat dan Sering Error Tanpa Pesan Jelas

### Root Cause (Evidence-Based)

**Tidak ada mekanisme retry/backoff sama sekali untuk koneksi WiFi.** Ini bukan asumsi — ini temuan. Seluruh WiFi connection logic di-delegate ke IotWebConf yang:

1. **Single attempt, no retry** — `IotWebConf::connectWifi()` (`IotWebConf.cpp:1013-1016`) hanya memanggil `WiFi.begin(ssid, password)`. Tidak ada retry loop, exponential backoff, atau multi-AP fallback.

2. **Failure handler returns nullptr by default** — `IotWebConf::handleConnectWifiFailure()` (`IotWebConf.cpp:1017-1019`) langsung return `nullptr`, artinya satu kali timeout → langsung jatuh ke AP mode. Tidak ada opsi retry.

3. **AP mode enforced on cold boot** — `src/main.cpp:58-59`: `WIFI_PASSWORD = nullptr` di `settings.h:7`. Karena tidak ada default WiFi password terkonfigurasi, `mustStayInApMode()` return true, dan sistem selalu boot ke AP mode dulu (~30 detik via `IOTWEBCONF_DEFAULT_AP_MODE_TIMEOUT_SECS`), baru mencoba WiFi. **Setiap cold boot minimal 30 detik sebelum WiFi attempt pertama.**

4. **WiFi disconnect detection tanpa debounce** — `IotWebConf.cpp:579-584`: Setiap kali `WiFi.status() != WL_CONNECTED` di state OnLine, langsung transisi ke Connecting. WiFi glitch sesaat → reconnect cycle penuh (disconnect → stop AP → WiFi.begin).

5. **Tidak ada pesan error user-visible** — Semua error reporting via `Serial.println()` / `printf()`. User tidak pernah tahu kenapa koneksi gagal kecuali mereka membuka serial monitor. LED blink pattern adalah satu-satunya indikator.

6. **WiFi.begin() tanpa timeout parameter** — ESP32 `WiFi.begin()` default timeout behavior tidak deterministic tanpa parameter eksplisit.

### Evidence

| File | Line | Item |
|------|------|------|
| `IotWebConf.cpp` | 1013-1016 | `connectWifi()` — single `WiFi.begin()`, no retry |
| `IotWebConf.cpp` | 1017-1019 | `handleConnectWifiFailure()` → `nullptr`, no retry |
| `include/settings.h` | 7 | `WIFI_PASSWORD nullptr` → forces AP mode first |
| `IotWebConfSettings.h` | 32-33 | `IOTWEBCONF_DEFAULT_WIFI_CONNECTION_TIMEOUT_MS = 30000` |
| `IotWebConfSettings.h` | 38-39 | `IOTWEBCONF_DEFAULT_AP_MODE_TIMEOUT_SECS = "30"` |
| `IotWebConf.cpp` | 579-584 | WiFi disconnect → immediate reconnect, no debounce |
| `IotWebConf.cpp` | 838-871 | `checkWifiConnection()` — single timeout check, no retry counter |
| `IotWebConf.cpp` | 652-714 | `stateChanged()` — no user-visible error message |

### Fix Recommendation

1. **Override `setWifiConnectionFailedHandler`** — implement retry dengan counter: 3x retry dengan exponential backoff (1s, 2s, 4s), baru jatuh ke AP mode setelah gagal total.
2. **Set `_skipApStartup = true` via `skipApStartup()`** — boot langsung ke WiFi connection jika kredensial tersimpan, lewati AP mode.
3. **Tambahkan WiFi disconnect debounce** — minimal 5 detik `WL_CONNECTED` false berturut-turut sebelum transisi ke Connecting.
4. **Tambahkan `WiFi.setAutoReconnect(true)`** — mengaktifkan auto-reconnect di level ESP-IDF.
5. **Tambahkan halaman status WiFi connection di web UI** — tampilkan state, error code terakhir, retry count.

### Risk if Unfixed

Setiap restart/power-cycle butuh 30-60 detik sebelum sistem online (AP timeout + WiFi timeout). WiFi interference sesaat menyebabkan RTSP disconnect untuk semua client saat sistem jatuh ke AP mode. Deployment di lokasi tanpa akses serial monitor → impossible untuk mendiagnosa kegagalan koneksi.

---

## 4. Restart/Reboot Spontan (Brownout, Stack Overflow, Heap Exhaustion, Watchdog, Task Priority)

### Root Cause (Evidence-Based)

**Beberapa kandidat independen, semuanya confirmed dari kode. Kombinasi >1 kandidat inilah yang paling mungkin memicu reboot.**

#### Kandidat A (PRIMARY): Brownout Detector Disabled

`src/main.cpp:317`:
```cpp
WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
```

Ini **menonaktifkan brownout detector** yang seharusnya me-reset ESP32 saat tegangan turun di bawah 2.7V. AI-Thinker ESP32-CAM memiliki regulator AMS1117 3.3V yang undersized untuk beban WiFi TX (spike ~300mA) + camera LED flash + frame buffer access. Tanpa brownout detector, tegangan drop tidak menyebabkan reset bersih — CPU mengalami **brownout corruption** (nilai register korup, bus error, random crash).

Catatan: Kode ini umum di project ESP32-CAM karena banyak power supply tidak stabil dan brownout reset dianggap "mengganggu." Tapi **menonaktifkannya menyembunyikan masalah power yang sebenarnya** dan menggantinya dengan crash yang lebih sulit didiagnosa.

#### Kandidat B: `skipScanBytes()` Infinite Loop Out-of-Bounds

`CStreamer.cpp:271-281`:
```cpp
void skipScanBytes(BufPtr *start) {
    BufPtr bytes = *start;
    while(true) { // FIXME, check against length   <-- KOMENTAR ASLI DEVELOPER
        while(*bytes++ != 0xff);
        if(*bytes++ != 0) {
            *start = bytes - 2;
            return;
        }
    }
}
```

Fungsi ini tidak memiliki bounds check. Jika JPEG data malformed (sensor glitch, low-light noise, atau corrupt PSRAM buffer), loop membaca beyond buffer → access violation → Guru Meditation Error (LoadProhibited/StoreProhibited) → reboot.

Dipanggil dari `decodeJPEGfile()` (`CStreamer.cpp:333`) → `streamFrame()` → `broadcastCurrentFrame()` → dipanggil setiap frame ke setiap RTSP client.

#### Kandidat C: Stack-Intensive Static Buffers + No Stack Monitoring

Multiple functions menggunakan `static char` buffer besar:

| File | Line | Buffer | Size |
|------|------|--------|------|
| `CRtspSession.cpp` | 38 | `CurRequest` | 10,000 bytes |
| `CRtspSession.cpp` | 378 | `RecvBuf` | 10,000 bytes |
| `CRtspSession.cpp` | 233,244,308,etc | `Response` | 1,024 bytes × 7 fungsi |
| `CStreamer.cpp` | 49 | `RtpBuf` | 2,048 bytes |

Meskipun `static` (bukan di stack), buffer-buffer ini **mengkonsumsi ~24KB BSS/DRAM** permanen. ESP32 klasik memiliki hanya ~320KB DRAM; dengan WiFi stack (~60KB), camera driver, IotWebConf, dan template HTML, total konsumsi DRAM mendekati limit.

Ditambah **tidak ada heap monitoring runtime** — tidak ada `esp_get_free_heap_size()` check sebelum alokasi atau warning threshold.

#### Kandidat D: Tidak Ada Task Watchdog Timer (TWDT)

**Zero TWDT configuration ditemukan di seluruh codebase.** Jika main loop task hang (misal: `handle_stream` blocking loop + dead TCP connection), tidak ada mekanisme hardware recovery. ESP32 Arduino framework menyediakan TWDT (`esp_task_wdt_init`, `esp_task_wdt_add`), tapi tidak digunakan.

#### Kandidat E: WiFi + Camera PSRAM Contention

ESP32 klasik memiliki bug hardware PSRAM cache (`-mfix-esp32-psram-cache-issue` di `boards/esp32cam_ai_thinker.json:11`). Workaround ini tidak sempurna — saat WiFi dan kamera mengakses PSRAM bersamaan (frame buffer PSRAM + WiFi TCP buffers), cache coherency issue dapat menyebabkan data corruption → crash.

#### Kandidat F: No Heap Guard / Memory Exhaustion Path

- `handle_root()` membuat String baru setiap request via `moustache_render()`.
- `WiFiClient` object untuk setiap RTSP client dialokasikan via `new` di `rtsp_server.cpp:36`.
- Tidak ada upper bound client count — setiap `accept()` yang sukses membuat client baru.
- Tidak ada pre-check `ESP.getFreeHeap()` sebelum alokasi besar.

### Evidence

| File | Line | Item |
|------|------|------|
| `src/main.cpp` | 317 | `WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0)` — brownout disabled |
| `CStreamer.cpp` | 271-281 | `skipScanBytes()` — unbounded pointer chase |
| `CStreamer.cpp` | 274 | `while(true)` — explicit FIXME comment from original author |
| `CRtspSession.cpp` | 38,378 | Static 10KB buffers × 2 |
| `boards/esp32cam_ai_thinker.json` | 11 | `-mfix-esp32-psram-cache-issue` — hardware bug workaround |
| All codebase | — | Zero TWDT configuration |
| All codebase | — | Zero heap guard before allocation |
| `rtsp_server.cpp` | 36 | Unbounded client accept |

### Fix Recommendation

1. **Re-enable brownout detector** — hapus `WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0)`. Jika power supply tidak stabil, fix power supply — jangan disable detector.
2. **Fix `skipScanBytes()` bounds** — tambahkan parameter `uint32_t *len` atau batasi loop dengan counter maksimum (e.g., 10000 iterasi). Return false (invalid JPEG) jika bounds exceeded.
3. **Enable TWDT** — tambahkan di `setup()`:
   ```cpp
   esp_task_wdt_init(10, true);  // 10 detik timeout, panic on timeout
   esp_task_wdt_add(NULL);        // monitor current task (loop)
   ```
   Tambahkan `esp_task_wdt_reset()` di dalam `loop()`.
4. **Batasi jumlah RTSP client** — hard cap 3 client, tolak koneksi baru dengan response 503.
5. **Tambahkan heap guard** — sebelum `new rtsp_client()` atau `moustache_render()`, check `ESP.getFreeHeap() > 30KB`, tolak request dengan 503 jika di bawah threshold.
6. **Pre-allocate atau pool RTSP client objects** — hindari heap fragmentation dari repeated `new`/`delete`.

### Risk if Unfixed

Production deployment akan mengalami reboot random yang sulit direproduksi (intermittent). Crash pada jam sibuk saat multiple RTSP viewer + web UI access + WiFi interference. Tanpa brownout detector, power issue tidak terdeteksi dan system corruption terakumulasi. Tanpa watchdog, system hang permanen sampai power cycle manual.

---

## Ringkasan Implikasi untuk Standar Robust/Reliable/Maintainable

Berikut constraint yang harus masuk ke CLAUDE.md sebagai aturan tetap project:

1. **Single-threaded loop tidak cukup** — setiap I/O blocking (MJPEG stream, JPEG parsing, WiFi reconnect) HARUS berjalan di FreeRTOS task terpisah. Loop task hanya untuk scheduling/orchestration, bukan execution.

2. **Setiap range parameter kamera harus divalidasi terhadap datasheet OV2640/esp_camera** sebelum diekspos ke UI. Mapping range wajib eksplisit, jangan pass-through buta.

3. **WiFi connection HARUS memiliki retry + exponential backoff** (minimal 3 attempt, 1s/2s/4s delay). Tidak boleh langsung fallback ke AP mode tanpa retry.

4. **Brownout detector WAJIB AKTIF** di production build. Jika power tidak stabil, solusinya hardware (tambahkan kapasitor 1000μF di rail 3.3V), bukan disable detector.

5. **Task Watchdog Timer (TWDT) wajib dikonfigurasi** untuk setiap task. Timeout 10 detik dengan panic handler. Setiap task yang bisa blocking HARUS reset TWDT secara periodik.

6. **Heap guard wajib** — setiap alokasi dinamis di atas 1KB harus didahului `ESP.getFreeHeap() > threshold`. Threshold didefinisikan sebagai 20% dari total heap.

7. **`-Ofast` tidak boleh digunakan untuk production** — ganti ke `-Os` atau `-O2`. Optimisasi agresif menutupi bug timing dan stack/heap.

8. **Debug level `VERBOSE` hanya untuk development** — build flag `CORE_DEBUG_LEVEL` harus `ARDUHAL_LOG_LEVEL_ERROR` (production) atau `ARDUHAL_LOG_LEVEL_WARN` (staging).

9. **Client connection HARUS memiliki batas atas (hard cap)** — RTSP max 3 concurrent, HTTP max 5 concurrent. Tolak dengan 503 + Retry-After header.

10. **JPEG parsing HARUS memiliki bounds checking** di semua path — tidak boleh ada unbounded pointer chase (`while(true)` seperti di `skipScanBytes`).
