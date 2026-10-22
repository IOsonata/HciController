# nRF5340 network core HciController

HciController runs on the nRF5340 network core and talks to the host over
IPC. The target files are `src/hci_nrf5340_net.cpp` and
`include/hci_nrf5340_net.h`. The host transport is the IOsonata `IpcIntrf`
memory block, selected the same way UART and CDC are on other targets
(`HCI_APP_MODE_IPC_H4`, H:4 packets over two byte rings).

## Ownership

HciController is the IPC owner (the service provider). At start it
partitions and initializes the shared block (`IpcIntrfHdr_t` plus the two
rings). The application core attaches as the peer: it is given only the
block address and its channels and reads the ring geometry from the header.
The application core host side is a separate step and is not part of this
project.

## Build

Open `nRF5340_Net/ioc/.project` in IOcomposer and build `Debug` or
`Release`. The project uses the network core linker script
`nrf5340_xxaa_network.ld`, the `IOsonata_nRF5340_Net` and `TaktOS_M33`
libraries, and the nrfxlib nrf53 soft-float multirole SDC and MPSL archives.

Defined symbols: `NRF5340_XXAA_NETWORK`, `NRFXLIB_SDC`,
`HCI_APP_HAS_USB=0`, `HCI_APP_HAS_UART=0`, `HCI_APP_HAS_IPC=1`. The three
`HCI_APP_HAS_*` switches remove the USB and UART code and statics from
`hci_app.cpp`; the other targets keep their defaults.

## Shared memory

Set in `src/board.h`:

| Macro | Default | Meaning |
| --- | --- | --- |
| `HCI_IPC_MEM_ADDR` | `0x20070000` | shared block, in application core RAM |
| `HCI_IPC_MEM_SIZE` | `0x10000` | room reserved for the block |
| `HCI_IPC_RING_BYTES` | `2048` | size of each of the two byte rings |
| `HCI_IPC_TX_CHAN` / `HCI_IPC_RX_CHAN` | `0` / `1` | controller to host, host to controller |
| `HCI_IPC_IRQ_PRIORITY` | `6` | IPC interrupt priority |

Requirements on the application core image:

- keep `HCI_IPC_MEM_ADDR` .. `+HCI_IPC_MEM_SIZE` out of its linker script
  RAM so nothing else uses it;
- open that region to the network core (SPU RAM permission) and the IPC
  channels before releasing the network core from reset;
- if the application core restarts it can leave a partly written H:4 packet
  in a ring; the controller recovers with the same idle resynchronization it
  uses on UART.

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

Verified on the host: the IPC transport round trip (command in, event out,
ACL both ways across ring wrap, full ring), `hci_app.cpp` in IPC mode and
`hci_nrf5340_net.cpp` compile. Not done: an ARM cross build, and any run on
nRF5340 hardware.
