# Changelog

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

### Runtime mode selection

- Persistent HCI mode selection on UDG-NRF52840x and IBK-NRF52840.
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

### Known limitation

- nRF52840 does not support encrypted isochronous-channel packets in the
  current SoftDevice Controller. Unencrypted ISO remains available.
