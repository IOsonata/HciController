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

## Runtime interface selection

The interface is selected once at reset:

```text
USB VBUS present  -> USB CDC
USB VBUS absent   -> UART
```

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

## Repository layout

```text
include/            HCI headers
src/                HCI sources
nRF52840/ioc/       Eclipse Embedded CDT project
nRF52840/src/       board.h, the pin and clock configuration
tests/              host tests and hardware tools
```

## Boards

`nRF52840/src/board.h` selects the board with `BOARD`. It carries the I-SYST
UDG-NRF52840x dongle, the IBK-NRF52840 breakout, and the Nordic Thingy:91,
whose nRF52840 is the Bluetooth side of a pair with an nRF9160 host over UART.

A board says three things beyond its pins. `HCI_STATUS_LEDS 0` where no status
LED is reachable from this part, so nothing drives pins that belong to
something else. `HCI_APP_FORCE_HOST` where the host transport is not the one
VBUS would imply, which is any board whose USB socket is there for something
other than this part. And `UART_FLOWCTRL`, since RTS and CTS matter only where
the peer drives them.

Replacing the stock firmware on a Thingy:91 takes its USB serial bridge with
it, because that is what the stock firmware was doing.

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
