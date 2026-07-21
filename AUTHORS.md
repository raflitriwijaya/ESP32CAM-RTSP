# Authors

## Original Author

**Rene Zeldent** ([@rzeldent](https://github.com/rzeldent))

- Original ESP32-CAM RTSP implementation
- Initial project structure and PlatformIO configuration
- IotWebConf integration for WiFi provisioning
- Micro-RTSP library integration

Original repository: [rzeldent/esp32cam-rtsp](https://github.com/rzeldent/esp32cam-rtsp)

## Production Hardening

**Rafli Triwijaya** ([@raftri](https://github.com/raftri))

- Systematic audit identifying 4 root-cause categories (see AUDIT_REPORT_V1.md)
- Engineering standards definition (CLAUDE.md)
- FreeRTOS 5-task architecture design and implementation
- Camera quality mapping layer (OV2640 datasheet-compliant)
- WiFi state machine with exponential backoff and debounce
- Task Watchdog Timer, heap guard, and client cap enforcement
- Bounded parsers replacing all unbounded loops
- Web server stack migration (synchronous → ESPAsyncWebServer)
- Integration test scripts
- Documentation: README, CHANGELOG, ARCHITECTURE, HARDENING

## AI Assistance

**Claude Code** (Anthropic) via **DeepSeek API**

- Assisted in code generation across 8 sequential hardening steps
- Automated codebase audit and rule-violation scanning
- Generated unit test cases and integration test scripts
- Produced final audit report with evidence-based verification

All AI-generated code was reviewed, tested, and validated by the human engineer
before being committed. No unreviewed AI output exists in this repository.

## Acknowledgments

- **Espressif** — ESP-IDF framework and camera driver
- **AI-Thinker** — ESP32-CAM hardware
- **ESP32-CAM community** — hardware workarounds and PSRAM cache bug documentation
- **IotWebConf** ([prampec/IotWebConf](https://github.com/prampec/IotWebConf)) — WiFi provisioning library
- **Micro-RTSP** ([geeksville/Micro-RTSP](https://github.com/geeksville/Micro-RTSP)) — RTSP server library
