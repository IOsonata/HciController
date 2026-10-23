# nRF5340 network core HciController

HciController runs on the nRF5340 network core. The target files are
`src/hci_nrf5340_net.cpp` and `include/hci_nrf5340_net.h`. The host link is a
swappable interface, selected at run time the way the UDG-nRF52840 build
swaps its host transport:

| Mode | Host link | Use |
| --- | --- | --- |
| `HCI_APP_MODE_IPC` (default) | IOsonata `BtHciIpc` packet interface over a shared memory block | application core host |
| `HCI_APP_MODE_UART_H4` | H:4 over a UART | bench host |

Both are plain `DevIntrf` objects to the controller. IPC is a packet
interface like native USB: one ring slot is one HCI packet, `DevAddr` is the
packet type, nothing is framed from a byte stream. UART goes through the H:4
adapter as on the other targets.

## Mode switch

A press of the mode button stops the controller, stores the other mode in
the two retained general purpose registers and resets the network core. The
network core has no NVM interface port, so the mode survives a reset but not
a power loss: after a power cycle IPC is used again. A vendor HCI command to
change the mode is planned.

The button and the UART pins are set in `src/board.h`: the nRF5340 Audio DK
by default (VOL- button, the network core's own virtual COM port P1.09 TXD,
P1.08 RXD, P1.10 RTS, P1.11 CTS, the second port the board enumerates; the
first one is the application core console), the nRF5340 DK with
`NRF5340_DK_PCA10095`.
The application core must hand those pins to the network core (GPIO MCUSEL)
before it starts the network core.

## Ownership

HciController is the IPC owner (the service provider). At start it
partitions and initializes the shared block (`IpcIntrfHdr_t` plus the two
packet rings). The application core attaches as the peer: it is given only
the block address and its channels and reads the ring geometry from the
header. The application core host side is a separate step and is not part of
this project; it uses the same `BtHciIpc` class from IOsonata.

## Build

Open `nRF5340_Net/ioc/.project` in IOcomposer and build `Debug` or
`Release`. The project uses the network core linker script
`nrf5340_xxaa_network.ld`, the `IOsonata_nRF5340_Net` and `TaktOS_M33`
libraries, and the nrfxlib nrf53 soft-float multirole SDC and MPSL archives.
The IOsonata library must contain `ipc_intrf.cpp`, `ipc_ctrlr_nrf53.cpp`,
`bt_hci_ipc.cpp` and `bt_hci_h4.cpp`.

Defined symbols: `NRF5340_XXAA_NETWORK`, `NRFXLIB_SDC`,
`HCI_APP_HAS_USB=0`, `HCI_APP_HAS_UART=1`, `HCI_APP_HAS_IPC=1`. The
`HCI_APP_HAS_*` switches leave out the USB code and statics of
`hci_app.cpp`; the other targets keep their defaults.

## Shared memory

Set in `src/board.h`:

| Macro | Default | Meaning |
| --- | --- | --- |
| `HCI_IPC_MEM_ADDR` | `0x20070000` | shared block, in application core RAM |
| `HCI_IPC_MEM_SIZE` | `0x10000` | room reserved for the block |
| `HCI_IPC_NB_PKT` | `8` | packet slots in each direction |
| `HCI_IPC_TX_CHAN` / `HCI_IPC_RX_CHAN` | `0` / `1` | controller to host, host to controller |
| `HCI_IPC_IRQ_PRIORITY` | `6` | IPC interrupt priority |

A slot holds `BT_HCI_IPC_PKT_MAXLEN` (272) bytes plus the type byte and a
length header, about 275 bytes. Eight slots in each direction use about
4.4 KB of the block. Both cores must be built with the same
`BT_HCI_IPC_PKT_MAXLEN` and slot count (the peer reads the count from the
block header).

Requirements on the application core image:

- keep `HCI_IPC_MEM_ADDR` .. `+HCI_IPC_MEM_SIZE` out of its linker script
  RAM so nothing else uses it;
- open that region to the network core (SPU RAM permission) and the IPC
  channels before releasing the network core from reset. The IOsonata
  nRF5340 IPC port does this when the application core initializes its side
  of the link (only the application core has the SPU), so initialize the
  host side first, then release the network core.

Because each slot is one whole packet, an application core restart cannot
leave half a packet behind. A slot that is not a whole packet is dropped and
counted.

## SDC memory

The default SDC resource set (16 peripheral, 2 central links, full ISO) needs
about 94 KB, which does not fit the 64 KB network core RAM. The project
therefore defines a smaller set: 4 peripheral and 1 central link, 1 CIG with
2 CIS, 1 BIG, 1 periodic advertiser, 1 periodic sync. That is about 37.8 KB
of SDC memory (about 43 KB for the whole `HciApp_t` on a 64-bit host build).
The counts in `include/hci_sdc_resources.h` can be overridden with `-D`; the
defaults for other targets are unchanged.

The final RAM use (stacks, MPSL, library data) has to be checked in the
linker map of the real network core build.

## Status

Verified on the host: the IPC link round trip (command in, event out, ACL
both ways across ring wrap, full ring, malformed slots), `hci_app.cpp` with
UART and IPC together, `hci_nrf5340_net.cpp` and `main.cpp` compile (the
latter two as syntax checks against stubs). Not done: an ARM cross build, and
any run on nRF5340 hardware, so the button, the retained registers and the
SPU setup are untested.
