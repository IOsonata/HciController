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

The linker script is named in the project configuration by basename. The project
adds the common IOsonata linker directory and the nRF52840 linker directory to
the linker search path instead of embedding one machine-specific absolute path.

## Build configurations

| Configuration | Use | C/C++ linker script |
| --- | --- | --- |
| `Debug` | bare-metal flash layout with SDC controller | `gcc_nrf52840_xxaa.ld` |
| `Release` | optimized bare-metal flash layout with SDC controller | `gcc_nrf52840_xxaa.ld` |
| `Release_MBR` | MBR origin; **DFU/storage map not approved** | `gcc_nrf52840_xxaa_mbr.ld` |

All configurations use the nrfxlib SoftDevice Controller (SDC). SDC
does not require a dedicated linker variant. The IOsonata linker files live
under `ARM/Nordic/nRF52/ldscript`, with the common section definitions in
`ARM/ldscript/gcc_arm_flash.ld`.

The Eclipse project currently has no `Release_SD` configuration. Supporting
an application alongside S140 requires a separate, verified memory layout.

## Persistent HCI mode storage

HciController uses IOsonata `NVM0` for the persistent HCI transport mode.
The build must reserve storage that does not overlap the bootloader, FDS,
settings or other nonvolatile data.

- `Debug` / `Release`: FLASH starts at 0x00000000, ends before NVM0
  at 0x000FD000, NVM0 occupies 0x000FD000..0x000FFFFF.
- `Release_MBR`: FLASH starts at 0x00001000, NVM0 is currently
  0x000FD000..0x000FFFFF in the IOsonata 0.13 migration script.

**Do not flash `Release_MBR` on a USB DFU dongle yet.** The currently
selected IOsonata `gcc_nrf52840_xxaa_mbr.ld` is not equivalent to the
previous HciController UDG layout (NVM0 at 0x000DD000, USB DFU loader
from 0x000E0000). The new NVM0 range overlaps the conventional USB DFU
settings and MBR parameter pages. The MBR/DFU/FDS/NVM partition is still
unresolved. A successful link or HEX merge does not validate that memory map.

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

For UDG-NRF52840 USB DFU, first validate and correct the MBR/DFU/FDS/NVM
partition. Do not program the current `Release_MBR` image.
Once the memory layout is approved and rebuilt:
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
