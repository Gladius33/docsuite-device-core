# DocSuite Device Core

Native Linux printing, scanning, OCR, document processing and device services for the future **DocSuite** office stack.

DocSuite Device Core starts from a concrete Linux desktop problem: printing and scanning should be fast, discoverable and consistent across applications without forcing users to understand vendor PPD options, proprietary backends or scanner command-line tools.

The project is written primarily in **C++23** and uses standards-first backends: **IPP/CUPS** for printing and **SANE/eSCL** for scanning. MCP remains an optional local automation surface and is never part of the critical print/scan path.

## v0.5 status

The current development build is a usable Device Center with a persistent local service, direct eSCL fallback, document sessions, OCR, dynamic print validation and Linux/CUPS integration.

### Devices

- CUPS printer discovery with transient/self-advertised queue filtering and deduplication.
- Direct physical-printer **IPP Get-Printer-Attributes** with CUPS fallback.
- Normalized color, monochrome, duplex, quality, resolution, media, source, native-format and copy capabilities.
- Physical printer state, warnings and supply levels when reported by the device.
- Ten-minute capability cache so slow printer queries do not need to block every print interaction.
- SANE scanner discovery plus direct eSCL scanner discovery from IPP multifunction devices.
- Normalized scanner modes, sources, resolutions and scan-bed dimensions.

### Scan / OCR

- Native SANE acquisition using `sane_init`, `sane_open`, `sane_control_option`, `sane_start`, `sane_get_parameters` and `sane_read` — no `scanimage` subprocess.
- Scanner routing through `DeviceManager`: SANE first, direct eSCL fallback when SANE cannot open/acquire the network scanner.
- Direct eSCL acquisition implemented in C++ over libcups HTTP primitives:
  - `ScannerCapabilities`;
  - `ScanJobs`;
  - `Location` handling;
  - `NextDocument`;
  - bounded retry when a scanner temporarily returns HTTP 503 while preparing the page;
  - full-page `ScanRegion` derived from advertised geometry;
  - JPEG decoding through OpenCV.
- Dynamic GUI controls derived from actual scanner capabilities.
- Preview and full-resolution acquisition.
- RGB and grayscale acquisition when exposed by the scanner.
- Rotate left/right and grayscale conversion.
- Export PNG/JPEG and PDF.
- Optional native **Tesseract** OCR (`fra+eng`) through `TessBaseAPI`.
- OCR text copy/save plus word confidence and bounding boxes.
- Searchable PDF text layer after OCR.

### Document / ImageCore

The **Document** tab is now a multi-page workflow rather than a single-scan screen:

- acquire multiple pages into one session;
- preview and select pages;
- reorder pages up/down;
- remove pages;
- OCR individual pages;
- export a multi-page PDF, preserving searchable OCR text page by page.

Reusable `ImageCore` processing is outside the GUI so the future PDF application and local automation can use the same implementation:

- grayscale conversion;
- content detection and automatic crop;
- contrast normalization;
- blank-page detection;
- OpenCV CLAHE document enhancement;
- adaptive black-and-white conversion;
- automatic deskew when OpenCV is enabled.

Portable fallbacks remain available for the basic image operations when OpenCV is not installed; direct eSCL JPEG acquisition currently requires an OpenCV-enabled build.

### Print / Jobs

- File submission through libcups; no `lp` subprocess.
- Fully dynamic standard IPP controls: media, media source, media type, color mode, duplex, quality and copies.
- Built-in GUI presets: standard color, document monochrome, economy mono duplex and high-quality color.
- **Fast preflight** validates a profile against the cached normalized capabilities before a job is created.
- **Detailed CUPS preflight** is available in PrintCore for explicit cross-option constraint checks through `cupsCopyDestInfo`, `cupsCheckDestSupported` and `cupsCopyDestConflicts`.
- The GUI performs zero-network live preflight whenever a print option changes, using the already loaded capability snapshot.
- CUPS job history and active-job listing.
- Job cancellation.
- Diagnostic print mode now uses the same advanced/preflighted submission path as normal printing.
- JSONL timing history under `$XDG_STATE_HOME/docsuite-device-core/print-history.jsonl` or `~/.local/state/docsuite-device-core/print-history.jsonl`.
- Timing deliberately reports only observable boundaries; it does not invent raster/network timings that the backend cannot measure.

### Copy

- Native scan-to-print workflow from the GUI.
- Scanner and printer options are populated dynamically from SANE/eSCL and IPP capabilities rather than fixed lists.
- Select scanner, printer, resolution, scan mode, paper, source/type, print color mode, quality, duplex and copy count.
- The source page is scanned once.
- Multiple copies are submitted as one CUPS job with the standard `copies` attribute instead of creating N duplicate jobs.

### System / Linux integration

The **System** tab provides an explicit standards-based way to expose a normalized driverless queue to desktop applications:

- creates/reuses an IPP Everywhere local queue through the CUPS `CUPS-Create-Local-Printer` operation;
- does not execute `lpadmin` or require the GUI to run as root;
- refuses to overwrite a conflicting non-driverless queue;
- clearly reports that a CUPS local printer created by this operation is temporary.

DocSuite can also create **per-user CUPS destination instances** for convenience profiles without creating extra physical printers:

- `mono` — exact standard `print-color-mode=monochrome`;
- `draft` — monochrome + draft quality;
- `duplex` — monochrome + long-edge duplex;
- `photo` — color + high quality, with discovered photo medium/type/source when available.

A semantic profile is skipped if its exact standard attributes are unsupported; DocSuite does not silently turn “monochrome” into color or “duplex” into simplex.

### Local service / IPC

`docsuite-device-service` is a persistent per-user local service using a Unix-domain socket (`QLocalServer`). It keeps the shared `DeviceManager` and capability cache alive independently of the GUI.

Current methods:

- `ping`
- `device.list`
- `printer.status`
- `printer.capabilities`
- `printer.jobs`
- `printer.cancel_job`
- `scanner.capabilities`
- `scanner.scan`

Security boundaries currently enforced for writable methods:

- the local socket uses user-only access;
- print-job cancellation is allowed only when CUPS reports the job as owned by the Unix user running the service;
- terminal jobs are not canceled again;
- scanner mode/DPI/source input is bounded;
- scan output paths are **not client-controlled**;
- scans are written under `$XDG_RUNTIME_DIR/docsuite-device-core/scans/` with a private directory and owner-only files.

### MCP sidecar

`integrations/mcp/` contains a separate TypeScript MCP server. It communicates with `docsuite-device-service` over the local Unix socket and exposes:

- `device_list`
- `printer_status`
- `printer_capabilities`
- `printer_jobs`
- `printer_cancel_job`
- `scanner_capabilities`
- `scanner_scan`

The sidecar uses stdio and has no network listener. MCP/Node.js is **not** required for normal printing, scanning or the desktop GUI.

### Packaging / CI

GitHub Actions validates:

- C++ Debug build;
- C++ Release build;
- CTest, including ImageCore and print-profile validation;
- Qt desktop application;
- persistent device service;
- Tesseract-enabled build;
- OpenCV-enabled build;
- direct eSCL end-to-end acquisition against a real local HTTP mock (`ScanJobs → 503 warm-up → NextDocument → JPEG → PNM`);
- Unix-socket service acquisition against that eSCL mock;
- service safety boundaries (private socket/output, bounded scan settings, no shell RPC, no caller-selected scan path);
- TypeScript MCP sidecar type-check/build;
- Debian package generation.

The Release workflow produces the `docsuite-device-core-deb` workflow artifact containing the versioned `.deb`.

## Reference hardware

Initial development is being validated against a **Canon TS5300-series / PIXMA TS5353** network multifunction printer. On the reference unit:

- direct printing endpoint: `ipp://<printer>/ipp/print`;
- standard IPP monochrome is exposed as `print-color-mode=monochrome`;
- duplex, quality, media/source/type and copies are reported by IPP;
- the printer accepts JPEG/URF/PWG raster but does **not** advertise direct PDF printing;
- scanner endpoint is eSCL on HTTP port 80;
- SANE/sane-airscan remains the preferred first route, with direct eSCL as a native fallback.

Canon is a reference target, not a product boundary. The core is intentionally vendor-neutral.

## Architecture

```text
Browser / LibreOffice / future DocSuite PDF + Office apps
                         |
                    standard IPP
                         |
                 DocSuite Device Core
                         |
       +-----------------+------------------+
       |                 |                  |
   PrintCore          ScanCore           ImageCore
       |             /        \              |
  CUPS / IPP      SANE      direct eSCL   OCR/PDF
       |                 |                  |
       +-----------------+------------------+
                         |
                    physical device

       docsuite-device-service (private Unix socket)
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
  device/                    device aggregation / scanner routing
  print/                     CUPS/IPP, preflight, jobs, system queue integration
  scan/                      SANE + direct eSCL backends
  image/                     reusable document image processing
  ocr/                       optional Tesseract OCR
integrations/
  mcp/                       optional MCP sidecar
packaging/
  linux/                     desktop launcher
  systemd/                   user service unit
tests/
  core_tests.cpp             native regression tests
  escl_mock_server.py        end-to-end eSCL test server
.github/workflows/            CI + Debian package
```

## Build on Debian / Ubuntu / Kali / Pop!_OS

For the full build with OCR, OpenCV and direct eSCL acquisition:

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
  libopencv-dev \
  libtesseract-dev \
  tesseract-ocr-fra \
  tesseract-ocr-eng

cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DDOCSUITE_BUILD_DESKTOP=ON \
  -DDOCSUITE_BUILD_SERVICE=ON \
  -DDOCSUITE_BUILD_TESTS=ON \
  -DDOCSUITE_ENABLE_OCR=ON \
  -DDOCSUITE_ENABLE_OPENCV=ON

cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Launch the Device Center:

```bash
./build/apps/desktop/docsuite-device-center
```

Current GUI surfaces include:

```text
Devices | Scan / OCR | Document | Copy | Print / Jobs | System
```

## Build a Debian package locally

```bash
cmake --build build --target package
```

The package is written under `build/` and currently uses project version **0.5.0**.

Install the generated package with:

```bash
sudo apt install ./build/docsuite-device-core_0.5.0_amd64.deb
```

Enable the optional persistent per-user service:

```bash
systemctl --user daemon-reload
systemctl --user enable --now docsuite-device-service.service
systemctl --user status docsuite-device-service.service
```

## CLI

Useful low-level diagnostics remain available:

```bash
./build/apps/cli/docsuite-device-cli list
./build/apps/cli/docsuite-device-cli capabilities <printer> --refresh
./build/apps/cli/docsuite-device-cli status <printer>
./build/apps/cli/docsuite-device-cli jobs <printer>
./build/apps/cli/docsuite-device-cli jobs <printer> active
./build/apps/cli/docsuite-device-cli preflight <printer> mono
./build/apps/cli/docsuite-device-cli print <printer> <file> mono
./build/apps/cli/docsuite-device-cli diagnose-print <printer> <file> mono
./build/apps/cli/docsuite-device-cli scan <scanner> /tmp/scan.pnm 300 color
```

`print` and `diagnose-print` use the same advanced, validated CUPS submission path as the GUI.

## MCP development

The MCP bridge is optional and does not affect normal device operation.

```bash
cd integrations/mcp
npm install
npm run build
```

Run `docsuite-device-service` first, then use the MCP sidecar over stdio according to the MCP host configuration.

## First real-device validation targets

The CI deliberately covers everything it can without a physical printer/scanner. The first hardware session should concentrate on boundaries that simulation cannot prove:

1. direct TS5353 SANE acquisition and automatic fallback to direct eSCL when SANE opening is unavailable;
2. 150/300/600 dpi color and grayscale scans;
3. automatic crop/deskew/blank-page behavior on real documents;
4. multi-page searchable PDF quality;
5. driverless CUPS queue creation from the System tab;
6. visibility of standard monochrome/duplex controls in Brave/LibreOffice;
7. user CUPS instances (`mono`, `draft`, `duplex`, `photo`) where the application exposes destination instances;
8. A/B print latency versus the legacy Canon `cnijbe2` queue;
9. job timing history around CUPS pending/processing/completion boundaries.

## Not implemented yet

v0.5 is deliberately not advertised as the finished DocSuite stack. Later milestones include:

- PAPPL/OpenPrinting Printer Application front end if needed for stronger system-wide presentation independent of a local CUPS queue;
- full internal PDF→PWG/URF raster pipeline so direct physical printing can convert once without relying on arbitrary application/filter chains;
- finer first-byte/network/printer-processing instrumentation when the transport backend can expose it truthfully;
- PDF/A conformance validation and encrypted/password-protected PDF export;
- ADF multi-page acquisition on hardware that advertises it;
- printer maintenance operations (cleaning, nozzle check, alignment, quiet/power controls) only when safely discoverable;
- additional controlled MCP actions and document operations;
- the separate professional DocSuite PDF Studio / office applications.

## Principles

- **Local-first.** Device operations do not depend on cloud services.
- **Standards-first.** IPP/eSCL/SANE before proprietary protocols.
- **Measure, do not guess.** Timings are reported only at measurable boundaries.
- **No fake capabilities.** UI options come from the device/backend whenever possible.
- **No silent semantic substitution.** A monochrome or duplex profile is skipped if that exact behavior is unavailable.
- **No hidden destructive actions.** Maintenance or configuration changes require explicit policy and confirmation.
- **No vendor lock-in.** Canon is the first real validation target, not the product boundary.

## License

Mozilla Public License 2.0 (**MPL-2.0**). See [`LICENSE`](LICENSE).

MPL-2.0 provides file-level copyleft: changes to covered source files remain open while DocSuite Device Core can still be combined with separately licensed components in a larger future suite.
