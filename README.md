# HciController

Bluetooth LE HCI controller firmware for the I-SYST UDG-NRF52840x dongle. It
presents a standard Bluetooth HCI H:4 byte stream that any Bluetooth host
stack can drive.

Built with IOsonata and TaktOS, with TinyUSB for the USB device stack and
Nordic Semiconductor's nrfxlib SoftDevice Controller and MPSL for the radio.

The H:4 stream is carried over an IOsonata `DeviceIntrf`. One firmware image
supports two host connections:

- USB CDC through IOsonata `UsbdCdcIntrf`;
- UART to an nRF9151 or another host processor.

The HCI controller, the H:4 parser, the SDC binding and the TaktOS execution
path are identical for both.

## Host interface selection

Which port the controller talks to its host on is a build option,
`HCI_HOST_SELECT`:

```text
HCI_HOST_SELECT_AUTO   read VBUS at reset, powered is USB CDC and otherwise UART
HCI_HOST_SELECT_USB    always USB CDC
HCI_HOST_SELECT_UART   always UART
```

`nRF52840/src/board.h` picks a default for each board, and `-DHCI_HOST_SELECT=...`
on the command line wins over it:

```sh
# a dongle image, VBUS decides
arm-none-eabi-g++ ...

# force a dongle to come up on its UART
arm-none-eabi-g++ -DHCI_HOST_SELECT=HCI_HOST_SELECT_UART ...

# a board whose host is another part on the same PCB
arm-none-eabi-g++ -DBOARD=MY_BOARD -DHCI_HOST_SELECT=HCI_HOST_SELECT_UART ...
```

AUTO only means something where the USB socket belongs to the nRF52840, which
is what a dongle is. Where the socket belongs to something else, VBUS reads as
a host that is not there: a board on a charger would come up talking USB CDC to
nobody while the real host waited for an answer over the UART. A board whose
host is another part on the same PCB should name UART outright.

Selecting a UART host with no `UART_PINS` in `board.h` is refused at compile
time rather than left to fail at startup, where nothing would be on the wire to
say why.

The selected concrete interface is passed to the HCI controller as a `DevIntrf_t *`, in the same way an IOsonata device driver can receive either an SPI or I2C interface.

The UART configuration for the BLYST840/nRF9151 product is:

```text
BLYST840 TXD: P0.24
BLYST840 RXD: P0.23
Rate:         1,000,000 bit/s
Format:       8-N-1
Flow control: none
Framing:      Bluetooth HCI H:4
```

The UART connection is crossed at the product level:

```text
BLYST840 P0.24 TXD -> nRF9151 RXD
BLYST840 P0.23 RXD <- nRF9151 TXD
```

## The dongle

The reference target is the I-SYST UDG-NRF52840x, an nRF52840 USB dongle built
on the I-SYST BLYST840 module. Two variants, differing only in the USB
connector:

| Part | Connector |
| --- | --- |
| `UDG-NRF52840` | USB Type-A |
| `UDG-NRF52840C` | USB Type-C |

Both are 31 x 16 x 4 mm with a real connector rather than a bare PCB edge, and
both carry a user LED, a user RGB LED, a user button, a reset button and 10
GPIO on edge castellations. The BLYST840 module brings both oscillators the
radio wants, a 32 MHz and a 32.768 kHz crystal at 20 ppm, so nothing here runs
the radio off an RC.

The module is certified rather than merely compliant: FCC ID `2ALTY-IBTZ840`,
IC `25671-IBTZ840`, and an EU Declaration of Conformity under RED 2014/53/EU
that names `UDG-NRF52840` and `UDG-NRF52840C` by part number. That matters if a
dongle has to travel to a customer site or live in a regulated lab.

The dongle carries a USB bootloader, so a first image needs no debugger. A
Tag-Connect debug port is present for SWD when one is wanted, with nothing to
solder.

### Where to buy

Both variants are stocked by Mouser and DigiKey. Prices and stock move, so
these are links rather than quoted numbers.

**UDG-NRF52840**, USB Type-A

- Mouser [392-UDG-NRF52840](https://www.mouser.com/ProductDetail/392-UDG-NRF52840)
- DigiKey [25675543](https://www.digikey.com/en/products/detail/i-syst/UDG-NRF52840/25675543)

**UDG-NRF52840C**, USB Type-C

- Mouser [392-UDG-NRF52840C](https://www.mouser.com/ProductDetail/392-UDG-NRF52840C)
- DigiKey [25675547](https://www.digikey.com/en/products/detail/i-syst/UDG-NRF52840C/25675547)

Product page: [i-syst.com/products/usb_dongle](https://www.i-syst.com/products/usb_dongle)

## Repository layout

```text
include/            HCI headers
src/                HCI sources
nRF52840/ioc/       Eclipse Embedded CDT project
nRF52840/src/       board.h, the pin and clock configuration
tests/              host tests and hardware tools
```

## Boards

`nRF52840/src/board.h` selects the board with `BOARD`. It holds the I-SYST
boards this firmware is developed and tested on, the UDG-NRF52840x dongle and
the IBK-NRF52840 breakout, and one board that is not I-SYST hardware: the
Nordic Thingy:91, whose nRF52840 reaches its host over the interconnect UART
to the nRF9160.

The Thingy:91 pins come from `sdk-nrf`,
`boards/nordic/thingy91/thingy91_nrf52840-pinctrl.dtsi`, the `uart1` node:

```text
nRF52840 side   TX P0.25   RX P1.00   RTS P0.22   CTS P0.19
nRF9160 side    TX P0.22   RX P0.23   RTS P0.24   CTS P0.25
Rate            1,000,000 bit/s, both board files
Flow control    hardware, all four wires exist
```

That board defaults to `HCI_HOST_SELECT_UART`, because its USB socket has
nothing behind it that speaks HCI and AUTO would come up talking to a host
that is not there. It also sets `HCI_STATUS_LEDS 0`, since the Thingy:91 LEDs
are driven from the nRF9160.

Other hardware is a port, and it is a small one. A board says four things
beyond its pins:

| | |
| --- | --- |
| `HCI_HOST_SELECT` | the host port, described above |
| `HCI_STATUS_LEDS 0` | where no status LED is reachable from this part, so nothing drives pins that belong to something else |
| `UART_HW_FLOWCTRL 1` | where the peer drives RTS and CTS |
| `MCU_OSC` | where the low frequency clock is not the default |

Add an id, add a branch to the `#if` chain, name the pins. Nothing else in the
tree is board-aware.

Flow control and the pin map are built together at the end of `board.h` rather
than per board, because the two have to agree. Asking the peripheral for
hardware flow control without RTS and CTS in the map gets a link that never
sends; putting them in the map without asking for flow control drives two pins
the peripheral never uses. A board that sets `UART_HW_FLOWCTRL` without naming
RTS and CTS is refused at compile time, rather than failing further down on an
undeclared macro inside the pin map, which would not say what is missing.

RTS is an output and has to meet the peer's CTS, so a port has to agree with
what the other side names each wire. Reversed, the board comes up, initialises
cleanly and never transmits, and nothing in the firmware can tell.

Where a board's stock firmware was doing something else over USB, a serial
bridge or a sniffer, replacing it with this takes that with it.

## Prerequisites

The firmware is built with IOcomposer, which supplies the ARM toolchain,
OpenOCD, the SDK integration and the Eclipse project support the
`nRF52840/ioc` project depends on.

### 1. Install IOcomposer

[IOcomposer](https://iocomposer.io) installs into `~/IOcomposer`, or
`%USERPROFILE%\IOcomposer` on Windows. That directory is the workspace root
everything else sits under.

**macOS**

```bash
curl -fsSL https://iocomposer.io/install_ioc_macos.sh -o /tmp/install_ioc_macos.sh && bash /tmp/install_ioc_macos.sh
```

**Linux**

```bash
curl -fsSL https://iocomposer.io/install_ioc_linux.sh -o /tmp/install_ioc_linux.sh && bash /tmp/install_ioc_linux.sh
```

**Windows, PowerShell as Administrator**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -Command "irm https://iocomposer.io/install_ioc_windows.ps1 | iex"
```

### 2. Lay out the workspace

Dependencies stay separate sibling repositories and external SDKs. The Eclipse
project reaches them by relative path, so the layout matters:

```text
~/IOcomposer/
    external/
        tinyusb/
        nrfx/
        sdk-nrfxlib/
    IOsonata/
    HciController/
    TaktOS/
```

### 3. Build the libraries

HciController does not compile IOsonata or TaktOS sources, it links them. Build
them first with the IOsonata library builder and select nRF52840. TaktOS is
built automatically when it is installed.

**macOS**

```bash
bash ~/IOcomposer/IOsonata/Installer/build_iosonata_lib_macos.sh
```

**Linux**

```bash
bash ~/IOcomposer/IOsonata/Installer/build_iosonata_lib_linux.sh
```

**Windows, PowerShell**

```powershell
& "$env:USERPROFILE\IOcomposer\IOsonata\Installer\build_iosonata_lib_win.ps1"
```

That produces what the firmware links against:

```text
libIOsonata_nRF52840.a  from  IOsonata/ARM/Nordic/nRF52/nRF52840/lib/Eclipse/Debug_SDC
libTaktOS_M4.a          from  TaktOS/ARM/cm4/Eclipse/DebugFPU
```

The host tests under `tests/` need none of this. They build with any C++14
compiler, so the HCI layers can be read and exercised without a toolchain
install or a board.

## Eclipse Embedded CDT

Import `HciController/nRF52840/ioc` as an existing project. It is a Managed CDT
Arm Cross GCC project, generated from the IOsonata Eclipse Embedded CDT
template.

After updating project metadata, remove the old workspace project without
deleting its files, delete `nRF52840/ioc/Debug`, and re-import it. Eclipse
caches linked resources and generated makefiles.

The application project compiles:

- HciController sources;
- TinyUSB device sources.

It links:

- `libIOsonata_nRF52840.a` from `Debug_SDC`;
- `libTaktOS_M4.a` from `DebugFPU`;
- nrfxlib MPSL;
- nrfxlib MPSL FEM common;
- nrfxlib multirole SoftDevice Controller.

Build outputs are written under `nRF52840/ioc/Debug`:

```text
HciController.elf
HciController.hex
HciController.bin
HciController.map
```

## Architecture

```text
                         +-------------------+
USB VBUS present ------> | UsbdCdcIntrf      | -- TinyUSB CDC
                         +-------------------+
                                  |
                                  | DevIntrf_t *
                                  v
                         +-------------------+
USB VBUS absent -------> | UART              | -- P0.24/P0.23
                         +-------------------+
                                  |
                                  v
                         +-------------------+
                         | HCI H:4 transport  |
                         +-------------------+
                                  |
                         +-------------------+
                         | HCI controller     |
                         +-------------------+
                                  |
                         +-------------------+
                         | nrfxlib SDC / MPSL |
                         +-------------------+
```

The H:4 transport uses only `DeviceIntrfRx()` and `DeviceIntrfTx()`. It does not include TinyUSB or UART-specific code.

The TinyUSB adapter services the IOsonata `UsbdCdcIntrf` FIFOs. When the RX FIFO has no space, it stops reading the TinyUSB OUT endpoint so USB backpressure is preserved rather than dropping HCI bytes.

The UART path uses IOsonata's interrupt-driven FIFO/DMA UART implementation. Its 4 KiB RX and TX FIFOs absorb complete HCI packets and host scheduling latency.

## Ports

The tree is split so that a part supplies hardware and nothing else.

```text
include/hci_target.h          what the application needs from a part
include/hci_sdc_resources.h   what the controller is configured for
src/hci_sdc_resources.cpp     every sdc_support_ call, the sdc_cfg_set sequence
include/hci_nrf52840.h        the nRF52840 port: clock, USB, errata
src/hci_nrf52840.cpp          its bring up, and the HciTarget_t it publishes
```

Nothing above the radio names a part. `hci_app` holds an `HciTarget_t`, which
is a table of function pointers and an opaque instance the port owns, and
`main.cpp` decides which port that is in one line.

The resource configuration is deliberately not part of a port. nrfxlib ships
one `sdc.h` covering nrf52, nrf53, nrf54h, nrf54l, nrf54lm, nrf54ls, nrf54lv
and nrf71, and every `SDC_MEM_` macro the pool is computed from comes from it.
A port does not get an opinion about how many links the controller supports.

Adding a part means one header and one source:

| | |
| --- | --- |
| a state structure | whatever its clock, USB and errata need |
| `Init` | MPSL, the entropy source, `sdc_init`, then `HciSdcResourcesApply()`, then `sdc_enable` |
| `GetTaktOsOps` | how the runtime starts it and pumps MPSL |
| `UsbStart`, `UsbPassMark`, `UsbPowerProcess` | or null on a part with no USB device peripheral, and the application skips them |
| `Stop` | |
| `LastError` | |
| one `HciTarget_t` returned by value | the instance is static in the port, because a board has one radio |

The dispatch table, the H:4 parser, the transport, the bridge, the counters and
the resource configuration are then already correct for it.

## Capacity

What one image is configured for, and what it costs in the SoftDevice
Controller memory pool. Every value is a constant in
`include/hci_sdc_resources.h`, and the pool is computed from them, so changing
a count grows the array that holds it rather than needing a second edit. There are no build options here:
configuring the controller differently means editing the value.

| | | Pool cost |
| --- | ---: | ---: |
| Peripheral links | 4 | 2935 each |
| Central links | 4 | 2839 each |
| ACL payload, each way | 251 octets | in the per-link cost |
| ACL buffers, each way | 4 | in the per-link cost |
| Advertising sets | 3 | 961 each |
| Advertising data | 255 octets | in the per-set cost |
| Scan buffers | 4 | 1688 for four |
| Filter accept list | 8 | 68 for eight |
| Channel survey | on | 40 |
| LE Power Control | on | 997 for eight links |
| Connection subrating | on | 492 for eight links |
| Extended feature pages | 10 | 2083 for eight links |
| Scan and initiate together | on | 384 |
| Periodic advertising sets | 1 | 753 each |
| Periodic syncs | 2 | 1787 each, four buffers, with responses |
| Periodic advertiser list | 8 | 64 for eight |
| Periodic sync transfer | on | 1125 for eight links |
| Periodic sets with responses | 1 | 1575 each |
| Connected isochronous groups | 2 | 339 for two |
| Connected isochronous streams | 4 | 2201 for four |
| Broadcast isochronous groups | 2 | 675 for two |
| Broadcast isochronous streams | 2 source, 2 sink | 1049 for four |
| Isochronous SDU, transmit | 247 octets, 4 buffers | 1196 |
| Isochronous SDU, receive | 251 octets, 4 buffers | 1064 |
| Isochronous PDU, per stream | 3 each way | 5280 transmit, 5280 receive |
| | | **55944 total** |

The pool is that total plus a 512 octet margin, because sdk-nrfxlib says the
memory macros may move between minor releases and the number that decides
whether the controller starts is the one `sdc_cfg_set` answers at run time.

Isochronous channels are the largest single item, 17084 of the total. They
have their own section below, because what this part does and does not do
with them takes more than a paragraph.

The ACL payload is worth calling out. 251 octets is the data length extension
maximum, and it is what the controller reports in LE Read Buffer Size, so a
host is entitled to use it. The common alternative is 27, which caps
throughput at about a ninth of what the radio can carry.

The buffer count is a total across every link, not an allowance each. Vol 4
Part E 4.1.1 gives the host one pool to spend, and the controller refuses a
packet past it rather than letting the SoftDevice Controller take the packet
and the host's buffer with it. `AclCreditOverrunCount`, counter 30, says how
often that has happened.

## Isochronous channels

The short version: unencrypted isochronous works on this part, encrypted does
not, and the reason is one missing register rather than anything about
isochronous transport or about the part's cryptography.

### What works

Every isochronous entry point is present in the nRF52 SoftDevice Controller
library and all four roles are enabled here:

| Role | `sdc_support_` call |
| --- | --- |
| Connected stream, central | `sdc_support_cis_central` |
| Connected stream, peripheral | `sdc_support_cis_peripheral` |
| Broadcast stream, source | `sdc_support_bis_source` |
| Broadcast stream, sink | `sdc_support_bis_sink` |

That is 24 opcodes, the group and stream setup, the data path, the four
isochronous test commands, and `sdc_hci_iso_data_put` for the data itself.
`LE Read Buffer Size v2` is part of the set rather than an extra: version 1
reports only the ACL packet length and count, so without v2 a host has every
command it needs to build a stream and no way to flow control it.

### What does not, and why

sdk-nrfxlib `README.rst` states the limit:

> For the Isochronous Channels features, nRF52820 and nRF52833 are the nRF52
> Series devices that support encrypting and decrypting the Isochronous
> Channels packets.

nRF52840 is not in that list. The cause is visible in the register maps. CCM
authenticates the PDU header octet, and which bits of it are authenticated
differ between an ACL data PDU and an isochronous one:

```text
nrf52840  CCM: ... MAXPACKETSIZE, RATEOVERRIDE
nrf52833  CCM: ... MAXPACKETSIZE, RATEOVERRIDE, HEADERMASK
nrf52820  CCM: ... MAXPACKETSIZE, RATEOVERRIDE, HEADERMASK
```

`CCM.HEADERMASK` is documented as the header (S0) mask, eight bits wide.
Without it the CCM applies the fixed ACL mask, which is the wrong additional
authenticated data for an isochronous PDU and so the wrong MIC.

This is worth stating plainly because it inverts the obvious reading: the
nRF52840 is the one of the three parts with a CryptoCell. It has more
cryptographic hardware than the other two, not less. What it lacks is one
register in the CCM accelerator that sits in the radio datapath, and nothing
in software reaches around it. IOsonata's crypto engines are host side and
cannot substitute: the one Bluetooth adjacent engine, `CryptoCtlrSdc`, sends
HCI `LE Encrypt` to borrow the controller's AES block for pairing, which is a
different thing entirely from link layer CCM inside the controller.

### What this means in practice

**Broadcast is unaffected.** Encryption is per group and optional: pass
`Encryption = 0` to LE Create BIG and it is a public broadcast. Nothing about
an unencrypted broadcast is degraded or non-conformant.

**Connected streams have an open question.** A CIS does not have its own
encryption switch the way a BIG does. What happens when a CIS is created over
an already encrypted ACL link has not been measured on this part, and this
document will not guess at it. The test is cheap now that pairing works:
bring up an encrypted link, send LE Set CIG Parameters and then LE Create
CIS, and read the status. Until that is run, treat unencrypted CIS between
two of these dongles as the supported case.

If encrypted isochronous is a requirement, it is a part change to nRF52820 or
nRF52833. It is not something this firmware can be made to do.

### Configuration

Two connected groups, four connected streams, two broadcast groups, two
broadcast source and two broadcast sink streams. Transmit SDU 247 octets,
receive SDU 251, four buffers each way, three protocol units per stream each
way. 17084 octets of the pool, the largest single item in it.

The receive SDU size is also what the packet buffer above the controller is
sized against, rather than the 4095 octet ceiling the specification allows,
because `sdc_hci.h` ties the `sdc_hci_get` requirement to
`sdc_cfg_iso_buffer_cfg_t::rx_sdu_buffer_size`. 251 plus the 12 octet
isochronous header is 263, which the existing 1024 octet packet buffer holds,
so isochronous costs no extra packet buffer at all. `src/hci_app.cpp` asserts
this against the configured size, so raising the SDU without raising the
buffer stops the build.

## TaktOS execution model

A high-priority HCI thread owns the low-priority path:

- selected host-interface servicing;
- H:4 parsing and packet delivery;
- HCI command dispatch;
- `mpsl_low_priority_process()`;
- SoftDevice Controller command, ACL, ISO, and event access.

USB, UART, MPSL, and SDC callbacks only wake the thread. HCI parsing and typed SDC command calls remain in thread context.

## USB development identity

The development build uses:

```text
HCI_USB_VID=0xCAFE
HCI_USB_PID=0x4070
HCI_USB_DEVICE_RELEASE=0x0100
```

They are defined in the Eclipse project, under C/C++ Build, Settings, Compiler,
Preprocessor. Change them there for a production USB identity.

`0xCAFE` is the TinyUSB example vendor ID and is allocated to nobody. It is
fine on a bench and wrong on anything shipped, so an image that leaves this
board and meets a host that keeps a device database needs a real VID and PID
before it goes.

## Tests

```sh
make -C tests run
```

The host tests need only a C++14 compiler. The target headers are replaced by
fakes, so no SDK checkout and no board are required. Pass
`NRFXLIB_DIR=/path/to/sdk-nrfxlib` to add the dispatch test, which compiles the
real command table against the real SoftDevice Controller headers.

`tests/hardware` holds the Python tools that drive an attached board over its
USB CDC port. See `tests/README.md`.

## Design constraints

- one firmware image;
- runtime USB/UART selection;
- IOsonata `DeviceIntrf` injection;
- standard Bluetooth HCI H:4 framing;
- static allocation;
- no heap in the HCI data path;
- no Zephyr dependency;
- IOsonata and TaktOS remain separately built static libraries.

## License

HciController source written by I-SYST inc. is licensed under the Mozilla Public License 2.0. External components retain their own licenses.

Copyright (c) 2026 I-SYST inc.
