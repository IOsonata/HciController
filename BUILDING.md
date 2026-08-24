# Building HciController

HciController is built with IOcomposer.

The normal setup does not require manually installing Nordic SDKs, TinyUSB,
IOsonata, TaktOS, or individual compiler toolchains. The IOcomposer installer
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
        tinyusb/
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

## 4. Build configurations

The nRF52840 project provides these configurations:

| Configuration | Use | Linker script |
| --- | --- | --- |
| `Debug` | debugger/no-bootloader development | `nrf52840_xxaa_sdc.ld` |
| `Release` | optimized no-bootloader build | `nrf52840_xxaa_sdc.ld` |
| `Release_MBR` | USB DFU/MBR layout | `nrf52840_xxaa_sdc_mbr.ld` |
| `Release_SD` | S140-compatible application origin | `nrf52840_xxaa_s140_sdc.ld` |

For the standard UDG-NRF52840 with its USB DFU bootloader, use
`Release_MBR`.

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

The current `Release_MBR` layout is:

```text
application      0x001000 .. 0x0DCFFF
NVM0             0x0DD000 .. 0x0DFFFF
USB DFU loader   0x0E0000 ..
```

HciController uses the first erase page of `NVM0` for persistent HCI mode
selection.

At startup, a UDG build using this layout should report:

```text
mode: nvm addr=0x000DD000 size=12288
```

If a different bootloader is installed, use the matching build configuration
and verify the generated map before programming the board.

## 7. Flash a local build

For UDG-NRF52840:

1. Build the `Release_MBR` configuration in IOcomposer.
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
IOsonata, TaktOS, TinyUSB, nrfx, and sdk-nrfxlib used to produce the binary.

## Related documentation

- [GETTING_STARTED.md](GETTING_STARTED.md): flash and use the released firmware.
- [README.md](README.md): architecture and controller capability overview.
- [USB-HCI.md](USB-HCI.md): native Bluetooth USB transport details.
- [README-commands.md](README-commands.md): command/profile coverage.
- [tests/README.md](tests/README.md): source and hardware test entry points.
