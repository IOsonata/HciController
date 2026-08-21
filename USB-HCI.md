# Native Bluetooth USB HCI

HciController supports three host-side physical transports without changing the
controller routing layer:

| Host link | Physical `DeviceIntrf` | Packet framing seen by `HciController` |
| --- | --- | --- |
| UART | UART byte stream | H:4 adapter exposes packet `DeviceIntrf` |
| USB CDC compatibility mode | CDC byte stream | H:4 adapter exposes packet `DeviceIntrf` |
| Native Bluetooth USB | Bluetooth USB class | native packet `DeviceIntrf` |

The common interface at the controller boundary is `DevIntrf_t *`.
`HciController` does not have a USB-specific transport API.

For every packet-oriented HCI `DeviceIntrf`:

```text
DevAddr = HciH4PacketType_t
pData   = complete HCI packet bytes, without an H:4 indicator
one successful DeviceIntrfRx/Tx = one complete HCI packet
```

`DevAddr` is the packet selector/metadata. This is the same use of
`DeviceIntrf` as other IOsonata packet/protocol interfaces: the meaning of the
device selection id belongs to the implementation.

## UART and CDC/H:4

UART and CDC are byte streams. `HciIntrfTransport` parses and emits H:4 on the
physical `DeviceIntrf`, then exposes another packet-oriented `DevIntrf_t` above
it. The H:4 indicator therefore exists only on the physical UART/CDC wire.

The controller sees the same packet interface whether the physical transport is
UART, CDC, or native USB.

## Native Bluetooth USB

`HciUsb_t` embeds a `DevIntrf_t`. The custom TinyUSB Bluetooth class driver
queues complete packets into that interface; it does not call HciController
callbacks directly.

Native mode is a composite USB device: a Bluetooth Controller function plus an
independent CDC log function. The device descriptor uses the IAD-aware
composite tuple:

```text
bDeviceClass    0xEF
bDeviceSubClass 0x02
bDeviceProtocol 0x01
```

The Bluetooth function itself keeps the Bluetooth Controller tuple in its IAD
and interface descriptors:

```text
bFunctionClass / bInterfaceClass       0xE0
bFunctionSubClass / bInterfaceSubClass 0x01
bFunctionProtocol / bInterfaceProtocol 0x01
```

The Bluetooth function has two interfaces.

### Interface 0 alternate 0: legacy Bluetooth USB HCI

```text
Host -> Controller Command   EP0 class control transfer
Controller -> Host Event     interrupt IN
Host -> Controller ACL       bulk OUT
Controller -> Host ACL       bulk IN
```

There is no H:4 packet indicator on these transfers.

In a composite device, hosts normally address HCI commands to the Bluetooth HCI
interface. The class driver also accepts HCI command requests addressed to the
device, as required for host compatibility, including the historical
`bRequest = 0xE0` form.

### Interface 0 alternate 1: Bulk Serialization

Commands, Events, ACL and ISO use the bulk endpoint pair. Each bulk-serialized
packet carries the standard one-byte HCI packet indicator on the USB wire.
`HciUsb_t` removes/adds that indicator below `DeviceIntrf`, so the controller
still sees a normal packet interface.

ISO HCI data uses Bulk Serialization. It is not mapped onto the legacy USB
synchronous endpoints.

### Interface 1 alternate 0: zero synchronous bandwidth

The current nRF52840 SoftDevice Controller image is LE-only and has no SCO data
path. The required synchronous Bluetooth interface is therefore present only at
alternate setting 0 with no endpoints. Non-zero SCO bandwidth settings are not
advertised.

The independent CDC function in the native descriptor is the diagnostic log;
it is not an HCI transport.

## Build selection

`HCI_USB_HCI_TRANSPORT` selects how a USB host carries HCI:

```text
HCI_USB_HCI_TRANSPORT_NATIVE   native Bluetooth USB HCI (default)
HCI_USB_HCI_TRANSPORT_CDC_H4   CDC byte stream carrying H:4 compatibility mode
```

`HCI_HOST_SELECT` still selects the physical host side (`AUTO`, `USB`, or
`UART`). Selecting USB then uses the `HCI_USB_HCI_TRANSPORT` choice above.

## USB identities

The open-source repository intentionally defaults to development identities:

```text
VID       0xCAFE
CDC/H4    PID 0x4070
native    PID 0x4071
log-only  PID 0x4072
```

These defaults make an unmodified checkout usable for development and testing.
A product build can override them through `HCI_USB_VID` and the three
`HCI_USB_PID_*` macros. Define:

```text
HCI_USB_REQUIRE_ASSIGNED_IDS=1
```

for a product build that must fail at compile time if any development USB
identity is still selected. The repository keeps the development defaults and
does not invent a product VID or PID.

## Official harness transport

The official BLE test system is under `tests/harness/`. Reusable HCI event,
command, transport and CIS/ISO support lives in `tests/harness/lib/`; focused
HciController entry points live in `tests/harness/hcicontroller/`.

`tests/harness/lib/hci_transport.py` discovers native USB controllers through
PyUSB/libusb. In legacy native mode it routes commands/events/ACL according to
the Bluetooth USB endpoints. Bulk Serialization selects alternate setting 1 and
carries the standard HCI packet indicator, including HCI ISO packets.

The focused two-dongle native USB CIS test is:

```sh
python3 tests/harness/hcicontroller/cis_usb_pair_test.py
```

With more than two controllers attached, select the pair by USB serial number:

```sh
python3 tests/harness/hcicontroller/cis_usb_pair_test.py \
    --central SERIAL_A --peripheral SERIAL_B
```

The harness CIS test exercises ACL connection establishment, CIG/CIS creation,
HCI ISO input/output path setup and bidirectional ISO SDUs over the air. The
full HciController release runner remains
`tests/harness/hcicontroller/release_test.py`.

## State-machine rules covered by host tests

The native USB host tests exercise the real configuration descriptor and pin:

- the composite device descriptor is `EF/02/01`, while the Bluetooth IAD and
  interfaces remain `E0/01/01`;
- a pending EP0 HCI command is not modified by `GET_INTERFACE`;
- device-targeted EP0 HCI commands are accepted, including historical
  `bRequest = 0xE0`, while an interface-targeted request must select the
  Bluetooth HCI interface;
- failed HCI alternate-setting changes restore the old endpoints and mode;
- failed synchronous alternate-setting changes restore the old endpoints;
- a bulk payload whose size is an exact endpoint-MPS multiple gets a terminating
  ZLP;
- failure to submit or complete that ZLP retries only the ZLP, never the HCI
  payload;
- a successful short IN completion is not retransmitted as a duplicate packet;
- a complete pending controller packet is reframed when the HCI alternate
  setting changes;
- truncated bulk packets are rejected and the OUT endpoint is rearmed;
- Bulk Serialization removes/adds the packet indicator below `DeviceIntrf`.
