# Architecture

## Decision 001 — Native C++ core

DocSuite Device Core uses C++23 for the core and Qt 6 for the desktop UI. Device backends are native library integrations rather than shell wrappers.

## Decision 002 — Standards first

Printing prefers IPP Everywhere/AirPrint through libcups. Scanning begins with libsane and is designed to gain a direct eSCL backend. Vendor-specific paths are fallbacks.

## Decision 003 — Capability cache

Capabilities are relatively static and must not be queried synchronously every time an application opens a print dialog. The future service will keep a cache and refresh it independently from the critical print path.

## Decision 004 — Stable domain API

UI, CLI, future D-Bus/Unix-socket service and MCP bridge must all consume the same domain API. MCP is a sidecar and never a dependency for printing or scanning.

## Decision 005 — Job instrumentation

Each print job will eventually record timestamps for submission, conversion, backend connection, first byte, device processing and completion. This makes intermittent Linux printing delays diagnosable rather than anecdotal.

## Reference-device findings

The first reference device is a Canon TS5300-series multifunction printer. Development testing has verified native IPP printing and eSCL/WSD scanning. This is test coverage, not a vendor-specific architectural constraint.

The implementation must remain suitable for other standards-compliant Canon, Epson, Brother, HP and similar devices.
