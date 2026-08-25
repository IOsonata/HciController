# HciController nRF52840 IOcomposer Project

This directory contains the IOcomposer project used to build HciController for
nRF52840.

The project links HciController with IOsonata, TaktOS, TinyUSB, Nordic nrfxlib
MPSL, and the multirole SoftDevice Controller.

## Development environment

Install IOcomposer before opening this project. The IOcomposer installer sets up
the required compiler toolchains, SDK/external repositories, IOsonata, TaktOS,
HciController, and the expected workspace layout. No manual SDK setup is
required for the normal build path.

See [../../BUILDING.md](../../BUILDING.md) for installation and build steps.

## Workspace layout

The project expects the normal IOcomposer sibling layout:

```text
~/IOcomposer/
    external/
        nrfx/
        sdk-nrfxlib/
        tinyusb/
    IOsonata/
    HciController/
    TaktOS/
```

The linker script is named in the project configuration by basename. The project
adds the common IOsonata linker directory and the nRF52840 linker directory to
the linker search path instead of embedding one machine-specific absolute path.

## Build configurations

| Configuration | Use | C++ linker script |
| --- | --- | --- |
| `Debug` | debugger/no-bootloader development | `nrf52840_xxaa_sdc.ld` |
| `Release` | optimized no-bootloader build | `nrf52840_xxaa_sdc.ld` |
| `Release_MBR` | USB DFU/MBR layout | `nrf52840_xxaa_sdc_mbr.ld` |
| `Release_SD` | S140-compatible application origin | `nrf52840_xxaa_s140_sdc.ld` |

All configurations use the nrfxlib multirole SDC. The `SD` and `MBR` names
describe the flash/linker memory map, not a second HCI implementation and not a
board selection.

For the standard UDG-NRF52840 with its USB DFU bootloader, use `Release_MBR`.

## Persistent HCI mode storage

UDG and IBK persist the selected HCI transport in IOsonata `NVM0`. The linker
owns that address. A different bootloader therefore requires the matching
linker layout even though the HciController source is unchanged.

The current IOsonata linker layouts used by this project are:

### USB DFU/MBR (`Release_MBR`)

```text
application      0x001000 .. 0x0DCFFF
NVM0             0x0DD000 .. 0x0DFFFF
USB DFU loader   0x0E0000 ..
```

This is the UDG-NRF52840 family USB-DFU layout. At startup the release log should
include:

```text
mode: nvm addr=0x000DD000 size=12288
```

### S140-compatible application origin (`Release_SD`)

The current `nrf52840_xxaa_s140_sdc.ld` starts application flash at `0x27000`
and reserves the same 12 KiB `NVM0` region used by the MBR layout:

```text
application      0x027000 .. 0x0DCFFF
NVM0             0x0DD000 .. 0x0DFFFF
BT_PDS           0x0FD000 .. 0x0FEFFF
settings         0x0FF000 .. 0x0FFFFF
```

The linker script does not define an OTA bootloader region between those
reservations. Do not infer an `NVM0` address such as `0xF5000` from the
`Release_SD` configuration name or from a particular board. If a bootloader is
installed, check its occupied flash against the generated map and the exact
IOsonata linker-script revision used for the build.

### No bootloader (`Debug` / `Release`)

The no-bootloader SDC linker reserves:

```text
application      0x000000 .. 0x0FCFFF
NVM0             0x0FD000 .. 0x0FFFFF
```

HciController uses only the first erase page of `NVM0` for the mode record.

## Board selection

`nRF52840/src/board.h` owns the board policy and pin map. `BOARD` may be set by
the build; otherwise UDG-NRF52840 is the default. The IOcomposer build
configuration selects a linker layout only. It does not select UDG, IBK,
Thingy:91, or a WildThing board.

Current policies are:

```text
UDG_NRF52840        USB H:4 <-> native USB HCI, persistent button selection
IBK_NRF52840        UART H:4 -> USB H:4 -> native -> UART, persistent
THINGY91_NRF52840   UART H:4 only
WILDTHING51         UART H:4 only
WILDTHING91         UART H:4 only
```

Do not add a board-specific transport decision elsewhere in the HCI stack.

## Build dependencies

IOcomposer installs the required build dependencies and places them in the
expected workspace. There is no separate manual nrfx, nrfxlib, TinyUSB,
IOsonata, or TaktOS setup step for a normal HciController build.

The project links the separately built IOsonata and TaktOS libraries and uses
the nRF52 hard-float SDC/MPSL libraries under the IOcomposer `external` tree.
TinyUSB sources are compiled by the application project.

## Open and build

Open this project in IOcomposer, select the board and required build
configuration, then build.

Build products are written under the active configuration directory and
include:

```text
HciController.elf
HciController.hex
HciController.bin
HciController.map
```

Keep the `.map` for release builds. It confirms the memory layout actually used
by the linker.

If project configuration files change while the project is already open in
IOcomposer, remove the old generated configuration output before rebuilding so
stale generated build files are not used.

## Language/runtime settings

The embedded project uses GNU C/C++ with:

```text
-fno-exceptions
-fno-rtti
```

Controller and real-time paths use static/caller-owned storage.

## Flashing UDG-NRF52840

For a standard UDG-NRF52840 USB DFU build:

1. Build `Release_MBR`.
2. Connect the dongle over USB.
3. Press **SW2 (RESET)** to enter DFU mode.
4. Use Nordic nRF Connect Programmer to load `HciController.hex`.
5. Program the dongle and allow it to reset.

See [../../GETTING_STARTED.md](../../GETTING_STARTED.md) for first-use and HCI
mode instructions.

## Validation

Do not treat a successful compile as complete validation. Run the host/source
suite, verify the generated map for the intended memory layout, and run the
hardware harness appropriate to the board and transport.

For the test entry points and two-controller hardware harness, see
[../../tests/README.md](../../tests/README.md) and
[../../tests/harness/README.md](../../tests/harness/README.md).
