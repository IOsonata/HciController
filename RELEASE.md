# Release procedure

This file defines the release gate for HciController. The first source release
is `v1.0.0`.

A tag is created only after the exact commit being tagged has passed the source,
build and hardware checks below. A result from an earlier commit is useful
debugging evidence, but it is not release evidence for a later commit.

## Version

Release 1.0.0 is encoded in `include/hci_version.h` as:

```text
FIRMWARE_VERSION = 0x0100
```

The native USB `bcdDevice` value is derived from the same definition. Do not
maintain a second release version in the USB descriptor or Eclipse project.

The controller-facing Bluetooth Core version is defined independently in
`include/hci_core_profile.h`. Release 1 targets the nRF52840/current nrfxlib
capability profile and reports Core 6.2. That is an HCI/controller capability
profile, not a Bluetooth SIG qualification claim.

## Dependency record

HciController links separately built repositories and SDK libraries. Before the
tag, record the exact revisions used for the release build:

```sh
git -C ../IOsonata rev-parse HEAD
git -C ../TaktOS rev-parse HEAD
git -C ../external/tinyusb rev-parse HEAD
git -C ../external/nrfx rev-parse HEAD
git -C ../external/sdk-nrfxlib rev-parse HEAD
```

Put those revisions in the GitHub release notes together with the tagged
HciController commit. The IOcomposer installer normally follows repository
HEADs, so the HciController tag alone is not enough to reproduce a binary.

## Host/source checks

Run from a clean checkout of the exact release candidate:

```sh
make -C tests clean
make -C tests run
```

The test Makefile resolves the default real nrfxlib tree as
`$(ROOT)/../external/sdk-nrfxlib`. If the release checkout uses a different
location, override it with an absolute path:

```sh
make -C tests run NRFXLIB_DIR=/absolute/path/to/sdk-nrfxlib
```

Do not pass `NRFXLIB_DIR=../external/sdk-nrfxlib` with `make -C tests`; GNU make
changes into `tests/` before resolving that command-line relative path, which
points at `HciController/external/sdk-nrfxlib` instead of the sibling external
tree.

For a release run, the real-nrfxlib checks must execute; a skip caused by a
missing `NRFXLIB_DIR` is not release evidence.

The host suite compiles C as GNU C17 and C++ as GNU C++23, matching the
nRF52840 Eclipse project. Do not weaken the host language standard to make a
warning disappear; a warning that appears under the target standard is a
release defect until resolved.

The checks include command-table/catalog agreement, SDC symbol availability,
resource-profile consistency, USB descriptor/state-machine rules, counter
schema, harness organization, board policy and persistent mode-switch wiring.

Before the build, run the source hygiene checks from `CODING.md` and the
IOsonata coding standard on every touched source file. In particular:

- source and documentation must be ASCII;
- added source text must pass the prohibited-word check;
- touched source files must have balanced braces;
- `.project` and `.cproject` must parse as XML;
- Eclipse linked resources must not contain machine-local absolute paths;
- all four nRF52840 C++ configurations must remain GNU C++23;
- obsolete project-level USB PID or USB release defines must not be present;
  USB PIDs are mode-specific in `usb_descriptors.c` and `bcdDevice` comes from
  `HCI_CONTROLLER_VERSION_BCD`.

## nRF52840 build configurations

The Eclipse project is `nRF52840/ioc`.

| Configuration | Purpose | Linker script |
| --- | --- | --- |
| `Debug` | debugger/no bootloader development | `nrf52840_xxaa_sdc.ld` |
| `Release` | no-bootloader release-style build | `nrf52840_xxaa_sdc.ld` |
| `Release_MBR` | USB DFU/MBR layout | `nrf52840_xxaa_sdc_mbr.ld` |
| `Release_SD` | S140-compatible application-origin layout | `nrf52840_xxaa_s140_sdc.ld` |

The project selects the script by filename and supplies the IOsonata linker
search directory. This is intentional: the linker owns the NVM location, and a
map from a different script is not equivalent even if the source is identical.
The configuration name selects the linker layout only; it does not select a
board.

### Required linker-owned NVM maps

For the USB DFU/MBR layout:

```text
application      0x001000 .. 0x0DCFFF
NVM0             0x0DD000 .. 0x0DFFFF
USB DFU loader   0x0E0000 ..
```

For the current S140-compatible linker layout:

```text
application      0x027000 .. 0x0DCFFF
NVM0             0x0DD000 .. 0x0DFFFF
BT_PDS           0x0FD000 .. 0x0FEFFF
settings         0x0FF000 .. 0x0FFFFF
```

The current `nrf52840_xxaa_s140_sdc.ld` does not define an `NVM0` region at
`0xF5000` and does not define the OTA bootloader region itself. Do not infer
flash geometry from the `Release_SD` name or from the board being built. If a
bootloader is installed, verify its occupied flash separately against the exact
IOsonata linker-script revision and the generated map.

For a no-bootloader/debug layout, the current IOsonata SDC linker reserves:

```text
application      0x000000 .. 0x0FCFFF
NVM0             0x0FD000 .. 0x0FFFFF
```

Before tagging, inspect the generated `.map` for every release configuration
that will be published. The NVM address must match the selected linker script
and must not overlap any installed bootloader or other persistent storage. A
successful link by itself is not sufficient evidence of the external bootloader
layout.

HciController uses only the first erase page of `NVM0` for the mode record. The
remaining reserved pages stay available to the application/platform layout.

## Hardware release checks

### UDG-NRF52840x, USB DFU build

Use `Release_MBR` and verify at least:

- cold boot in native USB HCI;
- native USB enumeration and CDC log enumeration;
- user-button switch native USB HCI -> USB H:4;
- user-button switch USB H:4 -> native USB HCI;
- selected mode survives a power cycle in both directions;
- startup log reports `mode: nvm addr=0x000DD000 size=12288`;
- no bootloader/reset loop during the NVM write/reset sequence.

### IBK-NRF52840

Build with `BOARD=IBK_NRF52840` and select the linker configuration that matches
the flash layout installed on the test board. The Eclipse configuration name is
not the board selector. Verify:

- UART H:4 -> USB H:4;
- USB H:4 -> native USB HCI;
- native USB HCI -> UART H:4;
- selected mode survives a power cycle in each mode;
- startup log reports the `NVM0` address from the selected linker script before
  the radio is started (`0x000DD000` for the current `Release_SD` script and
  `0x000FD000` for the no-bootloader `Release` script).

### UART-only boards

If Thingy:91, WildThing51 or WildThing91 are claimed by the release, run their
board-specific startup test. They remain UART H:4 only and must not expose the
mode-switch path.

For Thingy:91, preserve the measured RTS/CTS assignment and the startup NOP
behavior described in `README.md` and `board.h`.

## Official two-controller harness

Run the release harness on the transports that are part of the release claim.
For native USB with two explicitly selected controllers:

```sh
python3 tests/harness/hcicontroller/release_test.py \
    --transport usb \
    --a SERIAL_A \
    --b SERIAL_B
```

Run the serial/H:4 release path as well when UART H:4 is part of the release
claim. Focused CIS/ISO diagnostics remain available through
`cis_pair_test.py` and `cis_usb_pair_test.py`.

A feature advertised by the controller must be exercised positively. The
release harness treats inability to create the required state as incomplete or
failing, not as a passing skip.

## USB identity

The open-source tree defaults to development USB IDs (`0xCAFE` and development
PIDs). That is acceptable for a source/development release. A shipping product
build must supply assigned IDs and should define:

```text
HCI_USB_REQUIRE_ASSIGNED_IDS=1
```

Do not describe a development-ID binary as a production USB product image.

## Known nRF52840 limitation

Unencrypted connected/broadcast isochronous operation is supported by the
configured controller profile. nRF52840 does not support encrypted
isochronous-channel packets in the current SoftDevice Controller. See
`README.md` for the hardware limitation.

This limitation is not a reason to hide the unencrypted ISO capability, but the
release notes must not claim encrypted ISO support.

## Artifact check

Archive the release build outputs that matter for the chosen configuration:

```text
HciController.elf
HciController.hex
HciController.bin
HciController.map
```

Keep the `.map` with the binary release evidence because it proves which memory
layout was actually linked.

Record hashes for published binary artifacts.

## Tagging

After all release checks pass on the exact candidate commit:

```sh
git tag -a v1.0.0 <release-commit> -m "HciController 1.0.0"
git push origin v1.0.0
```

Do not move or recreate a published release tag. A post-release correction is a
new version and a new tag.
