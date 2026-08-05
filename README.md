# HciController

Bluetooth LE HCI controller firmware for the I-SYST BLYST840/nRF52840 platform, built with IOsonata, TaktOS, TinyUSB, and Nordic Semiconductor's nrfxlib SoftDevice Controller.

The firmware exposes a standard Bluetooth HCI H:4 byte stream through an IOsonata `DeviceIntrf`. One firmware image supports two host connections:

- USB CDC through IOsonata `UsbdCdcIntrf` with a TinyUSB backend;
- UART to an nRF9151 or another host processor.

The HCI controller, H:4 parser, SDC binding, and TaktOS execution path are identical for both interfaces.

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

## Development layout

Dependencies remain separate sibling repositories and external SDKs:

```text
root_dev/
├── external/
│   ├── tinyusb/
│   ├── nrfx/
│   └── sdk-nrfxlib/
├── IOsonata/
├── HciController/
└── TaktOS/
```

HciController does not compile IOsonata or TaktOS sources. Build these static libraries first:

```text
IOsonata_nRF52840 -> Debug_SDC
TaktOS_M4         -> DebugFPU
```

Then build the HciController `Debug` configuration.

## Eclipse Embedded CDT

Import `HciController/IOcomposer` as an existing project. It is a Managed CDT Arm Cross GCC project.

After updating project metadata, remove the old workspace project without deleting its files, delete `IOcomposer/Debug`, and re-import it. Eclipse caches linked resources and generated makefiles.

The application project compiles:

- HciController sources;
- TinyUSB device sources.

It links:

- `libIOsonata_nRF52840.a` from `Debug_SDC`;
- `libTaktOS_M4.a` from `DebugFPU`;
- nrfxlib MPSL;
- nrfxlib MPSL FEM common;
- nrfxlib multirole SoftDevice Controller.

## Command-line build

The repository-root `Makefile` uses the same sibling layout and static libraries:

```sh
make
```

Outputs are written under `build/`:

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

Override these values in `local.mk` or on the make command line for production USB identity.

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
