# HciController nRF52840 Eclipse project

This directory contains the Eclipse Embedded CDT managed project used to build
HciController for nRF52840.

It is not a generic generated IOsonata application. The project links the
HciController sources with separately built IOsonata/TaktOS libraries, TinyUSB,
Nordic nrfxlib MPSL and the multirole SoftDevice Controller.

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

The linker script itself is named in `.cproject` by basename. The project adds
both the common IOsonata linker directory and the nRF52840 linker directory to
the linker search path. This avoids embedding one machine-specific absolute
workspace path into the script option.

## Configurations

| Configuration | Use | C++ linker script |
| --- | --- | --- |
| `Debug` | debugger/no bootloader development | `nrf52840_xxaa_sdc.ld` |
| `Release` | optimized no-bootloader build | `nrf52840_xxaa_sdc.ld` |
| `Release_MBR` | USB DFU/MBR layout | `nrf52840_xxaa_sdc_mbr.ld` |
| `Release_SD` | S140-compatible application-origin layout | `nrf52840_xxaa_s140_sdc.ld` |

All HciController configurations use the nrfxlib multirole SDC. The `SD`/`MBR`
names describe the flash/linker memory map, not a second HCI implementation and
not a board selection.

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

This is the UDG-NRF52840x USB-DFU layout. At startup the release log should
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
installed, its occupied flash must be checked separately against the generated
map and the exact IOsonata linker-script revision used for the build.

### No bootloader (`Debug` / `Release`)

The no-bootloader SDC linker reserves:

```text
application      0x000000 .. 0x0FCFFF
NVM0             0x0FD000 .. 0x0FFFFF
```

HciController uses only the first erase page of `NVM0` for the mode record.

## Board selection

`nRF52840/src/board.h` owns the board policy and pin map. `BOARD` may be set by
the build; otherwise UDG-NRF52840 is the default. The Eclipse configuration
name selects a linker layout only. It does not select UDG, IBK, Thingy:91 or a
WildThing board.

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

Build the nRF52840 IOsonata and TaktOS libraries first. The managed project
links, rather than compiles, those repositories.

The SDC build also needs the nRF52 hard-float libraries from:

```text
external/sdk-nrfxlib/softdevice_controller/lib/nrf52/hard-float
external/sdk-nrfxlib/mpsl/lib/nrf52/hard-float
external/sdk-nrfxlib/mpsl/fem/common/lib/nrf52/hard-float
```

TinyUSB sources are compiled by the application project.

## Import/build notes

Import this directory as an existing Eclipse project.

If `.cproject` changes while the project is already imported, Eclipse may keep
old generated makefiles and linked-resource state. Remove the workspace project
without deleting files, remove the old configuration output directory, then
re-import before judging a new project setting.

Build products are written under the active configuration directory and include:

```text
HciController.elf
HciController.hex
HciController.bin
HciController.map
```

Keep the `.map` as release evidence. It is the easiest way to confirm that the
build used the intended linker-owned memory layout.

## Language/runtime settings

The embedded project uses GNU C/C++ with:

```text
-fno-exceptions
-fno-rtti
```

Controller and real-time paths use static/caller-owned storage. Follow
`../../CODING.md` for the IOsonata coding and review rules used by this project.

## Release

Do not tag a build based only on a successful compile. Run the host suite with
the real nrfxlib headers, verify the release map, then run the appropriate
hardware mode/persistence tests and official release harness described in
`../../RELEASE.md`.
