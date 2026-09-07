# Native Bluetooth USB HCI

HciController supports three host-side HCI transports without changing the
controller routing layer:

| Host link | Physical `DeviceIntrf` | Framing seen by `HciController` |
| --- | --- | --- |
| UART | UART byte stream | H:4 adapter exposes packet `DeviceIntrf` |
| USB CDC compatibility mode | CDC byte stream | H:4 adapter exposes packet `DeviceIntrf` |
| Native Bluetooth USB | IOsonata `UsbdHci` | native packet `DeviceIntrf` |

At the controller boundary, `DevAddr` is the HCI packet type and the data buffer
contains one complete HCI packet without an H:4 indicator. UART and CDC add or
remove the indicator below that boundary. Legacy Bluetooth USB does not put an
H:4 indicator on the wire.

## Native transport ownership

IOsonata `UsbdHci` owns the Bluetooth class transport, endpoint state, packet
assembly and buffering. HciController owns only its USB identity, runtime mode
selection and composite configuration assembly.

`UsbdHci` obtains interface and endpoint numbers from IOsonata's allocator and
exports its descriptor fragment with `MakeDesc`. HciController copies that
fragment into the native composite configuration, then allocates the independent
diagnostic CDC function from the remaining interfaces and endpoints. Application
code does not hardcode the HCI topology.

Native mode uses the IAD-aware composite device tuple:

```text
bDeviceClass    0xEF
bDeviceSubClass 0x02
bDeviceProtocol 0x01
```

The Bluetooth IAD and interfaces use the Bluetooth Controller tuple:

```text
bFunctionClass / bInterfaceClass       0xE0
bFunctionSubClass / bInterfaceSubClass 0x01
bFunctionProtocol / bInterfaceProtocol 0x01
```

## Legacy Bluetooth USB function

The implemented transport is:

| Direction | HCI packet | USB transport |
| --- | --- | --- |
| Host to controller | Command | EP0 class control OUT |
| Host to controller | ACL | bulk OUT |
| Controller to host | Event | interrupt IN |
| Controller to host | ACL | bulk IN |

The Bluetooth function has an HCI interface plus a synchronous interface at
alternate setting 0 with zero endpoints. SCO, ISO, non-zero synchronous
alternate settings and Bulk Serialization are not implemented in this
milestone.

Command requests must be class OUT requests addressed to the device or the
allocated HCI interface, with `bRequest = 0`, `wValue = 0` and a non-empty
payload. `UsbdHci` validates the three-byte Command header on completion and
does not accept a second command while one is pending.

ACL packets may span multiple physical bulk transactions. `UsbdHci` assembles
the four-byte ACL header and payload before exposing one logical receive. Event
and ACL transmission similarly accepts one complete logical HCI packet and
packetizes it for the USB controller. Exact-MPS transfers include the required
terminating ZLP.

The independent CDC function in the native descriptor is the diagnostic log;
it is not an HCI transport.

## Runtime HCI mode selection

`HCI_USB_HCI_TRANSPORT` selects the default USB mode when no persisted mode is
available:

```text
HCI_USB_HCI_TRANSPORT_NATIVE   native Bluetooth USB HCI
HCI_USB_HCI_TRANSPORT_CDC_H4   CDC byte stream carrying H:4
```

`HCI_HOST_SELECT` similarly controls the default host family on boards where
UART is legal. With `HCI_MODE_SWITCH=1`, these are first-boot defaults rather
than permanent build-time locks.

| Board | Runtime modes | Button sequence |
| --- | --- | --- |
| UDG-NRF52840 family | USB H:4, native USB HCI | USB H:4 ↔ native |
| IBK-NRF52840 | UART H:4, USB H:4, native USB HCI | UART → USB H:4 → native → UART |
| Thingy:91 | UART H:4 only | none |
| WildThing51 | UART H:4 only | none |
| WildThing91 | UART H:4 only | none |

On a confirmed mode-button press, HciController stops HCI, USB, SDC and MPSL,
writes and verifies the new mode in `NVM0`, then resets. The next boot reads the
mode before USB descriptors or the radio start. The NVM address is linker-owned;
see [BUILDING.md](BUILDING.md) and
[nRF52840/ioc/README.md](nRF52840/ioc/README.md).

## Diagnostic CDC log

```text
native Bluetooth USB HCI   Bluetooth function + CDC log
CDC/H:4 compatibility      CDC 0 HCI H:4 + CDC 1 log
UART host / log-only USB   CDC 0 log
```

The log remains available independently of the HCI transport so startup, mode
and controller diagnostics can be observed without a debugger.

## USB identities

The open-source defaults are development identities:

```text
VID       0xCAFE
CDC/H4    PID 0x4070
native    PID 0x4071
log-only  PID 0x4072
bcdDevice 0x0100 for HciController 1.0.0
```

A product build can override `HCI_USB_VID`, `HCI_USB_PID_CDC_H4`,
`HCI_USB_PID_NATIVE_HCI` and `HCI_USB_PID_LOG_ONLY`. Define
`HCI_USB_REQUIRE_ASSIGNED_IDS=1` to reject development IDs at compile time.

## Host and hardware validation

The host test compiles the real IOsonata `UsbdHci` implementation against a
fake USB controller. It pins allocator-owned descriptor integration, the real
`UsbRegisterFunc` endpoint/XferHandler rule, EP0 Command delivery, fragmented
ACL assembly, Event/ACL packetization, exact-MPS ZLPs and rejection of alternate
setting 1.

The Python native USB transport retains optional Bulk Serialization support for
controllers that advertise it, but this HciController firmware milestone does
not. Use CDC/UART H:4 for hardware ISO testing. Native USB hardware validation
is limited to Command, Event and ACL until ISO or Bulk Serialization is added to
IOsonata `UsbdHci`.

See [tests/README.md](tests/README.md) and
[tests/harness/README.md](tests/harness/README.md) for validation entry points.
