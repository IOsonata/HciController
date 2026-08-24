# HciController Python library

This directory contains the host-side Python library used to control
HciController directly through Bluetooth HCI.

It is intended for BLE product validation where the test needs precise control
of controller procedures instead of a fixed scanner or test application. The
same transport and feature helpers are used by the HciController hardware
release harness.

Use this library when the test should drive HCI commands, events, ACL or ISO
traffic directly. Use [Bumble](https://github.com/google/bumble) when the test
needs a complete Bluetooth host stack with higher-level GAP, GATT, L2CAP and
other host protocols.

## Install from the repository

From the HciController repository root:

```sh
python3 -m pip install -e ./python
```

The Python package installs `pyserial` for H:4 transports and `pyusb` for native
Bluetooth USB HCI. Native USB also requires a working libusb backend on the
host operating system.

## Minimal controller access

```python
from hcicontroller import Hci, discover

spec = discover(kind="usb")
if spec is None:
    raise RuntimeError("no HciController found")

hci = Hci(spec)
try:
    hci.setup()
    identity, address_type, source = hci.identity()
    print(identity.hex(), address_type, source)
finally:
    hci.close()
```

`discover()` accepts `auto`, `serial`, or `usb`. Auto prefers native Bluetooth
USB HCI when a compatible controller is present and otherwise falls back to
serial H:4.

For multiple native USB controllers, select one by its USB serial number:

```python
spec = discover(kind="usb", usb_selector="SERIAL_NUMBER")
```

For an explicitly selected serial H:4 port, construct a transport spec:

```python
from hcicontroller import Hci, TransportSpec

spec = TransportSpec("serial", "/dev/cu.usbmodem14201")
hci = Hci(spec)
```

## BLE product validation

A typical automated fixture is:

```text
Python validation program
        |
        | direct HCI
        v
UDG-NRF52840 + HciController
        |
        | BLE over the air
        v
product under test
```

The validation program can use one or more HciController dongles to create the
radio procedure required by the product: advertising/scanning, connections,
periodic advertising, PAST, PAwR, ACL traffic, CIS/BIS and HCI ISO traffic.

`hcicontroller.DutControl` defines the optional product-side control interface.
A product adapter can implement it over UART, USB, RPC, GPIO or a dedicated test
firmware command channel while HciController supplies the BLE-side observations
and traffic.

## Examples

`examples/controller_info.py` opens one controller, resets it through HCI and
prints the controller identity. It supports native USB discovery, USB serial
selection and an explicit serial H:4 port.

```sh
python3 python/examples/controller_info.py --transport usb
python3 python/examples/controller_info.py --usb SERIAL_NUMBER
python3 python/examples/controller_info.py --port /dev/cu.usbmodem14201
```

## Library organization

The package contains both low-level and procedure-level helpers:

- `hci_transport.py`: serial H:4 and native Bluetooth USB HCI transports;
- `hci_events.py`: packet/event vocabulary and connection-event parsing;
- `hci_commands.py`: HciController command profile used by hardware tooling;
- `hci_pair.py`: reusable two-controller coordination;
- `hci_iso.py`: HCI ISO packet/data-path helpers;
- `periodic_features.py`: periodic advertising, PAST and PAwR procedures;
- `connected_features*.py`: connected-role feature procedures;
- `bis_features.py`: BIG/BIS procedures;
- `profile.py`: generic controller capability reads;
- `dut.py`: optional product-under-test control interface;
- `results.py`: reusable result accounting.

The programs under `tests/harness/` remain HciController's own release and
hardware acceptance tests. They consume this Python implementation rather than
maintaining a separate HCI library.
