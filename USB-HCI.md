# Native Bluetooth USB HCI

HciController supports three host-side HCI transports without changing the
controller routing layer:

| Host link | Physical `DeviceIntrf` | Packet framing seen by `HciController` |
| --- | --- | --- |
| UART | UART byte stream | H:4 adapter exposes packet `DeviceIntrf` |
| USB CDC compatibility mode | CDC byte stream | H:4 adapter exposes packet `DeviceIntrf` |
| Native Bluetooth USB | Bluetooth USB class | native packet `DeviceIntrf` |

The common interface at the controller boundary is `DevIntrf_t *`.
`HciController` does not have a USB-specific controller API.

For every packet-oriented HCI `DeviceIntrf`:

```text
DevAddr = HciH4PacketType_t
pData   = complete HCI packet bytes, without an H:4 indicator
one successful DeviceIntrfRx/Tx = one complete HCI packet
```

`DevAddr` is packet metadata. UART and CDC add/remove the H:4 indicator below
that boundary; native USB already has packet boundaries.

## UART and CDC/H:4

UART and CDC are byte streams. `HciIntrfTransport` parses and emits H:4 on the
physical `DeviceIntrf`, then exposes another packet-oriented `DevIntrf_t` above
it. The H:4 indicator therefore exists only on the UART/CDC wire.

## Native Bluetooth USB

`HciUsb_t` embeds a `DevIntrf_t`. The TinyUSB Bluetooth class driver queues
complete packets into that interface; it does not call HciController routing
callbacks directly.

Native mode is a composite USB device: a Bluetooth Controller function plus an
independent CDC log function. The device descriptor uses the IAD-aware
composite tuple:

```text
bDeviceClass    0xEF
bDeviceSubClass 0x02
bDeviceProtocol 0x01
```

The Bluetooth function keeps the Bluetooth Controller tuple in its IAD and
interface descriptors:

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
interface. The class driver also accepts device-targeted HCI command requests
for host compatibility, including the historical `bRequest = 0xE0` form.

### Interface 0 alternate 1: Bulk Serialization

Commands, Events, ACL and ISO use the bulk endpoint pair. Each serialized packet
carries the standard one-byte HCI packet indicator on the USB wire.
`HciUsb_t` removes/adds that indicator below `DeviceIntrf`, so the controller
still sees a normal packet interface.

ISO HCI data uses Bulk Serialization. It is not mapped onto the legacy USB
synchronous endpoints.

### Interface 1 alternate 0: zero synchronous bandwidth

The nRF52840 SoftDevice Controller release profile is LE-only and has no SCO
data path. The required synchronous Bluetooth interface is therefore present at
alternate setting 0 with no endpoints. Non-zero SCO bandwidth settings are not
advertised.

The independent CDC function in the native descriptor is the diagnostic log;
it is not an HCI transport.

## Runtime HCI mode selection

`HCI_USB_HCI_TRANSPORT` is the default USB mode used when no persisted mode is
available:

```text
HCI_USB_HCI_TRANSPORT_NATIVE   native Bluetooth USB HCI
HCI_USB_HCI_TRANSPORT_CDC_H4   CDC byte stream carrying H:4
```

`HCI_HOST_SELECT` similarly controls the default host family on boards where
UART is legal. On boards with `HCI_MODE_SWITCH=1`, these are first-boot/default
choices, not permanent build-time transport locks.

The release board policy is:

| Board | Runtime modes | Button sequence |
| --- | --- | --- |
| UDG-NRF52840x | USB H:4, native USB HCI | USB H:4 <-> native |
| IBK-NRF52840 | UART H:4, USB H:4, native USB HCI | UART -> USB H:4 -> native -> UART |
| Thingy:91 | UART H:4 only | none |
| WildThing51 | UART H:4 only | none |
| WildThing91 | UART H:4 only | none |

On a confirmed mode-button press, HciController does not write flash while SDC
is active. It stops the HCI runtime, target USB, SDC and MPSL, writes and
verifies the new mode in `NVM0`, then resets. The next boot loads the persisted
mode before USB descriptors or the radio are started.

The selected mode therefore survives software reset and power cycle. The mode
is not handed through GPREGRET/GPREGRET2 or another bootloader-owned retained
register.

The physical NVM address is linker-owned and must match the installed DFU
layout. See [BUILDING.md](BUILDING.md) and
[nRF52840/ioc/README.md](nRF52840/ioc/README.md) for the build configurations
and memory maps.

## Diagnostic CDC log

The log is a separate USB CDC function:

```text
native Bluetooth USB HCI   Bluetooth function + CDC 0 log
CDC/H:4 compatibility      CDC 0 HCI H:4 + CDC 1 log
UART host / log-only USB   CDC 0 log
```

The log function exists independently of HCI transport selection so startup,
mode and controller diagnostics remain observable without a debugger.

## USB identities

The open-source repository intentionally defaults to development identities:

```text
VID       0xCAFE
CDC/H4    PID 0x4070
native    PID 0x4071
log-only  PID 0x4072
bcdDevice 0x0100 for HciController 1.0.0
```

A product build can override `HCI_USB_VID`, `HCI_USB_PID_CDC_H4`,
`HCI_USB_PID_NATIVE_HCI` and `HCI_USB_PID_LOG_ONLY`.

Define:

```text
HCI_USB_REQUIRE_ASSIGNED_IDS=1
```

for a product build that must fail at compile time if a development USB
identity is still selected. The USB device release value is derived from
`FIRMWARE_VERSION`; there is no independent USB release-version setting to
maintain.

## Official harness transport

The official hardware/release test system is under `tests/harness/`. Reusable
HCI event, command, transport and CIS/ISO support lives in
`python/hcicontroller/`; HciController entry points live in
`tests/harness/hcicontroller/`.

`python/hcicontroller/hci_transport.py` discovers native USB controllers through
PyUSB/libusb. In legacy native mode it routes commands/events/ACL according to
the Bluetooth USB endpoints. Bulk Serialization selects alternate setting 1 and
carries the HCI packet indicator, including HCI ISO packets.

Focused native USB CIS/ISO test:

```sh
python3 tests/harness/hcicontroller/cis_usb_pair_test.py
```

With more than two compatible controllers attached, select the pair by USB
serial number:

```sh
python3 tests/harness/hcicontroller/cis_usb_pair_test.py \
    --central SERIAL_A --peripheral SERIAL_B
```

Full native USB release run:

```sh
python3 tests/harness/hcicontroller/release_test.py \
    --transport usb --a SERIAL_A --b SERIAL_B
```

The release runner uses legacy native USB for ordinary HCI phases and switches
to Bulk Serialization for ISO phases, so alternate-setting transitions are
exercised as part of the same release run.

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

See [tests/README.md](tests/README.md) and
[tests/harness/README.md](tests/harness/README.md) for the source checks and
hardware validation entry points.
