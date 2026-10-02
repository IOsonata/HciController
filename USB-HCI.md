# HciController USB HCI transport

HciController uses IOsonata's USB device stack: the generic device core, the
`BtHciUsb` Bluetooth HCI class and the `UsbdCdc` class, all built into the
IOsonata nRF52840 library. The 1.0.0 release used TinyUSB with a transport
class of its own; that code is gone.

## Transport boundary

HciController always works with complete HCI packets at its controller boundary.
`DevAddr` identifies the HCI packet type and the packet buffer does not include
an H:4 indicator.

| Host transport | Wire format | HciController boundary |
| --- | --- | --- |
| UART | H:4 byte stream | packet `DeviceIntrf` through H:4 adapter |
| USB CDC compatibility | H:4 byte stream | packet `DeviceIntrf` through H:4 adapter |
| Native Bluetooth USB | IOsonata `BtHciUsb` | native packet `DeviceIntrf` |

Bulk Serialization uses a one-byte H:4 packet indicator on its USB bulk wire
format. `BtHciUsb` adds/removes that indicator internally.

## Ownership

IOsonata `main` owns the complete USB device architecture used here:

```text
UsbInit
  -> generic USB device core
      -> UsbDeviceClass registration
      -> interface/endpoint allocation
      -> configuration descriptor assembly
      -> Chapter 9 dispatch

BtHciUsb
  -> HCI interface
  -> ACL bulk transport
  -> Event interrupt transport
  -> optional Bulk Serialization (enabled here)
  -> optional synchronous SCO alternates over UsbIsoIntrf (not enabled here)

UsbdCdc
  -> diagnostic or H:4 CDC ACM function
```

HciController does **not** construct USB configuration descriptors and does not
assign interface or endpoint numbers. Its USB-specific source owns only product
identity policy (VID/PID per runtime mode) and application-level transport
selection.

The application initializes every class before `UsbEnable()`. Each class asks
IOsonata's allocator for interfaces/endpoints and registers its own descriptor
fragment. `UsbEnable()` asks the generic core for the complete configuration
descriptor; the core concatenates registered class fragments in registration
order.

## Native HCI layout

Native mode initializes `BtHciUsb` before the diagnostic CDC and requests:

```text
bSco                 false
bBulkSerialization   true
```

On nRF52840 the resulting full-speed layout is expected to be:

```text
interface 0 alt 0   Bluetooth HCI legacy transport
interface 0 alt 1   Bluetooth HCI Bulk Serialization
interface 1 alt 0   synchronous interface, no endpoints
interface 2 alt 0   diagnostic CDC control
interface 3 alt 0   diagnostic CDC data
```

The allocator normally assigns:

```text
Event IN             EP1
ACL OUT / IN         EP2
CDC notification IN  EP3
CDC data OUT / IN    EP4
```

The 1.0.0 release placed the diagnostic CDC on EP4/EP5 because its static
descriptor reserved EP3 for the synchronous slot. IOsonata allocates from the
lowest free endpoint, so the CDC log now sits on EP3/EP4. Hosts find the log
port by VID/PID and interface class, not by endpoint number.

### HCI interface 0

Alternate setting 0 is the standard Bluetooth USB HCI transport:

| Direction | HCI packet | USB transport |
| --- | --- | --- |
| Host -> controller | Command | class OUT control request |
| Host -> controller | ACL | bulk OUT |
| Controller -> host | Event | interrupt IN |
| Controller -> host | ACL | bulk IN |

Alternate setting 1 is Bluetooth HCI Bulk Serialization. It reuses the ACL bulk
endpoint pair and carries Command, Event, ACL, SCO and ISO packet types with the
H:4 indicator.

### Synchronous interface 1

Alternate setting 0 only, with no endpoints, as the Bluetooth HCI USB function
requires. The nRF52840 SoftDevice Controller is an LE controller with no SCO
data path, so the synchronous alternates `BtHciUsb` can publish (`bSco`) are
not enabled: they would advertise ISO endpoints that never carry anything.
CIS/BIS traffic uses HCI packet type `0x05` through Bulk Serialization.

## Runtime modes and USB identities

HciController keeps three runtime modes:

```text
UART H:4       USB log only
USB H:4        CDC H:4 + diagnostic CDC
USB native     BtHciUsb + diagnostic CDC
```

Development identities are:

| Mode | VID:PID |
| --- | --- |
| CDC H:4 | `CAFE:4070` |
| Native HCI | `CAFE:4071` |
| Log only | `CAFE:4072` |

Product builds can override `HCI_USB_VID`, `HCI_USB_PID_CDC_H4`,
`HCI_USB_PID_NATIVE_HCI`, and `HCI_USB_PID_LOG_ONLY`. Define
`HCI_USB_REQUIRE_ASSIGNED_IDS=1` to reject development IDs at compile time.

The generic IOsonata device descriptor uses the IAD-aware composite tuple:

```text
bDeviceClass    0xEF
bDeviceSubClass 0x02
bDeviceProtocol 0x01
```

## Suspend/wake policy

USB bus suspend does not end the HCI session. The device keeps its
configuration, `BtHciUsb` keeps its endpoint state and the SoftDevice
Controller keeps its links. A controller packet that cannot be sent while the
bus sleeps stays pending in the HCI bridge and goes out with the first host
poll after resume. Remote wakeup is not advertised, so an event raised during
suspend waits for the host to resume on its own.

The Python host transport resolves the native USB device by VID/PID/serial and
retries `Read BD_ADDR` for a bounded interval after a host wake, so a resume
that overlaps a test run is treated as transport settling.

## Validation

The HciController host gate compiles the IOsonata `main` USB components used by
the integration:

```text
src/usb/usb.cpp
src/usb/usbd_epalloc.cpp
src/usb/usb_intrf.cpp
src/usb/usb_iso.cpp
src/usb/usbd_cdc.cpp
src/usb/usbd_cdc_desc.cpp
src/bluetooth/bt_hci_usb.cpp
```

`hci_usb_test` initializes the real generic USB core, `BtHciUsb` and the
diagnostic `UsbdCdc`, then verifies the assembled device/configuration
descriptors, interface allocation and endpoint masks. IOsonata's own USB and
Bluetooth host tests cover the class internals, ISO service behavior, SCO packet
assembly/segmentation and controller-port rules.

Hardware acceptance remains:

```bash
./.venv/bin/python3 tests/harness/hcicontroller/release_test.py
```

The release suite validates command routing, connections, periodic procedures,
PAST/PAwR, native Bulk Serialization, CIS/BIS HCI ISO, recovery and concurrent
ACL/ISO/event stress.
