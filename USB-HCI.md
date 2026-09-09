# Native Bluetooth USB HCI

HciController supports three host-side HCI transports without changing the
controller routing layer:

| Host link | Physical `DeviceIntrf` | Framing seen by `HciController` |
| --- | --- | --- |
| UART | UART byte stream | H:4 adapter exposes packet `DeviceIntrf` |
| USB CDC compatibility mode | CDC byte stream | H:4 adapter exposes packet `DeviceIntrf` |
| Native Bluetooth USB | IOsonata `BtHciUsb` | native packet `DeviceIntrf` |

At the controller boundary, `DevAddr` is the HCI packet type and the data buffer
contains one complete HCI packet without an H:4 indicator. UART and CDC add or
remove the indicator below that boundary. Legacy Bluetooth USB does not put an
H:4 indicator on the wire. Bulk Serialization does put the one-byte H:4 packet
indicator on its bulk transport, and `BtHciUsb` removes or adds it before the
packet reaches HciController.

## Native transport ownership

IOsonata `BtHciUsb` owns the Bluetooth HCI USB transport, endpoint state,
packet assembly, packetization, Bulk Serialization and synchronous USB
isochronous handling. HciController owns its USB identity, runtime mode
selection and the independent diagnostic CDC composition.

`BtHciUsb::Init()` obtains interface and endpoint numbers from IOsonata's USB
function allocator. The application supplies the descriptor storage matching
its requested layout in `BtHciUsbCfg_t`; Init fills that fragment after resource
allocation. HciController then copies the complete fragment into its native
composite configuration and allocates the diagnostic CDC function from the
remaining ordinary interfaces and endpoints.

HciController native mode requests the full layout:

```text
bSco                 true
bBulkSerialization   true
pFullDesc             BtHciUsbFullDesc_t storage
```

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

## Native Bluetooth function

The full native descriptor contains two Bluetooth interfaces followed by the
independent diagnostic CDC function.

### HCI interface 0

Alternate setting 0 is the standard legacy Bluetooth USB HCI transport:

| Direction | HCI packet | USB transport |
| --- | --- | --- |
| Host to controller | Command | EP0 class control OUT |
| Host to controller | ACL | bulk OUT |
| Controller to host | Event | interrupt IN |
| Controller to host | ACL | bulk IN |

Commands are class OUT requests addressed to the device or the allocated HCI
interface. ACL packets may span physical bulk transactions; `BtHciUsb` exposes
one complete logical HCI packet to HciController.

Alternate setting 1 is Bluetooth HCI Bulk Serialization. It reuses the ACL
bulk endpoint pair and carries a one-byte H:4 packet indicator. This is the
native transport HciController uses for LE HCI ISO packets in the CIS/BIS
hardware tests.

The HCI interface therefore provides:

```text
alt 0   Command / Event / ACL using legacy Bluetooth USB rules
alt 1   Command / Event / ACL / SCO / ISO using Bulk Serialization
```

### Synchronous interface 1

Alternate setting 0 has no endpoints. Alternate settings 1 through 6 use the
USB controller's dedicated bidirectional isochronous endpoint for Bluetooth SCO:

| Alternate | Full-speed MPS |
| ---: | ---: |
| 1 | 9 |
| 2 | 17 |
| 3 | 25 |
| 4 | 33 |
| 5 | 49 |
| 6 | 63 |

On nRF52840, ordinary control/bulk/interrupt endpoints are numbered 0 through 7
and the dedicated isochronous endpoint is EP8 in both directions. IOsonata
`UsbIsoIntrf` owns the one-frame-per-service-interval ISO staging and transfer
lifecycle; `BtHciUsb` owns SCO packet assembly and segmentation above it.

Bulk Serialization alt 1 and a non-zero synchronous alternate are mutually
exclusive, matching the transport state owned by `BtHciUsb`.

Bluetooth SCO and LE HCI ISO are distinct paths. SCO uses the synchronous USB
isochronous interface. LE HCI ISO uses HCI packet type `0x05` over Bulk
Serialization. The nRF52840 SoftDevice Controller used by this firmware is an
LE controller, so the release acceptance suite exercises LE CIS/BIS HCI ISO;
the SCO USB transport is present as part of the complete Bluetooth HCI USB
function but is not a BR/EDR radio capability supplied by SDC.

### Diagnostic CDC

The native composite then adds the diagnostic CDC function on interfaces 2/3.
Its endpoints are allocated from the ordinary endpoint pool after the Bluetooth
function has reserved Event, ACL and the dedicated SCO ISO endpoint.

```text
native Bluetooth USB HCI   Bluetooth full HCI function + CDC log
CDC/H:4 compatibility      CDC 0 HCI H:4 + CDC 1 log
UART host / log-only USB   CDC 0 log
```

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

A host sleep also terminates the active USB HCI session on nRF52840. After the
suspend/resume cycle HciController resets the MCU so USB, `BtHciUsb`, SDC and
HCI bridge state restart together. Host discovery resolves the stable
VID/PID/serial identity again and requires a successful HCI `Read BD_ADDR`
before a new release run begins.

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

## Validation

The HciController host gate compiles the current IOsonata `BtHciUsb`,
`UsbIntrf` and `UsbIsoIntrf` sources. Its native USB unit test pins the full
allocator-owned descriptor topology, including serialized HCI alt 1, synchronous
alternates 1 through 6 on EP8, and diagnostic CDC placement after the Bluetooth
function.

IOsonata's own USB tests validate the generic ISO service-interval data path and
Bluetooth SCO packet assembly/segmentation. HciController's hardware release
suite validates the controller behavior it actually exposes through SDC,
including native Bulk Serialization, bidirectional CIS HCI ISO, BIS HCI ISO,
recovery and concurrent ACL/ISO/event stress.

See [tests/README.md](tests/README.md) and
[tests/harness/README.md](tests/harness/README.md) for validation entry points.
