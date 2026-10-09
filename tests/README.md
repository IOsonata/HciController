# HciController tests

The repository has two test layers plus a reusable host-side Python library.

```text
python/
    hcicontroller/           public HCI/BLE validation library
    examples/                user-facing library examples

tests/
    unit/                    native C++ host tests
    stubs/                   target/vendor fakes used by host tests
    iosonata/                real host-buildable IOsonata support
    harness/                 official hardware/release test system
        hcicontroller/       two-HciController release/focused tests
        ble_device/          arbitrary BLE-device DUT harness
```

## Host tests

Run the complete host suite with:

```sh
make -C tests clean
make -C tests run
```

The host suite compiles C as GNU C17 and C++ as GNU C++23, matching the
nRF52840 target project. Target dependencies such as nRF, MPSL, IOsonata,
TaktOS and the SoftDevice Controller are replaced by the fakes under `stubs/`, except
where a test deliberately compiles against the real nrfxlib headers. IOsonata
is not faked: the host builds include its real headers from the sibling
`../IOsonata` checkout (override with `IOSONATA_ROOT`), and `hci_usb_test`
compiles the real USB core, `BtHciUsb` and `UsbdCdc` against a fake controller
port, so an IOsonata API change fails here before it fails on the target.

The USB test uses the actual `hci_app.cpp` setup and worker queue. It covers
all three USB layouts, queue saturation and retry, bounded draining, stop/start
with a pending process event, and a log-only cable attached after startup.
The nRF52840 test links IOsonata's real shared POWER_CLOCK dispatcher to catch
duplicate vector ownership and checks MPSL crystal request/release and failure
cleanup. These host tests do not establish hardware timing or enumeration.

The Makefile looks for the real nrfxlib tree at the sibling path
`$(ROOT)/../external/sdk-nrfxlib`. If nrfxlib is elsewhere, override it with an
absolute path:

```sh
make -C tests run NRFXLIB_DIR=/absolute/path/to/sdk-nrfxlib
```

The default PAwR completion contract follows current nrfxlib (DRGN-29455):
the direct response-data handler returns status immediately. Nordic exposes no
`SDC_HCI_PAWR_SYNC_RETURN_IMMEDIATELY` feature macro. Only builds using an older
SDK that documents a delayed completion through `sdc_hci_get()` should define
`HCI_SDC_LEGACY_PAWR_COMPLETION=1` consistently across all translation units.
The ordinary `hci_sdc_test` checks that successful and rejected `0x2083`
commands return exactly one completion and allow a subsequent Reset without
depending on vendor feature macros. The real-header critical dispatch test
also checks the selected SDK contract.

A release run must not report that the real-header SDC dispatch, critical or
resource tests were skipped.

The Python host checks validate repository/project consistency, board pin maps,
USB descriptor/runtime invariants, command coverage, SDC symbol availability,
SMP vectors, connection-event parsing, CIS cleanup ordering, native USB
transport behavior and the firmware/Python counter schema.

`command_coverage.py` compares the complete externally reachable command
profile, including the SDC dispatch tables and bridge-local HciController
commands, with the release command profile in
`python/hcicontroller/hci_commands.py`. Dedicated target-profile coverage
metadata lives in `python/hcicontroller/target_profile.py`.

The counter-schema check reads the canonical decoder sources from
`python/hcicontroller/` so moving the library cannot leave the release check
validating a stale private copy.

## Public Python validation library

Reusable HCI packet parsing, command catalog data, serial/native-USB transports,
controller coordination and BLE feature helpers live only in:

```text
python/hcicontroller/
```

Install the package from the repository with:

```sh
python3 -m pip install -e ./python
```

The official hardware harness consumes the same implementation directly. Its
entry points locate the in-tree package automatically, so running repository
tests does not require an external `PYTHONPATH` setting or an editable install.

See `python/README.md` for direct-HCI use and examples. Bumble remains an
alternative when a validation program needs a complete Bluetooth host stack
rather than direct controller procedures.

## Official hardware/release harness

All tests that talk to real controllers live under `tests/harness/`.

The two-controller HciController harness is under:

```text
tests/harness/hcicontroller/
```

Useful entry points are:

```sh
# One native USB dongle: repeated reopen/reset in both HCI alternates
python3 tests/harness/hcicontroller/usb_reopen_test.py

# Basic two-dongle profile and ACL-role validation
python3 tests/harness/hcicontroller/pair_smoke_test.py

# Full release-strict HciController validation
python3 tests/harness/hcicontroller/release_test.py

# Focused CIS/ISO test over H:4 controllers
python3 tests/harness/hcicontroller/cis_pair_test.py

# Broad command/radio probe
python3 tests/harness/hcicontroller/probe_test.py --help
```

Native USB alternate 0 carries commands on EP0, events on interrupt IN and ACL
on bulk IN/OUT. Alternate 1 is Bulk Serialization: every packet type, ISO
included, on the bulk pair with an H:4 indicator. `cis_usb_pair_test.py` opens
both controllers that way. No SCO alternates are advertised; the controller is
LE only.

## Harness organization

Focused HciController programs in `tests/harness/hcicontroller/` use the public
`python/hcicontroller/` helpers rather than carrying independent transport or
protocol implementations.

`tests/harness/ble_device/` uses an HciController dongle as a BLE test
instrument for another product or board. The DUT does not need to use IOsonata.
A product-specific DUT adapter may implement `hcicontroller.DutControl` over any
available control channel.

A feature advertised by the DUT must be exercised positively. If the harness
cannot create the required state, the release result is incomplete/failing;
`N/A` is reserved for capability outside the target profile.


