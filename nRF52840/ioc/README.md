# HciController nRF52840 IOcomposer Project

This directory contains the IOcomposer project used to build HciController for
nRF52840.

The project links HciController with IOsonata, TaktOS, Nordic nrfxlib MPSL,
and the multirole SoftDevice Controller. IOsonata provides the USB stack and
nRF52840 USB controller driver.

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
    IOsonata/
    HciController/
    TaktOS/
```

The managed-project linker options reference the IOsonata family scripts by
relative paths from each configuration's build directory. Common sections come
from `ARM/ldscript/gcc_arm_flash.ld`. Both repositories must be checked out
as siblings for these paths to resolve.

## Build configurations

| Configuration | Use | C/C++ linker script |
| --- | --- | --- |
| `Debug` | no MBR/DFU, SDC controller | `gcc_nrf52840_xxaa.ld` |
| `Release` | optimized no MBR/DFU, SDC controller | `gcc_nrf52840_xxaa.ld` |
| `Release_MBR` | MBR + Nordic USB DFU layout, SDC controller | `gcc_nrf52840_xxaa_mbr.ld` |

All configurations use the nrfxlib SoftDevice Controller (SDC), which
requires no separate linker variant. The IOsonata scripts reside in
`ARM/Nordic/nRF52/ldscript`; common sections come from
`ARM/ldscript/gcc_arm_flash.ld`.

## Persistent HCI mode and application storage

IOsonata replaces Nordic FDS. The original 12 KB user-data reservation is
preserved, divided into `NVM0` (8 KB for PDS/persistent Bluetooth data) and
`NVM1` (4 KB for application storage). Both are independent flash regions.

| Region | `Debug` / `Release` | `Release_MBR` |
| --- | --- | --- |
| FLASH | 0x00000..0xFCFFF | 0x01000..0xDCFFF |
| NVM0 (8 KB) | 0xFD000..0xFEFFF | 0xDD000..0xDEFFF |
| NVM1 (4 KB) | 0xFF000..0xFFFFF | 0xDF000..0xDFFFF |
| DFU bootloader | none | 0xE0000..0xFDFFF |
| MBR parameters | none | 0xFE000..0xFEFFF |
| DFU settings | none | 0xFF000..0xFFFFF |

HciController's selected HCI transport record uses IOsonata NVM0.
The `Release_MBR` NVM0 start is again 0x000DD000, as in the original UDG
storage placement. The additional NVM1 region is reserved for applications.

The linker addresses are consistent with the standard Nordic USB DFU
bootloader boundaries, and mixed native USB / H:4 runtime validation passed the 77-case hardware
release suite on nRF52840. Confirm the device's actual bootloader
configuration, generated map/HEX and any existing persistent data before
programming; the release suite does not independently verify DFU update
behavior or migrate older stored flash contents.

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
expected workspace. There is no separate manual nrfx, nrfxlib, IOsonata, or
TaktOS setup step for a normal HciController build.

The project links the separately built IOsonata and TaktOS libraries and uses
the nRF52 hard-float SDC/MPSL libraries under the IOcomposer `external` tree.
Native USB sources are provided by the IOsonata library.

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

For UDG-NRF52840 USB DFU, build `Release_MBR`, verify its map and
the installed bootloader boundaries, then validate programming on hardware.
The following is the normal programming sequence:
1. Connect the dongle over USB.
2. Press **SW2 (RESET)** to enter DFU mode.
3. Use Nordic nRF Connect Programmer to load `HciController.hex`.
4. Program the dongle and allow it to reset.

See [../../GETTING_STARTED.md](../../GETTING_STARTED.md) for first-use and HCI
mode instructions.

## Validation

Do not treat a successful compile as complete validation. Run the host/source
suite, verify the generated map for the intended memory layout, and run the
hardware harness appropriate to the board and transport.

For the test entry points and two-controller hardware harness, see
[../../tests/README.md](../../tests/README.md) and
[../../tests/harness/README.md](../../tests/harness/README.md).
