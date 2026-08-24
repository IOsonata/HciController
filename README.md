# HciController

HciController is an open-source Bluetooth LE HCI controller firmware for the
nRF52840. It is built with IOsonata and TaktOS, TinyUSB for USB device support,
and Nordic Semiconductor's nrfxlib SoftDevice Controller (SDC) and MPSL for the
radio.

Release 1 is version **1.0.0** (`FIRMWARE_VERSION 0x0100`) and targets the
nRF52840/current SDC capability profile reported by the firmware as Bluetooth
Core 6.2. This is an implementation/capability statement, not Bluetooth SIG
qualification or RF certification.

The controller boundary is packet-oriented: one `DeviceIntrf` transaction is
one complete HCI packet, selected by HCI packet type. The same controller code
therefore accepts three host transports:

- native Bluetooth USB HCI;
- USB CDC carrying H:4;
- UART carrying H:4.

UART and CDC are byte streams, so `HciIntrfTransport` adds/removes the H:4
indicator below the packet `DeviceIntrf`. Native USB is already packet-oriented
and plugs into the same controller without an H:4 adapter.

The repository also includes the reusable Python HCI/validation library used by
the hardware release tests. User validation programs can drive HciController
directly through that library or use Bumble when a complete Bluetooth host stack
is required.

See also:

- [GETTING_STARTED.md](GETTING_STARTED.md) to flash the released firmware, select an HCI mode, and start using HciController;
- [python/README.md](python/README.md) for the direct Python HCI and BLE validation library;
- [BUILDING.md](BUILDING.md) to install IOcomposer and build HciController from source;
- [USB-HCI.md](USB-HCI.md) for the native USB transport;
- [README-commands.md](README-commands.md) for command/profile coverage;
- [CHANGELOG.md](CHANGELOG.md) for release changes.

## Host modes and persistent selection

Transport selection is runtime on boards that provide a mode button. Build-time
settings choose the default used when there is no valid stored mode; after that,
the selected mode is stored in NVM and survives reset and power cycle.

| Board | HCI modes | Mode-button sequence |
| --- | --- | --- |
| I-SYST UDG-NRF52840x | USB H:4, native USB HCI | USB H:4 <-> native |
| I-SYST IBK-NRF52840 | UART H:4, USB H:4, native USB HCI | UART -> USB H:4 -> native -> UART |
| Nordic Thingy:91 | UART H:4 only | none |
| I-SYST WildThing51 | UART H:4 only | none |
| I-SYST WildThing91 | UART H:4 only | none |

The default policy is controlled by `HCI_HOST_SELECT`:

```text
HCI_HOST_SELECT_AUTO   choose the initial USB/UART family from VBUS where legal
HCI_HOST_SELECT_USB    choose the configured USB transport
HCI_HOST_SELECT_UART   choose UART H:4
```

For USB, `HCI_USB_HCI_TRANSPORT` chooses the default USB transport:

```text
HCI_USB_HCI_TRANSPORT_NATIVE   native Bluetooth USB HCI
HCI_USB_HCI_TRANSPORT_CDC_H4   USB CDC carrying H:4
```

On UDG, UART HCI is not allowed. On Thingy:91 and WildThing boards, USB HCI is
not allowed. Invalid board/mode combinations are rejected rather than left to
fail after startup.

### How a mode change is stored

Internal flash is not written while MPSL/SDC owns the radio. A confirmed button
press performs this sequence:

```text
button debounce
    -> choose next board-legal mode
    -> stop HCI runtime
    -> stop USB, SDC and MPSL
    -> erase/write/verify the NVM mode record
    -> system reset
    -> load the persisted mode before USB/radio startup
```

The mode does not depend on reset reason and is not handed through
GPREGRET/GPREGRET2 or another bootloader-owned retained register.

The startup log prints the linker-selected NVM address, for example on the UDG
USB-DFU build:

```text
mode: nvm addr=0x000DD000 size=12288
```

## NVM and bootloader memory maps

The linker owns `NVM0`. This is important on nRF52 because different bootloaders
occupy different high-flash regions. The HCI mode code asks IOsonata for the
linker-defined NVM region; it does not guess a flash address in C++.

The nRF52840 IOcomposer project has these configurations:

| Configuration | Intended layout | Linker script |
| --- | --- | --- |
| `Debug` | no bootloader/debug | `nrf52840_xxaa_sdc.ld` |
| `Release` | no bootloader | `nrf52840_xxaa_sdc.ld` |
| `Release_MBR` | USB DFU/MBR | `nrf52840_xxaa_sdc_mbr.ld` |
| `Release_SD` | S140-compatible application origin | `nrf52840_xxaa_s140_sdc.ld` |

The configuration name selects a linker layout; it does not select a board.
The current IOsonata linker-owned NVM regions are:

```text
USB DFU/MBR:     NVM0 0xDD000 .. 0xDFFFF
S140-compatible: NVM0 0xDD000 .. 0xDFFFF
no bootloader:   NVM0 0xFD000 .. 0xFFFFF
```

The current S140-compatible script starts application flash at `0x27000` and
ends it at `0xDCFFF`; it also reserves `BT_PDS` at `0xFD000..0xFEFFF` and the
bootloader-settings page at `0xFF000..0xFFFFF`. It does not define an `NVM0`
region at `0xF5000`, and it does not define an OTA bootloader region itself.
Any installed bootloader must therefore be checked separately against the exact
linker-script revision and generated map used for the build.

HciController uses one erase page at the start of `NVM0` for its mode record.
The rest of the region remains available to the platform/application layout.

For local builds, verify the generated `.map`; the configuration name alone does
not prove that the external IOsonata linker script has the intended address.
See [BUILDING.md](BUILDING.md).

## Native USB and the log port

Native mode is a composite USB device containing:

- a Bluetooth Controller USB function;
- an independent CDC diagnostic log.

The log is never an HCI transport in native mode. CDC assignment is:

```text
native Bluetooth USB HCI   Bluetooth function + CDC 0 log
USB CDC/H:4                CDC 0 HCI H:4 + CDC 1 log
UART H:4 / log-only USB    CDC 0 log
```

`HciTrace` writes into a RAM ring that the runtime drains to the log port. The
log path is non-blocking and is available without semihosting. `HCI_TRACE=1`
adds the semihosting copy; it is not required for the USB log.

See [USB-HCI.md](USB-HCI.md) for the endpoint layout, Bulk Serialization and USB
state-machine behavior.

## Python HCI and BLE validation library

The host-side Python implementation used by the HciController release harness is
available as an installable package under `python/hcicontroller/`.

From the repository root:

```sh
python3 -m pip install -e ./python
```

The package supports both native Bluetooth USB HCI and serial H:4 and includes
reusable command/event, pair, periodic advertising, PAwR, CIS/BIS, HCI ISO,
capability, DUT-control and result helpers.

Use the included package when the validation software needs direct HCI control,
including exact controller procedures or timing required by a custom BLE
product. Use Bumble when the validation program needs a complete Bluetooth host
stack with higher-level GAP, GATT, L2CAP or related host protocols.

A minimal direct-HCI program is:

```python
from hcicontroller import Hci, addr_str, discover

spec = discover(kind="usb")
if spec is None:
    raise RuntimeError("no HciController found")

hci = Hci(spec)
try:
    hci.setup()
    identity, _, source = hci.identity()
    print(addr_str(identity), source)
finally:
    hci.close()
```

For BLE product validation, HciController can supply the programmable peer/radio
side while product-specific automation controls the DUT independently:

```text
Python validation program
        |
        | direct HCI library or Bumble
        v
UDG-NRF52840 + HciController
        |
        | Bluetooth LE over the air
        v
product under test
```

`hcicontroller.DutControl` defines optional DUT hooks for UART, USB, RPC, GPIO or
a dedicated product test interface. The DUT does not need to use IOsonata.

See [python/README.md](python/README.md) and
[tests/harness/ble_device/README.md](tests/harness/ble_device/README.md).

## USB development identity

The source tree intentionally defaults to development USB identities:

```text
VID       0xCAFE
CDC/H4    PID 0x4070
native    PID 0x4071
log-only  PID 0x4072
bcdDevice 0x0100
```

Product builds can override `HCI_USB_VID`, `HCI_USB_PID_CDC_H4`,
`HCI_USB_PID_NATIVE_HCI` and `HCI_USB_PID_LOG_ONLY`.

A shipping build should define:

```text
HCI_USB_REQUIRE_ASSIGNED_IDS=1
```

so compilation fails if a development VID/PID remains selected. The USB
`bcdDevice` value comes from `FIRMWARE_VERSION`; there is no second USB version
to keep synchronized.

## Reference hardware

### UDG-NRF52840x

The primary USB target is the I-SYST UDG-NRF52840x built on the BLYST840
nRF52840 module.

| Part | Connector |
| --- | --- |
| `UDG-NRF52840` | USB Type-A |
| `UDG-NRF52840C` | USB Type-C |

The BLYST840 provides 32 MHz and 32.768 kHz crystals. The dongle has a USB DFU
bootloader, user RGB LED, user button, reset button, SWD/Tag-Connect access and
edge I/O.

Product page: https://www.i-syst.com/products/usb_dongle

### IBK-NRF52840

IBK is the breakout/development board. It allows UART H:4 as well as both USB
modes. Its user button cycles all three modes and the selection is persistent.
UART pins in `board.h` are development defaults and must match the actual bench
wiring. The board selection is independent of the IOcomposer build
configuration; set `BOARD=IBK_NRF52840` when building IBK firmware.

### Thingy:91

On Nordic Thingy:91, the nRF52840 HCI host is the nRF9160 over the interconnect
UART. USB is used for diagnostics, not for HCI. The measured nRF52840 mapping is:

```text
nRF52840 side   TX P0.25   RX P1.00   RTS P0.19   CTS P0.22
nRF9160 side    TX P0.22   RX P0.23   RTS P0.24   CTS P0.25
rate            1,000,000 bit/s
flow control    hardware
```

The measured RTS/CTS assignment on the nRF52840 differs from the sdk-nrf pinctrl
labels. `board.h` documents the measurement and must not be changed from the
pinctrl labels without another hardware measurement.

The host side should use:

```text
CONFIG_BT_WAIT_NOP=y
```

The nRF52840 emits the startup No Operation Command Complete expected by that
startup scheme. H:4 startup synchronization also discards boot text that may
precede the first HCI packet on the shared interconnect UART.

### WildThing51 and WildThing91

Both are UART-H4-only controller configurations. Their interconnect pins and
flow-control policy are in `nRF52840/src/board.h`; they do not enter the runtime
mode-switch state machine.

## Repository layout

```text
include/                public HCI/controller headers
src/                    generic HCI, transport, USB and nRF52840 target sources
nRF52840/ioc/           IOcomposer nRF52840 project
nRF52840/src/           board policy and pin mapping
python/hcicontroller/   reusable Python HCI/BLE validation library
python/examples/        user-facing Python examples
tests/                  native host tests and repository checks
tests/harness/          official hardware/release test system
```

## IOcomposer development environment

Install IOcomposer for the host platform before building HciController. The
IOcomposer installer prepares the required compiler toolchains, SDK/external
repositories, IOsonata, TaktOS, HciController, and the expected workspace. No
manual SDK or dependency setup is required for the normal build path.

The normal workspace is:

```text
~/IOcomposer/
    external/
        nrfx/
        sdk-nrfxlib/
        tinyusb/
    IOsonata/
    HciController/
    TaktOS/
```

Open `HciController/nRF52840/ioc` in IOcomposer and select the required board and
build configuration. Build outputs for the active configuration are:

```text
HciController.elf
HciController.hex
HciController.bin
HciController.map
```

See [BUILDING.md](BUILDING.md) for installation and build steps and
[nRF52840/ioc/README.md](nRF52840/ioc/README.md) for configuration details.

For reproducibility, record the exact IOsonata, TaktOS, TinyUSB, nrfx and
sdk-nrfxlib revisions used by the build. The installer normally follows
repository HEADs, so the HciController tag by itself does not identify all
binary inputs.

## Architecture

```text
native USB HCI ---> HciUsb packet DeviceIntrf -------------------+
                                                                  |
USB CDC/H:4 ---> UsbdCdcIntrf ---> H:4 packet adapter -----------+--> HCI controller
                                                                  |
UART H:4 ------> UART -----------> H:4 packet adapter -----------+
                                                                  |
                                                        nrfxlib SDC / MPSL
```

At the HCI controller boundary, `DevAddr` is the HCI packet type and the data
buffer contains the complete packet without an H:4 indicator.

The HCI runtime is a high-priority TaktOS thread. USB, UART, MPSL and SDC
interrupt/callback paths wake it and leave command parsing and typed SDC access
in thread context. On boards with a mode switch, a short critical-priority
control thread polls/debounces the button so a continuously ready HCI thread
cannot starve the control path.

Target-specific clock, USB peripheral, interrupt, SDC and MPSL code is isolated
behind `HciTarget_t`. The parser, H:4 transport, routing, command dispatch,
counters and resource profile do not depend on a concrete UART or USB class.

## Controller capability profile

The release command surface is not documented as a hand-maintained opcode count.
The dispatch tables, supported-command bitmap, Python command catalog and
target-profile coverage are compared by the host test suite.

Major configured feature families include:

- Central and Peripheral roles;
- legacy and extended advertising/scanning/initiating;
- privacy and resolving list;
- LE Data Length and PHY control;
- periodic advertising, synchronization and sync transfer;
- PAwR resources;
- LE Power Control, SCA update and connection subrating;
- Extended Feature Set, Frame Space Update and Shorter Connection Intervals;
- connected and broadcast isochronous channels;
- controller/vendor diagnostics.

See [README-commands.md](README-commands.md) for how the exposed command surface
is checked.

## Isochronous limitation on nRF52840

The configured nRF52840 controller supports unencrypted connected and broadcast
isochronous operation and HCI ISO transport. The current nRF52840 SoftDevice
Controller does not support encrypted isochronous-channel packets.

This is not an HCI USB limitation. nRF52840 lacks the radio-path CCM header-mask
support used by the nRF52820/nRF52833 controller implementations for encrypted
ISO packets. Software-side AES cannot replace that link-layer operation.

Do not claim encrypted ISO support for the nRF52840 release.

## Tests

Run the host/source suite with the IOcomposer workspace installed:

```sh
make -C tests clean
make -C tests run
```

The host suite exercises parser/routing behavior, SDC dispatch/resources, USB
state machines, command/catalog consistency, counter schemas, board policy and
persistent mode-switch wiring. The Python-side source/schema checks consume the
canonical implementation under `python/hcicontroller/`.

Hardware and over-the-air testing lives under `tests/harness/` and uses the same
public Python implementation.

For two native USB controllers:

```sh
python3 tests/harness/hcicontroller/release_test.py \
    --transport usb --a SERIAL_A --b SERIAL_B
```

Focused native USB CIS/ISO testing is available through:

```sh
python3 tests/harness/hcicontroller/cis_usb_pair_test.py
```

A feature advertised by the release must be exercised positively. A test that
cannot create the state required for an advertised feature is incomplete, not a
passing skip.

See [tests/README.md](tests/README.md) and
[tests/harness/README.md](tests/harness/README.md) for test organization and
hardware harness usage.

## Design constraints

- IOsonata packet `DeviceIntrf` injection at the controller boundary;
- H:4 framing only on UART/CDC byte streams;
- runtime host-mode selection where the board permits it;
- bootloader-specific memory layout selected by the linker;
- static/caller-owned storage;
- no heap in controller/real-time paths;
- no exceptions or RTTI in the embedded build;
- no Zephyr dependency in the controller firmware;
- IOsonata and TaktOS remain separately built libraries.

## License

HciController source written by I-SYST inc. is licensed under the Mozilla Public
License 2.0. External components retain their own licenses.

Copyright (c) 2026 I-SYST inc.
