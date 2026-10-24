# Changelog

## Unreleased

### Auracast

- Python library: `hcicontroller.auracast` and `examples/auracast_source.py`
  broadcast a stereo LC3 stream from the PC through a controller, one BIG
  with two BIS. Unit test: `tests/auracast_test.py`.

### Features are linked, not compiled in

- The vendor crypto commands moved to `hci_vendor_crypto.cpp` and the four
  optional SDC features to `hci_sdc_feat_subrating.cpp`,
  `hci_sdc_feat_extfeat.cpp`, `hci_sdc_feat_past.cpp` and
  `hci_sdc_feat_pawr.cpp`. A project links the files it wants; weak defaults
  report the rest absent. `HCI_VENDOR_CRYPTO`, `HCI_SDC_FEAT_*` and
  `HCI_TRACE_COMMANDS` are gone. The SDC pool is sized for all four
  features. The command log is a runtime setting, `HciTraceCommands()`.

### Host transports

- Added an IPC host transport for the nRF5340 network core (`nRF5340_Net`
  target). It is IOsonata's `BtHciIpc` packet interface: one ring slot is one
  HCI packet, no H:4 framing. HciController owns the shared memory block and
  the application core attaches as the peer. The host link swaps between IPC
  and H:4 over UART with the mode button. SDC resource counts can be
  overridden with `-D` for the 64 KB network core RAM.
- The host transports are files a project links: `hci_host_uart.cpp`,
  `hci_host_usb.cpp` (the HCI function, the diagnostic CDC port and the
  UART bridge) and `hci_host_ipc.cpp`, with weak defaults in `hci_app.cpp`
  for the ones a board does not have. The `HCI_APP_HAS_*` compile switches
  and `hci_usb_mode.h` are gone; `HCI_USB_HCI_TRANSPORT` too, native HCI
  being the USB default and CDC H:4 a mode button press away.
- Reduced static RAM by sharing the mutually exclusive HCI host UART and USB
  transport FIFOs. The diagnostic CDC transmit buffering remains independent;
  diagnostic input is consumed and discarded.
- Added an independent bidirectional USB CDC-to-UART bridge on supported
  companion-modem boards. Host-selected CDC baud rates are applied immediately
  to the bridge UART without resetting the HCI UART or waiting for FIFO drain.
- Removed forced floating-point printf linkage from nRF52840 and nRF54LM20
  optimized build configurations.

- Native Bluetooth USB HCI is now IOsonata's `BtHciUsb` class on the IOsonata
  USB device core. TinyUSB and the HciController USB transport class are
  removed; HciController keeps only VID/PID policy and the serial string.
- Interface and endpoint numbers are allocated by IOsonata. The diagnostic
  CDC function moves from EP4/EP5 to EP3/EP4 in native mode. Hosts locate it
  by VID/PID and interface class.
- The USB serial string keeps the 1.0.0 byte order (DEVICEID[1] then
  DEVICEID[0]); IOsonata's default would have reversed it.
- USB bus suspend keeps the HCI session: configuration, endpoints and links
  are retained and pending packets go out after resume.
- No SCO alternate settings are advertised; the controller is LE only.

### Controller

- sdk-nrfxlib baseline moved: per link memory changed, Core 6.2 pool is 93530
  of 94042 bytes. `sdc_support_chan_idx_in_adv_report` is not enabled since it
  alters the standard LE Advertising Report layout.

### Tests

- Host suite builds against the real IOsonata headers and USB core; the
  DeviceIntrf host stub is gone.

## 1.0.0

Initial HciController source release.

### Controller

- nRF52840 Bluetooth LE HCI controller using Nordic nrfxlib SoftDevice
  Controller and MPSL.
- Runtime HCI command dispatch with supported-command reporting kept in sync
  with the exposed command table.
- nRF52840 release profile reports Bluetooth Core 6.2 and includes the
  configured legacy, extended, periodic, PAwR, power-control, subrating,
  isochronous and supplemental Core command paths supported by the selected
  SDC library/profile.
- ACL host-credit guard and controller counters for accepted/refused traffic.

### Host transports

- Native Bluetooth USB HCI.
- Native USB Bulk Serialization alternate setting for HCI packet-indicator
  transport, including ISO.
- USB CDC carrying H:4 as a compatibility transport.
- UART H:4 for an on-board or external host processor.
- Independent USB CDC diagnostic log.

### Python validation library

- Reusable `hcicontroller` Python package under `python/hcicontroller/`.
- Direct access to serial H:4 and native Bluetooth USB HCI transports.
- Reusable command/event, pair, periodic advertising, PAwR, CIS/BIS, ISO,
  capability, DUT-control and result helpers.
- Editable installation from the repository with `pip install -e ./python`.
- Example program for controller discovery and identity access.
- HciController hardware/release harness imports the same public Python
  implementation directly and runs from the source tree without `PYTHONPATH`.
- Bumble remains supported when a complete Bluetooth host stack is preferable
  to direct HCI control.

### Runtime mode selection

- Persistent HCI mode selection on the UDG-NRF52840 family and IBK-NRF52840.
- UDG cycles USB H:4 and native USB HCI.
- IBK cycles UART H:4, USB H:4 and native USB HCI.
- Mode changes stop the HCI runtime and USB/SDC/MPSL before writing internal
  NVM, verify the record, then reset and load the selected mode before startup.
- Thingy:91, WildThing51 and WildThing91 remain UART-only.

### USB

- Composite native descriptor with Bluetooth HCI plus a CDC log function.
- Legacy Bluetooth USB HCI command/event/ACL endpoints.
- Bulk Serialization support and mode-safe packet reframing.
- Development USB identities by default, with a build-time guard for product
  builds that require assigned IDs.

### Test system

- Native C++ host tests for parser, routing, SDC dispatch/resources and USB
  state machines.
- Repository policy checks for board modes, command coverage and schemas.
- Official hardware/release harness under `tests/harness/` for two-controller
  and BLE-device testing.
- Counter-schema and hardware tooling consume the canonical public Python
  library sources.

### Known limitation

- nRF52840 does not support encrypted isochronous-channel packets in the
  current SoftDevice Controller. Unencrypted ISO remains available.
