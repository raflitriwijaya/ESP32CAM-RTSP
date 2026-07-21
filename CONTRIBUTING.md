# Contributing

Thank you for considering contributing to this project.

## Quick Rules

Before you start, know that this project has a defined engineering standard
([CLAUDE.md](CLAUDE.md)). All contributions must comply. The non-negotiable
rules are:

- **No magic numbers** — every literal goes in `include/config.h` or
  `include/task_definitions.h`
- **No unbounded loops** on external data — every parser has explicit bounds
- **No dynamic allocation after setup()** without `heap_can_allocate()` guard
- **No blocking calls in task loops** without `vTaskDelay` or yield
- **Brownout detector stays enabled** — never add `WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0)`
- **Build flags:** `-Os`, `CORE_DEBUG_LEVEL=ERROR`, `-mfix-esp32-psram-cache-issue`
- **Functions that can fail return `esp_err_t`** — no silent defaults

## How to Contribute

### Reporting Bugs

Open an issue. Include:
- Hardware: AI-Thinker ESP32-CAM? Custom board?
- Firmware version: from `include/version.h` or git commit hash
- Steps to reproduce
- Serial output (baud 115200) — especially reboot reason and heap stats
- How long the device had been running

### Suggesting Features

Open an issue with "Feature Request" in the title. Explain what problem it
solves. Note that speculative features (MQTT, OTA, multi-device) are
intentionally deferred per CLAUDE.md §12 — check if your idea is already
on the roadmap.

### Pull Requests

1. **Fork and branch.** Branch from `main`, name your branch descriptively
   (`fix/rtsp-timeout`, `feature/ota-support`).

2. **Follow the directory structure** (CLAUDE.md §3):
   - New hardware abstractions → `src/app/`
   - New tasks → `src/tasks/` with entry in `task_definitions.h`
   - Pure logic → testable on host per CLAUDE.md §10.1

3. **Add tests.** Pure logic functions must have unit tests in `test/`.
   Integration tests in `test/integration/` for anything touching
   network or hardware boundaries.

4. **Build and verify:**

   ```
   pio run -e release   # must compile with zero warnings
   pio test -e native    # unit tests must pass
   ```

5. **Update documentation.** If you add, change, or remove behavior:
   - `CHANGELOG.md` — add an entry under `[Unreleased]`
   - `docs/ARCHITECTURE.md` — if task layout or data flow changes
   - `docs/HARDENING.md` — if it's a reliability improvement

6. **Run integration tests on hardware** before requesting review for
   changes that touch RTSP, HTTP, WiFi, or camera code.

7. **Describe your change.** In the PR description:
   - What problem it solves
   - What CLAUDE.md rules are affected (if any)
   - What testing was done (unit, integration, soak)

### Code Review Standards

Every PR is checked against the [engineering standard](CLAUDE.md). Reviewers
will verify:

- No magic numbers introduced
- All parsing loops bounded
- Heap guard before runtime allocations ≥1KB
- Error paths return `esp_err_t`, not silent defaults
- Task priorities and stack sizes are in `task_definitions.h`, not inline
- Build compiles with `-Os` and `ERROR` log level

### Commit Messages

Follow [Conventional Commits](https://www.conventionalcommits.org/):

```
feat: add OTA update support
fix: bound skipScanBytes to prevent buffer overrun
docs: update architecture diagram for frame pipeline
test: add integration test for HTTP client cap
refactor: extract quality mapping to camera_manager
```

## Development Environment

- **PlatformIO** (recommended) — `platformio.ini` has `[env:dev]` with
  `-Og` and `INFO` logging for development
- **ESP-IDF** — not yet supported. Migration is planned per CLAUDE.md §11.
  Do not submit ESP-IDF-only code until migration is complete.

## Project Status

See [README.md](README.md#project-status) for the current checklist.
Items marked `[ ]` are open for contribution.

## Questions?

Open a discussion or issue. Mention `@raftri` if it's architecture-related.
