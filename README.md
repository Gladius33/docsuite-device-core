# DocSuite Device Core

Native Linux printing, scanning and device services for the future **DocSuite** office stack.

The project starts from a concrete pain point: printing and scanning on Linux should be reliable, fast, consistent across browsers and office applications, and should not require users to understand vendor-specific PPD options or backend quirks.

## Goals

- Native **C++23** core with stable backend abstractions.
- Prefer standards: **IPP Everywhere / AirPrint** for printing and **SANE / eSCL / WSD** for scanning.
- Keep vendor-specific backends as fallbacks, not as the architectural center.
- Present consistent print profiles such as Color, Monochrome, Economy and Photo to every application.
- Cache device capabilities so opening a print dialog never blocks on slow device discovery.
- Instrument print jobs with measured state transitions instead of guessed timings.
- Reuse the same image/OCR/document foundations in a future professional PDF editor and office suite.
- Expose a stable local IPC API that can later be wrapped by an **MCP sidecar**, without putting MCP or an LLM in the critical print path.

## Current state

The current development version provides:

- CUPS printer discovery with transient/self-advertised queue filtering.
- Direct physical-printer IPP capability and status queries with CUPS fallback.
- Normalized color, duplex, quality, resolution, media, copy and format capabilities.
- Physical supply/status reporting when the printer exposes it.
- SANE scanner discovery with device deduplication.
- Native SANE acquisition using `sane_open`, `sane_control_option`, `sane_start`, `sane_get_parameters` and `sane_read`.
- CUPS job enumeration, cancellation and tracked diagnostic printing.
- JSONL diagnostic history under `$XDG_STATE_HOME/docsuite-device-core/print-history.jsonl` or `~/.local/state/docsuite-device-core/print-history.jsonl`.
- Qt 6 desktop device view with asynchronous capability/status loading.

## Reference hardware

Initial development is being validated against a Canon TS5300-series multifunction printer. The reference device exposes native IPP printing and eSCL/WSD scanning, making it a useful first target without coupling the codebase to Canon.

The architecture is intentionally vendor-neutral.

## Architecture

```text
Applications / Browser / LibreOffice / future DocSuite apps
                         |
                         v
                 DocSuite Device Core
                         |
       +-----------------+-----------------+
       |                 |                 |
   PrintCore          ScanCore         StatusCore
       |                 |                 |
  CUPS / IPP          SANE/eSCL            IPP
       |                 |                 |
       +-----------------+-----------------+
                         |
                    Physical device

                 stable local IPC API
                         |
                    MCP sidecar
```

The MCP bridge is deliberately outside the core. Printing and scanning must continue to work with no AI service, network service or MCP process running.

## Repository layout

```text
apps/
  cli/                 diagnostic CLI
  desktop/             Qt 6 desktop shell
include/docsuite/       public C++ API
src/
  device/              device aggregation
  print/               CUPS/IPP printing and job tracking
  scan/                SANE/eSCL scanner backends
tests/                  native tests
docs/                   architecture and design decisions
.github/workflows/       CI
```

## Build on Debian / Ubuntu / Kali / Pop!_OS

```bash
sudo apt update
sudo apt install -y \
  build-essential \
  cmake \
  ninja-build \
  pkg-config \
  libcups2-dev \
  libsane-dev \
  qt6-base-dev

cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo

cmake --build build
ctest --test-dir build --output-on-failure
```

## CLI examples

Discover devices:

```bash
./build/apps/cli/docsuite-device-cli list
```

Read physical printer capabilities/status:

```bash
./build/apps/cli/docsuite-device-cli capabilities canon_ts5353_ipp --refresh
./build/apps/cli/docsuite-device-cli status canon_ts5353_ipp
```

List jobs:

```bash
./build/apps/cli/docsuite-device-cli jobs canon_ts5353_ipp
./build/apps/cli/docsuite-device-cli jobs canon_ts5353_ipp active
```

Submit and trace a print job:

```bash
./build/apps/cli/docsuite-device-cli \
  diagnose-print canon_ts5353_ipp /tmp/test.jpg mono
```

The diagnostic reports measured CUPS state transitions and stores a JSONL trace. It deliberately does **not** claim to measure filter/raster/network stages that CUPS does not expose separately yet.

Native SANE scan to PNM:

```bash
./build/apps/cli/docsuite-device-cli \
  scan 'airscan:w0:CANON INC. TS5300 series' /tmp/scan.pnm 300 color
```

For grayscale:

```bash
./build/apps/cli/docsuite-device-cli \
  scan 'airscan:w0:CANON INC. TS5300 series' /tmp/scan-gray.pgm 300 gray
```

Run the desktop shell:

```bash
./build/apps/desktop/docsuite-device-center
```

## Near-term roadmap

1. Device enumeration through libcups/libsane. ✅
2. Canonical capability model independent of CUPS/PPD naming. ✅
3. Direct IPP capability/status backend with caching. ✅
4. CUPS job timeline instrumentation and latency diagnostics. ✅ first version
5. Native SANE scan acquisition. ✅ first version
6. Scan preview, crop and image export in the Qt desktop app.
7. Internal raster pipeline instrumentation for finer print timings.
8. Monochrome/color/economy/photo profiles exposed consistently to Linux applications.
9. Image processing + OCR + searchable PDF pipeline.
10. Local D-Bus/Unix-socket service and persistent shared cache.
11. MCP sidecar built on top of that stable API.
12. Shared DocumentCore for the future DocSuite PDF editor.

## Principles

- **Local-first.** Device operations do not depend on cloud services.
- **Standards-first.** IPP/eSCL/SANE before proprietary protocols.
- **Measure, do not guess.** Print latency is timestamped only where the stack exposes a measurable boundary.
- **No hidden destructive actions.** Maintenance operations such as deep cleaning require explicit confirmation.
- **No vendor lock-in.** Canon is the first reference target, not the product boundary.

## License

Mozilla Public License 2.0 (**MPL-2.0**). See [`LICENSE`](LICENSE).

MPL-2.0 was chosen for file-level copyleft: improvements to covered source files remain open while the core can still be combined with separately licensed or proprietary components in a larger future suite.
