# DocSuite Device Core

Native Linux printing, scanning and device services for the future **DocSuite** office stack.

The project starts from a concrete pain point: printing and scanning on Linux should be reliable, fast, consistent across browsers and office applications, and should not require users to understand vendor-specific PPD options or backend quirks.

## Goals

- Native **C++23** core with stable backend abstractions.
- Prefer standards: **IPP Everywhere / AirPrint** for printing and **SANE / eSCL / WSD** for scanning.
- Keep vendor-specific backends as fallbacks, not as the architectural center.
- Present consistent print profiles such as Color, Monochrome, Economy and Photo to every application.
- Cache device capabilities so opening a print dialog never blocks on slow device discovery.
- Instrument print jobs end-to-end to identify latency in the application, raster pipeline, CUPS/backend, network or device.
- Reuse the same image/OCR/document foundations in a future professional PDF editor and office suite.
- Expose a stable local IPC API that can later be wrapped by an **MCP sidecar**, without putting MCP or an LLM in the critical print path.

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
  print/               CUPS/IPP printing backend
  scan/                SANE scanner backend
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

Run the diagnostic CLI:

```bash
./build/apps/cli/docsuite-device-cli list
```

Run the desktop shell:

```bash
./build/apps/desktop/docsuite-device-center
```

## Near-term roadmap

1. Device enumeration through libcups and libsane.
2. Canonical capability model independent of CUPS/PPD naming.
3. Direct IPP capability/status backend with caching.
4. Job timeline instrumentation and latency diagnostics.
5. Monochrome/color/economy/photo profiles exposed consistently to Linux applications.
6. Native scan acquisition and preview.
7. Image processing + OCR + searchable PDF pipeline.
8. Local D-Bus/Unix-socket API.
9. MCP sidecar built on top of that stable API.
10. Shared DocumentCore for the future DocSuite PDF editor.

## Principles

- **Local-first.** Device operations do not depend on cloud services.
- **Standards-first.** IPP/eSCL/SANE before proprietary protocols.
- **Measure, do not guess.** Print latency gets timestamps at each stage.
- **No hidden destructive actions.** Maintenance operations such as deep cleaning require explicit confirmation.
- **No vendor lock-in.** Canon is the first reference target, not the product boundary.

## License

Mozilla Public License 2.0 (**MPL-2.0**). See [`LICENSE`](LICENSE).

MPL-2.0 was chosen for file-level copyleft: improvements to covered source files remain open while the core can still be combined with separately licensed or proprietary components in a larger future suite.
