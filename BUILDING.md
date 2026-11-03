# Building HciController

HciController is built with IOcomposer.

The normal setup does not require manually installing Nordic SDKs, IOsonata,
TaktOS, or individual compiler toolchains. The IOcomposer installer
sets up the required development tools, SDK/external repositories, IOsonata,
TaktOS, HciController, and the expected workspace layout so the project is ready
to build.

For normal use of the published firmware, building is not required. See
[GETTING_STARTED.md](GETTING_STARTED.md) to flash and use the released image.

## 1. Install IOcomposer

Use the IOcomposer installer for the host platform.

### macOS

```sh
curl -fsSL https://raw.githubusercontent.com/IOsonata/IOsonata/master/Installer/install_iocdevtools_macos.sh | bash
```

### Linux

```sh
curl -fsSL https://raw.githubusercontent.com/IOsonata/IOsonata/master/Installer/install_iocdevtools_linux.sh | bash
```

### Windows

Run PowerShell as Administrator:

```powershell
iwr -useb https://raw.githubusercontent.com/IOsonata/IOsonata/master/Installer/install_iocdevtools_win.ps1 | iex
```

The installer prepares the complete IOcomposer development environment. Do not
follow a separate manual SDK setup procedure after installation.

## 2. Workspace layout

The default installation root is `~/IOcomposer` on macOS/Linux and the
corresponding IOcomposer directory under the Windows user profile.

The HciController build expects this layout:

```text
IOcomposer/
    external/
        nrfx/
        sdk-nrfxlib/
        ...
    IOsonata/
    HciController/
    TaktOS/
```

IOcomposer installs and maintains the required repositories in these locations.
The HciController project refers to the IOcomposer workspace rather than a set
of manually configured machine-local SDK paths.

## 3. Open HciController in IOcomposer

Open the nRF52840 HciController project from:

```text
HciController/nRF52840/ioc
```

For the UDG-NRF52840 release build, the default board selection is
`UDG_NRF52840`.

Other supported board definitions are:

```text
UDG_NRF52840
IBK_NRF52840
THINGY91_NRF52840
WILDTHING51
WILDTHING91
```

Board selection and build configuration are separate. The board selects the pin
map and legal HCI transports. The build configuration selects optimization and
flash/linker layout.

## nRF54LM20A DK (validated native USB HCI)

Open `HciController/nRF54LM20/ioc` in IOcomposer and use the
matching IOsonata nRF54LM20x library built with SDC support.
HciController uses SDC exclusively and provides only `Debug` and
`Release` configurations; neither configuration name takes an `_SDC`
suffix. A separate SoftDevice build is not supported.
For the **Nordic DK**, explicitly set
`BOARD=NORDIC_DK_NRF54LM20`; otherwise the nRF54LM20 board header
defaults to `UDG_NRF54LM20`. Keep `NRF54LM20B_XXAA`,
`NRF_APPLICATION`, and `NRFXLIB_SDC` defined in both the C and
C++ compiler options. The project uses
`gcc_nrf54lm20a_xxaa.ld`. Additional MBR/DFU configurations
may be introduced later, once the DFU approach and memory layout are defined.

Hardware validation performed on October 16, 2026: a DK and a
UDG-nRF52840 passed the heterogeneous two-controller native USB release
suite (66/66 checks, including 10,000 bidirectional ACL and 1,000
bidirectional ISO stress iterations). Both devices additionally passed
100 native USB reopen/Reset cycles in each HCI alternate setting.
For long-running macOS tests, `caffeinate` prevents normal idle sleep while
Python is running. It does not eliminate all USB or RF timing failures.
See [nRF54LM20/README.md](nRF54LM20/README.md) for scope and remaining
board-specific validation. This does not validate WildThing51 UART H:4.

```sh
caffeinate -dimsu ./.venv/bin/python3 tests/harness/hcicontroller/release_test.py --transport usb
./.venv/bin/python3 tests/harness/hcicontroller/usb_reopen_test.py --usb <DK_USB_SERIAL> --count 100
```

## 4. Build configurations

The nRF52840 project provides these configurations:

| Configuration | Use | Linker script |
| --- | --- | --- |
| `Debug` | no bootloader, SDC controller | `gcc_nrf52840_xxaa.ld` |
| `Release` | optimized no-bootloader, SDC controller | `gcc_nrf52840_xxaa.ld` |
| `Release_MBR` | MBR application origin; storage layout pending | `gcc_nrf52840_xxaa_mbr.ld` |

There is no `Release_SD` configuration in the checked-in project. SDC
uses the regular memory layouts; it does not require a separate `_sdc`
script.

`Release_MBR` must **not** be flashed to an existing UDG USB-DFU device
until the NVM0, FDS, bootloader, and settings locations are reconciled.
The standardized script is not equivalent to the previous UDG flash map.

The configuration name does not select UDG, IBK, Thingy:91, or a WildThing
board.

## 5. Build

Select the required configuration in IOcomposer and build HciController.

The active configuration produces:

```text
HciController.elf
HciController.hex
HciController.bin
HciController.map
```

The `.hex` is normally used for programming the UDG-NRF52840.

The `.map` is important when changing bootloader or memory-layout settings. It
shows the flash regions actually used by the build.

## 6. UDG-NRF52840 USB DFU memory layout

The previous HciController UDG memory map reserved NVM0 at
0x000DD000..0x000DFFFF and placed the USB DFU bootloader at
0x000E0000. The current IOsonata migration script
`gcc_nrf52840_xxaa_mbr.ld` instead places NVM0 at
0x000FD000..0x000FFFFF, which overlaps USB DFU metadata pages.

This is an **unresolved memory-layout incompatibility**. Do not
program `Release_MBR` until the bootloader, FDS, NVM0, MBR parameter
and settings boundaries are resolved and checked against the linker
map and programmed image. Linking successfully is insufficient.

## 7. Flash a local build

For UDG-NRF52840, these steps are **blocked** pending the approved
MBR/DFU memory layout. Do not flash the current `Release_MBR` image.
After the layout is corrected and validated:

1. Build the corrected `Release_MBR` configuration in IOcomposer.
2. Connect the UDG-NRF52840 to USB.
3. Press **SW2 (RESET)** to enter DFU mode.
4. Open Nordic nRF Connect Programmer.
5. Select the UDG DFU device.
6. Load the generated `HciController.hex`.
7. Program the device.
8. Allow it to reset and enumerate in the selected HCI mode.

The HCI mode stored in NVM survives firmware reset and power cycle unless the
mode record is erased or changed by the user button.

## 8. HCI transport selection

UDG-NRF52840 supports:

```text
USB H:4 <-> native USB HCI
```

IBK-NRF52840 supports:

```text
UART H:4 -> USB H:4 -> native USB HCI -> UART H:4
```

Thingy:91, WildThing51, and WildThing91 are UART H:4 controller builds.

The board policy in `nRF52840/src/board.h` determines which modes are legal. Do
not use a build-time default to force a transport that the selected board does
not support.

## 9. Source and hardware validation

A successful build is not the complete validation.

For routine source checks from the HciController repository:

```sh
make -C tests clean
make -C tests run
```

IOcomposer already provides the expected SDK/external layout used by these
tests.

The repository also contains the two-controller hardware harness under
`tests/harness/`. For native USB validation with two explicitly selected
controllers:

```sh
python3 tests/harness/hcicontroller/release_test.py \
    --transport usb --a SERIAL_A --b SERIAL_B
```

See [tests/README.md](tests/README.md) and
[tests/harness/README.md](tests/harness/README.md) for the host and hardware test
organization.

## 10. Updating the development environment

Use the IOcomposer installer/update procedure rather than replacing individual
SDK directories manually. This keeps IOsonata, TaktOS, toolchains, and external
repositories in the layout expected by the project.

For a reproducible build, record the exact revisions of HciController,
IOsonata, TaktOS, nrfx, and sdk-nrfxlib used to produce the binary.

## Related documentation

- [GETTING_STARTED.md](GETTING_STARTED.md): flash and use the released firmware.
- [README.md](README.md): architecture and controller capability overview.
- [USB-HCI.md](USB-HCI.md): native Bluetooth USB transport details.
- [README-commands.md](README-commands.md): command/profile coverage.
- [tests/README.md](tests/README.md): source and hardware test entry points.
