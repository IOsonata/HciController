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
| `Release_SD` | OTA DFU/S140-compatible layout | `nrf52840_xxaa_s140_sdc.ld` |

All HciController configurations use the nrfxlib multirole SDC. The `SD`/`MBR`
names describe the flash/bootloader memory map, not a second HCI implementation.

## Persistent HCI mode storage

UDG and IBK persist the selected HCI transport in IOsonata `NVM0`. The linker
owns that address. A different bootloader therefore requires the matching
linker layout even though the HciController source is unchanged.

Required release layouts are:

### USB DFU/MBR (`Release_MBR`)

```text
application      0x001000 .. 0x0DCFFF
NVM0             0x0DD000 .. 0x0DFFFF
USB DFU loader   0x0E0000 ..
```

This is the UDG-NRF52840x layout. At startup the release log should include:

```text
mode: nvm addr=0x000DD000 size=12288
```

### OTA DFU/S140-compatible (`Release_SD`)

```text
application      0x027000 .. 0x0F4FFF
NVM0             0x0F5000 .. 0x0F7FFF
OTA bootloader   0x0F8000 .. 0x0FDFFF
MBR parameters   0x0FE000 .. 0x0FEFFF
settings         0x0FF000 .. 0x0FFFFF
```

This is the layout required when the installed Nordic OTA DFU bootloader starts
at `0xF8000` and reserves the standard 12 KiB application-data area immediately
below it.

Before publishing a `Release_SD` binary, inspect both the IOsonata linker script
and the generated map. The application must stop at `0xF5000` and `NVM0` must
start at `0xF5000`; do not infer this from the configuration name alone.

### No bootloader (`Debug` / `Release`)

The no-bootloader SDC linker currently reserves `NVM0` at
`0xFD000..0xFFFFF`. These configurations are for debugger/development use and
must not be substituted for a DFU-specific release image.

HciController uses only the first erase page of `NVM0` for the mode record.

## Board selection

`nRF52840/src/board.h` owns the board policy and pin map. `BOARD` may be set by
the build; otherwise UDG-NRF52840 is the default.

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
build used the intended bootloader/NVM layout.

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
