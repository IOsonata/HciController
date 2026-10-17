# nRF54LM20 HciController

HciController supports the nRF54LM20 target through
`src/hci_nrf54lm20.cpp` and `include/hci_nrf54lm20.h`.
The target uses IOsonata, TaktOS, and Nordic nrfxlib SoftDevice Controller
(SDC) with MPSL. It is independent of the nRF52840 target.

## Build

Open `nRF54LM20/ioc/.project` in IOcomposer and build the `Debug` or
`Release` configuration. Use the nRF54LM20 application linker script
`nrf54lm20a_xxaa_application.ld`, the `IOsonata_nRF54LM20x` and
`TaktOS_M33` libraries, and the compatible nrfxlib nRF54LM
hard-float multirole SDC/MPSL archives.

The build uses `NRF54LM20B_XXAA`, `NRF_APPLICATION`, and
`NRFXLIB_SDC`. No separate SoftDevice firmware is linked. Board selection
is independent of the Debug/Release configuration; the default board is
`UDG_NRF54LM20`. Specify `BOARD=NORDIC_DK_NRF54LM20` for the Nordic DK.

## Hardware validation

On October 16, 2026, the Nordic nRF54LM20A DK and UDG-nRF52840
completed the cross-platform native USB HCI release suite with
66 PASS, 0 FAIL, and 0 INCOMPLETE. Coverage included ACL roles,
PHY/DLE, power control, periodic advertising, PAwR, PAST, CIS/BIS,
recovery, bidirectional ACL/ISO stress, and repeated USB reopen/Reset.
The result applies to the tested DK configuration, not every nRF54LM20
board. Recurring ISO sequence-gap reports did not prevent successful SDU
validation. UDG-nRF54LM20 and WildThing51 nRF54LM20 board configurations
have not been hardware-validated in that suite.

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
| Bridge: nRF91 TX → BLE RX | P0.02 | P0.21 | P3.05 |
| Bridge: BLE TX → nRF91 RX | P0.03 | P1.04 | P1.13 |

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
the UART peripheral mappings, and sustained HCI/bridge traffic.

## Dependencies and interrupts

The nRF54L target uses its own GRTC/MPSL interrupt configuration rather
than the nRF52840 RTC0/TIMER0 setup. Its CPU clock must be 128 MHz and
GRTC SYSCOUNTER must be running before MPSL initialization. MPSL/SDC
resources and interrupt ownership are provided by the nRF54LM20 target;
do not link an additional MPSL vector implementation. USB is provided by
IOsonata's device stack and `BtHciUsb`, not a duplicated controller driver.

The USB worker calls `UsbCheckStatus` when its event queue is empty.
Firmware image validation requires a clean link and inspection of the
map for correct interrupt ownership and memory layout.

## Validation

Run `make -C tests run` with the matching nrfxlib installation.
For a release image, also verify the selected board's clean Release build,
USB enumeration, HCI Reset/reopen, and radio behavior on hardware.

