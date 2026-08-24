# Getting Started with HciController

HciController turns an nRF52840 board into a programmable Bluetooth LE HCI
controller. A host application controls the Bluetooth radio through standard HCI
commands instead of being limited to a fixed scanner, sniffer, or test utility.

This is useful when validating a BLE product whose firmware uses custom GATT
services, unusual connection parameters, extended or periodic advertising,
PAwR, ISO, or other procedures that a general-purpose BLE application may not
exercise the way the product requires.

For the 1.0.0 release, the primary user hardware is the I-SYST UDG-NRF52840
family:

- UDG-NRF52840: USB Type-A;
- UDG-NRF52840C: USB Type-C.

Both use the BLYST840 nRF52840 module and support the same HciController USB
modes.

## 1. Download the firmware

Open the HciController 1.0.0 release page:

https://github.com/IOsonata/HciController/releases/tag/1.0.0

Download the published `.hex` firmware image for the UDG-NRF52840.

You do not need to build HciController to use the released firmware. To modify
or rebuild it, see [BUILDING.md](BUILDING.md).

## 2. Enter DFU mode

Connect the UDG-NRF52840 to the computer, then press **SW2 (RESET)** to enter
DFU mode.

The USB DFU bootloader remains separate from HciController. If an application
image is damaged or the selected HCI mode is not useful to the host, SW2 still
provides the firmware update path.

## 3. Flash HciController

Use Nordic Semiconductor's **nRF Connect Programmer**:

1. Open Programmer.
2. Select the UDG-NRF52840 DFU device.
3. Add the downloaded HciController `.hex` file.
4. Program the device.
5. Allow the dongle to reset and enumerate again.

The 1.0.0 UDG release uses the USB DFU/MBR flash layout. Do not flash a build
made for a different memory layout unless the installed bootloader and linker
layout are known to match.

## 4. HCI modes on UDG-NRF52840

UDG-NRF52840 supports two host transports:

| Mode | Host connection | Typical use |
| --- | --- | --- |
| Native USB HCI | Standard Bluetooth USB controller interface | Included Python library, Bumble, and other hosts that use Bluetooth HCI over USB |
| USB H:4 | H:4 byte stream over USB CDC | Included Python library and hosts/scripts that expect a serial HCI transport |

The user button switches between the two modes:

```text
USB H:4 <-> native USB HCI
```

A mode change is stored in internal flash and survives reset and power cycle.
The firmware stops the HCI runtime and radio stack before updating the mode
record, then resets so the correct USB descriptors are active from enumeration.

If the dongle appears with a different USB interface after pressing the mode
button, that is expected.

## 5. Native USB HCI

Native mode exposes a Bluetooth Controller USB function and a separate CDC log
port. The CDC log is diagnostic output; it is not the HCI transport in native
mode.

The development USB identities in the open-source build are:

```text
VID       0xCAFE
CDC/H4    PID 0x4070
native    PID 0x4071
log-only  PID 0x4072
```

A host that understands standard Bluetooth USB HCI can communicate directly
with the native Bluetooth interface.

## 6. Using the included Python library

The repository includes the same Python HCI library used by the HciController
hardware/release harness. Use it when a validation program needs direct control
of HCI commands, events, ACL or ISO traffic and controller procedures.

Install it from a HciController source checkout:

```sh
python3 -m pip install -e ./python
```

The package supports both native Bluetooth USB HCI and serial H:4. Native USB
uses PyUSB and requires a working libusb backend on the host.

A minimal controller program is:

```python
from hcicontroller import Hci, addr_str, discover

spec = discover(kind="usb")
if spec is None:
    raise RuntimeError("no HciController found")

hci = Hci(spec)
try:
    hci.setup()
    identity, address_type, source = hci.identity()
    print("identity:", addr_str(identity))
    print("source:", source)
finally:
    hci.close()
```

For several native USB controllers, select one by its USB serial number:

```python
spec = discover(kind="usb", usb_selector="SERIAL_NUMBER")
```

For an explicit H:4 port:

```python
from hcicontroller import Hci, TransportSpec

spec = TransportSpec("serial", "/dev/cu.usbmodem14201")
hci = Hci(spec)
```

The library also contains reusable pair, periodic advertising, PAST, PAwR,
CIS/BIS, HCI ISO, capability and DUT-control helpers. See
[`python/README.md`](python/README.md) and
[`python/examples/controller_info.py`](python/examples/controller_info.py).

## 7. Using HciController with Bumble

[Bumble](https://github.com/google/bumble) is a Python Bluetooth stack that can
use physical controllers through HCI. Use Bumble when a validation program
needs a complete Bluetooth host stack and higher-level GAP, GATT, L2CAP or other
host protocols rather than direct HCI procedure control.

With UDG-NRF52840 in native USB HCI mode, first list the USB controllers Bumble
can see:

```sh
bumble-usb-probe
```

Bumble accepts transport names such as:

```text
usb:0
usb:<vendor>:<product>
usb:<vendor>:<product>/<serial-number>
```

For a single HciController dongle, `usb:0` is normally the simplest starting
point. If several Bluetooth controllers are connected, use the VID/PID and
serial form reported by `bumble-usb-probe` so the test always opens the intended
controller.

### Minimal Bumble program

The following program opens the first native USB Bluetooth controller and powers
on a Bumble device using HciController:

```python
import asyncio

from bumble.device import Device, DeviceConfiguration
from bumble.transport import open_transport


async def main():
    async with await open_transport("usb:0") as hci_transport:
        device = Device.from_config_with_hci(
            DeviceConfiguration("HciController Demo"),
            hci_transport.source,
            hci_transport.sink,
        )

        await device.power_on()
        print("HciController is online")


asyncio.run(main())
```

Once the controller is open, Bumble can provide the host-side protocol layers
needed by the product test.

## 8. BLE firmware validation

A common HciController setup is:

```text
Product validation program
       |
       | HciController Python library
       | or Bumble
       v
UDG-NRF52840 running HciController
       |
       | Bluetooth LE over the air
       v
BLE product under test
```

Use the included HciController library when the test needs exact direct-HCI
control. Use Bumble when the test needs a complete host stack. Both approaches
let the validation program reproduce behavior specific to the product firmware.

Examples include:

- connect with specific PHY and data-length settings;
- exercise a custom GATT service repeatedly;
- validate reconnect and recovery behavior;
- generate extended or periodic advertising procedures;
- test PAwR request/response behavior;
- create connected or broadcast ISO traffic;
- run long-duration traffic and regression tests;
- capture pass/fail results as part of an automated validation system.

This is the main difference from a fixed BLE utility. HciController supplies the
controller interface; the host validation program defines the BLE procedure
being exercised.

## 9. USB H:4 mode

USB H:4 mode presents HCI as a serial byte stream over a CDC interface. Use this
mode when the host software expects H:4 rather than native Bluetooth USB HCI.

In USB H:4 mode there are separate CDC functions for HCI and diagnostic log
output. Make sure the host opens the HCI CDC port, not the log port.

The exact device name depends on the operating system. On macOS it normally
appears under `/dev/cu.usbmodem*`; Linux normally exposes `/dev/ttyACM*`.

The included Python library can also open an explicit serial H:4 device with a
`TransportSpec` as shown above.

## 10. Diagnostic log

HciController provides a USB CDC diagnostic log independent of the native
Bluetooth HCI data path. Startup output identifies the board, selected mode,
and persistent NVM location.

For the UDG USB-DFU build, the mode-storage line should report:

```text
mode: nvm addr=0x000DD000 size=12288
```

The log is useful for checking startup and mode selection without inserting
logging into the HCI data stream.

## 11. Troubleshooting

### The dongle does not appear as a Bluetooth USB controller

It may be in USB H:4 mode. Press the user mode button and allow the device to
reset and enumerate again.

### A serial port appears instead of a native Bluetooth controller

That is expected in USB H:4 mode. Switch the HCI mode if the host application
expects native USB HCI.

### The Python library reports that PyUSB cannot find a backend

Install libusb for the host operating system. PyUSB is the Python binding; a
native USB backend is still required to access the controller.

### Bumble opens the wrong Bluetooth controller

Run:

```sh
bumble-usb-probe
```

Then use a transport name containing the HciController VID/PID and serial number
instead of `usb:0`.

### Firmware no longer starts normally

Press **SW2 (RESET)** to enter DFU mode and reflash the released `.hex` with
nRF Connect Programmer.

### The selected HCI mode returns after reset

That is intentional. Mode selection is persistent. Press the mode button to
select the other UDG transport.

## Next steps

- [python/README.md](python/README.md): direct Python HCI and validation library.
- [BUILDING.md](BUILDING.md): install IOcomposer and build HciController from source.
- [USB-HCI.md](USB-HCI.md): native Bluetooth USB HCI implementation details.
- [README-commands.md](README-commands.md): HCI command and capability coverage.
- [CHANGELOG.md](CHANGELOG.md): release changes and known limitations.
