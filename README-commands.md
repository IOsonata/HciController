# HCI command coverage

HciController does not maintain a hand-copied release command count in this
file. The command surface is source-defined and checked mechanically because a
static list becomes wrong as soon as the SDC profile moves.

Two dispatch sources make up the externally reachable command surface:

```text
src/hci_sdc_nrfxlib.cpp   commands routed directly to nrfxlib SDC
src/hci_sdc.cpp           supplemental Core/profile/vendor routing
```

The hardware tooling accounts for the same surface through:

```text
tests/harness/lib/hci_commands_catalog.py
tests/harness/lib/hci_commands.py
tests/harness/lib/target_profile.py
```

`tests/command_coverage.py` resolves the real nrfxlib opcode values, reads both
firmware dispatch tables, and requires every exposed opcode to be either in the
broad command catalog or in explicit target-profile coverage. An opcode exposed
by the firmware but not driven by the release tooling is a test failure. An
opcode driven by the tooling but not exposed by the firmware is also a failure.

Run the release form of the check with the real nrfxlib tree available:

```sh
make -C tests run NRFXLIB_DIR=../external/sdk-nrfxlib
```

## Release profile

`include/hci_core_profile.h` sets the nRF52840 release profile to Bluetooth Core
6.2. The profile is the intersection of what the product exposes and what the
selected nRF52840 multirole SoftDevice Controller can actually support. It is
not a Bluetooth SIG qualification claim.

The exposed command families include:

- Controller/Baseband, information and status commands used by normal LE hosts;
- connection management and filter accept list;
- privacy/resolving-list control;
- LE encryption and key-request handling;
- data length and PHY control;
- legacy advertising/scanning;
- extended advertising/scanning/initiating;
- periodic advertising, periodic synchronization and sync transfer;
- PAwR command paths supported by the configured SDC resources;
- LE Power Control and path-loss reporting;
- Sleep Clock Accuracy update/request support;
- connection subrating;
- Extended Feature Set support;
- Direct Test Mode command versions exposed by the SDC profile;
- connected and broadcast isochronous setup/data-path/test commands;
- Core 6.2 Frame Space Update and Shorter Connection Intervals command paths;
- Nordic SDC vendor commands used by the release profile;
- HciController vendor diagnostics, including VS Read Counters.

The exact opcodes and safe probe payloads are in the harness command catalog.
That catalog is what release testing executes against hardware.

## Supported Commands bitmap

`HCI_Read_Local_Supported_Commands` is built from the actual standard command
surface. A host test checks the bitmap and the dispatch table against one
another in both directions where the Bluetooth Core specification assigns a
Supported Commands bit.

Supplemental commands that do not map cleanly into that standard bitmap are
covered separately by the target profile; they are not made invisible just to
make the bitmap test simpler.

## Commands deliberately not advertised

Controller-to-host ACL flow control is not advertised. In the current nRF52840
SDC configuration, Host Buffer Size is refused by the controller with status
0x11. Advertising the three host-flow-control commands would therefore promise
a capability the controller does not provide. Their rows and Supported Commands
bits remain absent together.

Direction Finding is outside the nRF52840 target profile.

The nRF52840 also does not provide encrypted isochronous-channel packets in the
current SoftDevice Controller. That is a link-layer hardware/controller
limitation, not an HCI transport limitation; the unencrypted CIS/BIS command and
ISO data paths remain exposed.

## LE Read Supported States

The multirole SDC archive does not provide the older direct
`sdc_hci_cmd_le_read_supported_states` entry point used by some SDC variants.
HciController supplies the product-level LE Supported States result in
`src/hci_sdc.cpp` instead. The host test verifies that the concurrency claim is
backed by the configured Central, Peripheral, advertising, scanning and
parallel scan/initiate resources.

This is intentionally different from inventing an unsupported SDC command: the
returned state bitmap describes the product's configured concurrency profile and
is checked against that profile.

## Public and static random addresses

The nRF52840 board does not carry a programmed public Bluetooth device address,
so `HCI_Read_BD_ADDR` reports all zeroes. A host must not assume public address
type 0x00 is available just because the command exists.

The Nordic static-address vendor command reports the static random identity
derived from FICR. The hardware harness resolves an identity before running
advertising, scanning or initiating procedures so a missing public address is
not misdiagnosed as a failure of the following HCI command.

## HciController VS Read Counters

`0xFFF0` is the HciController diagnostic command. It is answered in the routing
layer without asking the radio and reports counters spanning the H:4 parser,
transport, bridge and SDC routing paths.

The wire schema is versioned in `hci_counters.h`. Fields are appended and the
schema version is raised; existing fields are not renumbered. The Python harness
checks that its decoder and the firmware schema stay in agreement.

One reason the counters exist is the measured nRF52840 SDC ACL behavior: a
packet beyond the controller-advertised ACL credit can be accepted by the SDC
API without ever producing a matching Number Of Completed Packets event. The
routing layer therefore enforces the known host credit after LE Read Buffer Size
has established it and counts any overrun it refuses.

## Handler rules

Fixed-size handlers take parameter/return sizes from the nrfxlib types and pin
wire-critical sizes with static assertions where needed.

Variable-length commands validate the complete HCI Parameter_Total_Length
against the count fields inside the command before calling SDC. This is exact,
not merely a minimum-length check. SDC command structures contain trailing
arrays and a short packet with a larger count must not be allowed to read bytes
left in a reused receive buffer.

Each dispatch entry also declares the response kind. Commands that complete
asynchronously use Command Status; synchronous commands use Command Complete.
The distinction is part of the host test coverage because a host waiting for the
wrong completion form behaves like the controller never answered.

## Vendor symbol availability

The nrfxlib headers describe a broader API than every library variant exports.
A declaration in a header is therefore not proof that the selected nRF52 archive
contains the entry point.

Run:

```sh
python3 tests/sdc_symbols.py ../external/sdk-nrfxlib/softdevice_controller/lib/nrf52/hard-float/libsoftdevice_controller_multirole.a
```

The symbol check reports missing functions named by the release dispatch table.
A new nrfxlib revision is not release-ready until this check and the real-header
dispatch/resource tests pass.

## Release hardware coverage

The broad command probe drives the catalog against a real controller and handles
state requirements such as a live connection, an advertising set, a periodic
sync or explicit operator consent. Commands that require a second radio or a
specific role are exercised by the two-controller release phases rather than by
sending them in a meaningless state and treating Command Disallowed as proof of
support.

The full release runner is:

```sh
python3 tests/harness/hcicontroller/release_test.py --help
```

See `tests/README.md`, `tests/harness/README.md` and `RELEASE.md` for the release
gate.
