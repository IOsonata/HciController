# nRF54LM20 HciController port

## Status

**Nordic nRF54LM20A DK native USB HCI: hardware validated (October 16, 2026).**
The target implementation is in `src/hci_nrf54lm20.cpp` and
`include/hci_nrf54lm20.h`; the nRF52840 target remains separate.

A Nordic nRF54LM20A DK and a UDG-nRF52840 completed the full
cross-platform native USB release suite: **66 PASS, 0 FAIL, 0 INCOMPLETE**.
The suite covered HCI commands, both ACL roles, DLE, 1M/2M/Coded PHY,
power control, Core 6.0/6.2 procedures, PAST, PAwR, CIS/BIS,
recovery, and **10,000 bidirectional ACL plus 1,000 bidirectional
ISO stress iterations**. Both boards also passed **100 reopen/Reset
cycles in each USB HCI alternate**, 200 per board.
These results are evidence for this tested DK configuration, not for
an untested board or a pair of nRF54LM20 units. The recurring ISO
sequence-gap reports did not prevent successful SDU validation.

Open `nRF54LM20/ioc/.project` in IOcomposer; select `Debug` or
`Release`. HciController is SDC-only: no `_SDC` configuration suffix
and no alternative SoftDevice build. Additional MBR/DFU configurations
are deferred until the DFU strategy and memory layout are defined.
The target uses Cortex-M33, `NRF54LM20B_XXAA`,
`NRF_APPLICATION`, `NRFXLIB_SDC`, IOsonata's
`IOsonata_nRF54LM20x`, `TaktOS_M33`, nrfxlib nRF54LM
hard-float multirole SDC/MPSL, and
`nrf54lm20a_xxaa_application.ld`. No separate SoftDevice firmware is linked.
**Board selection is separate from build configuration**:
`board.h` defaults to `UDG_NRF54LM20`, so a DK image must
explicitly define `BOARD=NORDIC_DK_NRF54LM20` in the build.

**Not yet hardware validated:** UDG-NRF54LM20, WildThing51
nRF54LM20 UART H:4 / USB CDC UART bridge, and any other board
variant. A clean Release build and final map inspection should be
recorded independently before packaging a release image.

The dependency is IOsonata `main`; the source/API baseline reviewed
during porting was `49f08107af29dcdd1f8a6bae3aae16fc7b5b1843`
(October 14, 2026).

## Existing dependencies to reuse

- IOsonata's nRF54LM20x MCU library, vectors, clock, USB controller and
  `src/usb/usb.cpp`. Do not copy a second USB stack into HciController.
- The generic `HciApp`, `HciController`, `HciSdc` command routing, and
  `BtHciUsb` class. The generic USB worker already runs on the HCI TaktOS
  worker through `UsbEvtQue`.
- IOsonata's existing nRF54LM20x TaktOS and USB examples as build-project
  references, rather than nRF52840 peripheral instances.
- IOsonata's `nrf54lm20a_xxaa_application.ld` for the current SDC/MPSL
  application. Do not introduce a separate SoftDevice configuration.
  Any future MBR/DFU layout requires its own design and validation.

## Target integration requirements

1. **Target adapter:** Implement `HciNrf54lm20Target()` and its start/stop,
   resource sizing, error tracking, MPSL processing, and IRQ forwarding without
   adding MCU checks to generic HCI sources.
2. **MPSL:** Nordic's nRF54L integration requires a 128 MHz CPU clock, GRTC
   running with SYSCOUNTER enabled *before* `mpsl_init`, and the reserved
   GRTC_3/TIMER10/TIMER20/ECB00 interrupt ownership. Do not reuse nRF52840's
   RTC0/TIMER0/RADIO/POWER_CLOCK vector setup. Implement the nRF54L low-latency
   acquire/release callbacks with the appropriate IOsonata NVM and clock
   primitives and verify their required linkage.
3. **SDC:** Audit the exact installed nrfxlib multirole SDC and MPSL archives
   for nRF54LM20A compatibility, HCI 6.2 feature coverage, required resources,
   IRQ names, and public APIs. The current generic command dispatcher assumes
   the newer SDC HCI command set. Do not substitute the S145 BLE stack just
   because IOsonata also supports it: that is a separate API and a different
   controller implementation.
4. **USB:** Use the IOsonata nRF54 USB driver and HCI USB class. The nRF54
   USB clock and cable/IRQ implementations are not the nRF52 POWER_CLOCK
   implementation. Ensure the controller's deferred completions run in the
   HCI worker and that a stop/start does not strand pending USB work.
5. **Firmware startup:** Move target selection and nRF52840-only
   `NRF_POWER->USBREGSTATUS` reads out of generic `src/main.cpp` into
   target/board policy, preserving the existing nRF52840 build. The DK pin
   mapping is only in `nRF54LM20/src/board.h`.
6. **Build:** Create the IOcomposer nRF54LM20 project using the appropriate
   IOsonata nRF54LM20x library, TaktOS, Nordic SDK headers, and verified
   nRF54L MPSL/SDC multirole archives. Inspect the .map for GRTC and
   interrupt handler ownership; treat duplicate vector definitions as errors.

## Validation gates

- Host regression suite for generic HCI and existing nRF52840 targets.
- Firmware compile/link with **real** Nordic headers and chosen binaries;
  no missing/weak-placeholder required callbacks and no duplicate IRQs.
- nRF54LM20 DK USB enumeration, CDC logging, native HCI alternate 0/1
  selection and repeated first Reset without retries.
- MPSL/SDC startup and radio operation with native USB active.
- Two-controller release profile including ACL roles, advertising,
  periodic advertising, PAST/PAwR (when actually supported), and ISO
  CIS/BIS. Features unavailable in the selected binary must be explicitly
  reported, not silently assumed from the nRF52840 profile.
- USB detach/reconnect, suspend/resume, TaktOS worker saturation, reset,
  and simultaneous USB/radio stress.

## Source references

- IOsonata: `ARM/Nordic/nRF54/nRF54LM20x/` (USB, MCU projects, TaktOS
  examples) and `ARM/Nordic/nRF54/ldscript/`.
- Nordic MPSL Integration Notes:
  https://github.com/nrfconnect/sdk-nrfxlib/blob/main/mpsl/doc/mpsl.rst
- Nordic SoftDevice Controller distribution:
  https://github.com/nrfconnect/sdk-nrfxlib/blob/main/softdevice_controller/README.rst

The nRF54L MPSL requirements above are from Nordic's integration notes.
The October 16, 2026 DK release-suite results demonstrate operation with the
tested MPSL/SDC binaries. Other nrfxlib revisions and board configurations
require their own link and hardware validation.

## Board builds

The IOcomposer project selects the MCU; the board selects physical transport
policy through `BOARD` in `nRF54LM20/src/board.h`.

| BOARD | HCI | Additional USB function | UART hardware |
| --- | --- | --- | --- |
| `UDG_NRF54LM20` (default, 200) | USB H:4 or native USB HCI | HciController diagnostic CDC | None |
| `NORDIC_DK_NRF54LM20` (201) | USB by default, UART optional | HciController diagnostic CDC | DK UARTE20 when UART selected |
| `WILDTHING51_NRF54LM20` (101) | nRF91 H:4 at 1 Mbaud without RTS/CTS | Dedicated nRF91 115200 baud UART bridge CDC plus controller diagnostic CDC | Two *different* UART instances |

**WildThing51 shared BLE footprint:** the supplied schematic (BLE sheet,
page 3) labels the module `BLYST840/LM20`. The four shared BTLTE nets
are used as **two independent UART TX/RX pairs**; RTS/CTS is not used:

| Function | nRF9151 | nRF52840 | nRF54LM20 |
| --- | --- | --- | --- |
| HCI: nRF91 TX → BLE RX | P0.00 | P0.23 | P1.14 |
| HCI: BLE TX → nRF91 RX | P0.01 | P0.24 | P3.04 |
| Trace: nRF91 TX → BLE RX | P0.02 | P0.21 | P3.05 |
| Trace: BLE TX → nRF91 RX | P0.03 | P1.04 | P1.13 |

The HCI UART runs at **1000000 baud** without hardware flow control. The
independent nRF91 bridge UART runs at **115200 baud** and is bridged to a
dedicated USB CDC interface. The nRF54LM20 configuration uses IOsonata
`UART_DEVNO=1` (UARTE20) for HCI and `HCI_BRIDGE_UART_DEVNO=2`
(UARTE21) for the UART bridge; verify the peripheral-to-pin routing during
the target build and hardware test. The nRF91 firmware must also select
two UARTs over these nets rather than use BTLTE2/BTLTE3 as flow-control
signals.

The existing `HCI_UART_BRIDGE` implementation in `hci_app.cpp` owns
the second UART and CDC class. It is enabled for WildThing51 and disabled for
UDG and the DK. Hardware validation is still required for USB enumeration,
the UART peripheral mappings, and sustained HCI/trace traffic.

## IOsonata main integration baseline

Verified source/API baseline:
`IOsonata/IOsonata` `main` at
`49f08107af29dcdd1f8a6bae3aae16fc7b5b1843`.

- Build the IOsonata nRF54LM20x library with SDC support and define
  `NRF54LM20B_XXAA`, `NRF_APPLICATION`, and `NRFXLIB_SDC`,
  and link `IOsonata_nRF54LM20x`, `TaktOS_M33`, nrfxlib multirole
  SDC, MPSL and MPSL FEM libraries for `nrf54lm/hard-float`.
- The HCI application implements the current `UsbEvtQue` callback
  and calls `UsbCheckStatus` when the worker queue empties. It uses
  IOsonata's USB device stack, `BtHciUsb` and `UsbdCdc`
  classes directly; do not copy controller drivers into HciController.
- HCI controller target owns its own nRF54L MPSL/SDC interrupts
  and deferred processing; IOsonata's `nrf_mpsl.cpp` is not an
  alternate vector implementation to be linked alongside it.
- IOsonata's `uart_nrfx.cpp` defines `DevNo=1` as UARTE20 and
  `DevNo=2` as UARTE21 for nRF54L; WildThing51 uses these
  separately for HCI and the nRF91 UART bridge.
- Run `python3 -m unittest tests/nrf54lm20_project_test.py` at the
  repository root, then build the IOsonata library with SDC support
  and the HciController IOC `Debug` / `Release` configuration. A static configuration review is **not** an ELF link
  or hardware test. Inspect the map for clock/vector ownership
  and reject unresolved or duplicate symbols.
