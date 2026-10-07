# DocSuite Device Core

Native Linux printing, scanning, OCR and device services for the future **DocSuite** office stack.

DocSuite Device Core starts from a concrete Linux desktop problem: printing and scanning should be fast, discoverable and consistent across applications without forcing users to understand vendor PPD options, proprietary backends or scanner command-line tools.

The project is written primarily in **C++23** and uses standards-first backends: **IPP/CUPS** for printing and **SANE/eSCL/WSD** for scanning.

## v0.4 status

The current development build is a usable Device Center rather than only an architecture skeleton.

### Devices

- CUPS printer discovery with transient/self-advertised queue filtering and deduplication.
- Direct physical-printer **IPP Get-Printer-Attributes** with CUPS fallback.
- Normalized color, monochrome, duplex, quality, resolution, media, source, native-format and copy capabilities.
- Physical printer state, warnings and supply levels when reported by the device.
- Capability cache so slow printer queries do not need to block every UI interaction.
- SANE/eSCL/WSD scanner discovery and deduplication.
- Normalized scanner modes, sources, resolutions and scan-bed dimensions.

### Scan / OCR

- Native SANE acquisition using `sane_init`, `sane_open`, `sane_control_option`, `sane_start`, `sane_get_parameters` and `sane_read` — no `scanimage` subprocess.
- Resilient scanner opening with rediscovery/retry for network backends whose SANE identifier changes transiently.
- Dynamic GUI controls derived from the scanner's actual SANE capabilities.
- Preview acquisition and full-resolution scans.
- RGB and grayscale acquisition when exposed by the scanner.
- Large-frame support validated with real 300 dpi and 600 dpi network scans.
- Rotate left/right and convert to grayscale.
- Export PNG and JPEG.
- Export PDF.
- Optional native **Tesseract** OCR (`fra+eng`) through `TessBaseAPI`.
- OCR text copy/save plus word confidence and bounding boxes.
- PDF export can include the OCR text layer so a scanned page can become searchable after OCR.

### Print / Jobs

- File submission through libcups.
- Built-in profiles: standard color, document monochrome, economy mono duplex and high-quality color.
- CUPS job history and active-job listing.
- Job cancellation.
- Diagnostic print mode with measured CUPS state transitions.
- JSONL history under `$XDG_STATE_HOME/docsuite-device-core/print-history.jsonl` or `~/.local/state/docsuite-device-core/print-history.jsonl`.
- Timing deliberately reports only observable boundaries; it does not invent a separate raster/network duration when the current backend cannot measure it.

### Copy

- Native scan-to-print workflow from the GUI.
- Select scanner, printer, resolution, scan mode, print color mode, duplex and copy count.
- The source page is scanned once and submitted to CUPS without requiring the user to create an intermediate document manually.

### Local service / IPC

`docsuite-device-service` is a persistent per-user local service using a Unix-domain socket (`QLocalServer`). It keeps the shared DeviceManager/capability cache alive independently of the GUI.

The first IPC surface is intentionally read-only:

- `ping`
- `device.list`
- `printer.status`
- `printer.capabilities`
- `printer.jobs`
- `scanner.capabilities`

The socket is local and user-access-only. Print/scan/maintenance actions are intentionally not exposed through this first IPC revision.

### MCP sidecar

`integrations/mcp/` contains a separate TypeScript MCP server using the official MCP v2 server package. It communicates with `docsuite-device-service` over the local Unix socket and exposes read-only tools:

- `device_list`
- `printer_status`
- `printer_capabilities`
- `printer_jobs`
- `scanner_capabilities`

The sidecar uses stdio. MCP, Node.js or any AI component is **not** in the critical print or scan path.

### Packaging / CI

GitHub Actions validates:

- C++ Debug build
- C++ Release build
- CTest
- Qt desktop application
- persistent device service
- Tesseract-enabled build
- TypeScript MCP sidecar type-check/build
- Debian package generation

The Release workflow produces:

```text
docsuite-device-core_0.4.0_amd64.deb
```

as the `docsuite-device-core-deb` workflow artifact.

## Reference hardware

Initial development is being validated against a **Canon TS5300-series / PIXMA TS5353** network multifunction printer. On the reference unit:

- printing is available directly at `ipp://<printer>/ipp/print`;
- physical IPP reports standard color/monochrome, duplex, quality, media and native raster formats;
- scanning works through sane-airscan/eSCL/WSD;
- real native RGB scans have been validated at 300 dpi and 600 dpi.

Canon is a reference target, not a product boundary. The core is intentionally vendor-neutral.

## Architecture

```text
Browser / LibreOffice / future DocSuite PDF + Office apps
                         |
                         v
                 DocSuite Device Core
                         |
       +-----------------+------------------+
       |                 |                  |
   PrintCore          ScanCore          StatusCore
       |                 |                  |
  CUPS / IPP       SANE / eSCL / WSD    direct IPP
       |                 |                  |
       +-----------------+------------------+
                         |
                    physical device

       docsuite-device-service (local Unix socket)
                         |
                    MCP sidecar
                      (stdio)
```

## Repository layout

```text
apps/
  cli/                       diagnostic CLI
  desktop/                   Qt 6 Device Center
services/
  device-service/            persistent local IPC service
include/docsuite/            public C++ API
src/
  device/                    device aggregation
  print/                     CUPS/IPP + job tracking
  scan/                      SANE/eSCL backends
  ocr/                       optional Tesseract OCR
integrations/
  mcp/                       optional MCP v2 sidecar
packaging/
  linux/                     desktop launcher
  systemd/                   user service unit
tests/                       native smoke tests
.github/workflows/            CI + Debian package
```

## Build on Debian / Ubuntu / Kali / Pop!_OS

For the full build with OCR:

```bash
sudo apt update
sudo apt install -y \
  build-essential \
  cmake \
  ninja-build \
  pkg-config \
  libcups2-dev \
  libsane-dev \
  sane-airscan \
  qt6-base-dev \
  libtesseract-dev \
  tesseract-ocr-fra \
  tesseract-ocr-eng

cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DDOCSUITE_BUILD_DESKTOP=ON \
  -DDOCSUITE_BUILD_SERVICE=ON \
  -DDOCSUITE_BUILD_TESTS=ON \
  -DDOCSUITE_ENABLE_OCR=ON

cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Launch the Device Center:

```bash
./build/apps/desktop/docsuite-device-center
```

The GUI is the preferred manual test surface and currently contains:

```text
Devices | Scan / OCR | Copy | Print / Jobs
```

## Build a Debian package locally

```bash
cmake --build build --target package
```

The package is written under `build/`.

Install it with:

```bash
sudo apt install ./build/docsuite-device-core_0.4.0_amd64.deb
```

Then the desktop launcher **DocSuite Device Center** is available in the application menu.

Enable the optional persistent per-user service:

```bash
systemctl --user daemon-reload
systemctl --user enable --now docsuite-device-service.service
systemctl --user status docsuite-device-service.service
```

The GUI does not require the service yet; the service exists for shared cache/IPC and the MCP bridge.

## CLI

Useful low-level diagnostics remain available:

```bash
./build/apps/cli/docsuite-device-cli list
./build/apps/cli/docsuite-device-cli capabilities <printer> --refresh
./build/apps/cli/docsuite-device-cli status <printer>
./build/apps/cli/docsuite-device-cli jobs <printer>
./build/apps/cli/docsuite-device-cli jobs <printer> active
./build/apps/cli/docsuite-device-cli diagnose-print <printer> <file> mono
./build/apps/cli/docsuite-device-cli scan <scanner> /tmp/scan.pnm 300 color
```

## MCP development

The MCP bridge is optional and does not affect normal device operation.

```bash
cd integrations/mcp
npm install
npm run build
```

Run `docsuite-device-service` first, then use the MCP sidecar over stdio according to your MCP host configuration.

## Not implemented yet

v0.4 is deliberately not advertised as the finished DocSuite stack. The following are later milestones:

- system-wide Printer Application/PAPPL front end replacing the need to expose raw CUPS queues to applications;
- full internal PDF/PWG/URF raster pipeline with finer conversion/network instrumentation;
- crop, automatic document edges, deskew, blank-page removal and advanced image cleanup;
- multi-page scan project/session and multi-page PDF;
- PDF/A validation and encrypted/password-protected PDF export;
- printer maintenance operations (cleaning, nozzle check, alignment, quiet/power controls) only when safely discoverable;
- writable MCP actions with explicit authorization/confirmation policies;
- the separate full professional DocSuite PDF Studio / office applications.

## Principles

- **Local-first.** Device operations do not depend on cloud services.
- **Standards-first.** IPP/eSCL/SANE before proprietary protocols.
- **Measure, do not guess.** Timings are reported only at measurable boundaries.
- **No fake capabilities.** UI options come from the device/backend when possible.
- **No hidden destructive actions.** Maintenance or configuration changes require explicit policy and confirmation.
- **No vendor lock-in.** Canon is the first real validation target, not the product boundary.

## License

Mozilla Public License 2.0 (**MPL-2.0**). See [`LICENSE`](LICENSE).

MPL-2.0 provides file-level copyleft: changes to covered source files remain open while DocSuite Device Core can still be combined with separately licensed components in a larger future suite.
