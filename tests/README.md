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
nRF52840 target project. Target dependencies such as nRF, MPSL, TinyUSB, TaktOS
and the SoftDevice Controller are replaced by the fakes under `stubs/`, except
where a test deliberately compiles against the real nrfxlib headers.

The Makefile looks for the real nrfxlib tree at the sibling path
`$(ROOT)/../external/sdk-nrfxlib`. If nrfxlib is elsewhere, override it with an
absolute path:

```sh
make -C tests run NRFXLIB_DIR=/absolute/path/to/sdk-nrfxlib
```

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
# Basic two-dongle profile and ACL-role validation
python3 tests/harness/hcicontroller/pair_smoke_test.py

# Full release-strict HciController validation
python3 tests/harness/hcicontroller/release_test.py

# Focused CIS/ISO test over H:4 controllers
python3 tests/harness/hcicontroller/cis_pair_test.py

# Focused CIS/ISO test over two native Bluetooth USB HCI controllers
python3 tests/harness/hcicontroller/cis_usb_pair_test.py

# Broad command/radio probe
python3 tests/harness/hcicontroller/probe_test.py --help
```

The native USB CIS test auto-selects exactly two compatible controllers. When
more than two are connected, select them by USB serial number:

```sh
python3 tests/harness/hcicontroller/cis_usb_pair_test.py \
    --central SERIAL_A \
    --peripheral SERIAL_B
```

Native USB legacy mode carries commands on EP0, events on interrupt IN and ACL
on bulk IN/OUT. Bulk Serialization uses alternate setting 1 and carries the HCI
packet indicator on the bulk endpoint pair; the focused native USB CIS test uses
Bulk Serialization because HCI ISO is carried there.

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
