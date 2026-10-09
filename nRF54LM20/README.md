# nRF54LM20 HciController port

## Status

**Port source and IOC project added; ARM build and DK validation pending.**
The nRF54LM20 target is in `src/hci_nrf54lm20.cpp` and
`include/hci_nrf54lm20.h`; `src/main.cpp` selects it only on nRF54LM20.
The original nRF52840 target remains separate.

Open `nRF54LM20/ioc/.project` in IOcomposer. Select `Debug` or `Release`.
The build uses Cortex-M33, `NRF54LM20B_XXAA`, IOsonata's
`IOsonata_nRF54LM20x` library, `TaktOS_M33`, the nrfxlib nRF54LM
hard-float multirole SDC/MPSL archives, and the unbootloaded
`nrf54lm20a_xxaa_application.ld` script. No S145 binary is linked.

**Not yet proven runnable:** The actual ARM compile/link, map inspection,
and nRF54LM20 DK hardware tests have not been executed in this environment.
Do not merge this branch or use it as a release binary until those pass.

This port targets Nordic nRF54LM20A (DK PCA10184) and is developed separately
from HciController `main`. Its dependency is IOsonata
`prerelease_0.13` at or after `b515475734daef300db264dc7fef4c38ef5874a2`.

## Existing dependencies to reuse

- IOsonata's nRF54LM20x MCU library, vectors, clock, USB controller and
  `src/usb/usb.cpp`. Do not copy a second USB stack into HciController.
- The generic `HciApp`, `HciController`, `HciSdc` command routing, and
  `BtHciUsb` class. The generic USB worker already runs on the HCI TaktOS
  worker through `UsbEvtQue`.
- IOsonata's existing nRF54LM20x TaktOS and USB examples as build-project
  references, rather than nRF52840 peripheral instances.
- IOsonata's `nrf54lm20a_xxaa_application.ld` or an expressly selected
  S145 layout. These are *different* runtime contracts; use the application
  script for an nrfxlib SDC/MPSL runtime, and use an S145 script only with a
  compatible Nordic S145 runtime.

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

The nRF54L MPSL requirements above are from Nordic's integration notes;
whether the exact locally installed binary supports the LM20A **must be
verified at link and on hardware**.

## Board builds

The IOcomposer project selects the MCU; the board selects physical transport
policy through `BOARD` in `nRF54LM20/src/board.h`.

| BOARD | HCI | Additional USB function | UART hardware |
| --- | --- | --- | --- |
| `UDG_NRF54LM20` (default, 200) | USB H:4 or native USB HCI | HciController diagnostic CDC | None |
| `NORDIC_DK_NRF54LM20` (201) | USB by default, UART optional | HciController diagnostic CDC | DK UARTE20 when UART selected |
| `WILDTHING51_NRF54LM20` (101) | nRF91 H:4 at 1 Mbaud with RTS/CTS | Dedicated nRF91 115200 baud UART trace CDC plus controller diagnostic CDC | Two *different* UART instances |

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
independent nRF91 trace UART runs at **115200 baud** and is bridged to a
dedicated USB CDC interface. The nRF54LM20 configuration uses IOsonata
`UART_DEVNO=1` (UARTE20) for HCI and `HCI_NRF91_TRACE_UART_DEVNO=2`
(UARTE21) for the trace bridge; verify the peripheral-to-pin routing during
the target build and hardware test. The nRF91 firmware must also select
two UARTs over these nets rather than use BTLTE2/BTLTE3 as flow-control
signals.

The existing `HCI_NRF91_TRACE_BRIDGE` implementation in `hci_app.cpp` owns
the second UART and CDC class. It is enabled for WildThing51 and disabled for
UDG and the DK. Hardware validation is still required for USB enumeration,
the UART peripheral mappings, and sustained HCI/trace traffic.
