# BLE test harnesses

`tests/harness/` is the official hardware/release test system for HciController.
It is self-contained: harness code must not import or execute test code from any
parallel legacy directory.

```text
tests/harness/
    lib/             reusable HCI, USB, pair, capability and result helpers
    hcicontroller/   HciController DUT validation and focused two-dongle tests
    ble_device/      arbitrary BLE-device DUT validation using a dongle
```

## HciController DUT

The HciController harness uses two dongles. Each controller is both DUT and peer
for the other, allowing Central, Peripheral, periodic and isochronous roles to
be exercised over the air.

Main entry points:

```sh
python3 tests/harness/hcicontroller/pair_smoke_test.py
python3 tests/harness/hcicontroller/release_test.py
python3 tests/harness/hcicontroller/cis_pair_test.py
python3 tests/harness/hcicontroller/cis_usb_pair_test.py
python3 tests/harness/hcicontroller/probe_test.py --help
```

`pair_smoke_test.py` and `release_test.py` accept
`--transport {auto,serial,usb}`. `auto` prefers native Bluetooth USB HCI when
native controllers are present, then falls back to serial H:4. With native USB,
`--a` and `--b` select controllers by USB serial number; with serial H:4 they
name device paths.

A complete native USB release run with two explicitly selected dongles is:

```sh
python3 tests/harness/hcicontroller/release_test.py \
    --transport usb \
    --a SERIAL_A \
    --b SERIAL_B
```

The ordinary release phases use legacy Bluetooth USB HCI alternate setting 0.
CIS, BIS and the combined ACL/ISO stress phase switch the same controllers to
Bulk Serialization alternate setting 1 because HCI ISO is carried on the bulk
endpoint pair there. The sequence then returns to legacy mode for later
non-ISO phases, so the release runner also exercises alternate-setting changes
between independent sessions.

`cis_usb_pair_test.py` remains the focused native USB CIS/ISO diagnostic. It
opens two native Bluetooth USB HCI controllers directly in Bulk Serialization
mode. With exactly two attached it auto-selects both; otherwise use
`--central USB-SERIAL --peripheral USB-SERIAL`.

The reusable transport layer in `lib/hci_transport.py` supports serial H:4 and
native Bluetooth USB HCI. Native USB legacy mode uses EP0/interrupt/bulk as
specified by the Bluetooth USB transport; Bulk Serialization uses the bulk pair
and HCI packet indicators, including ISO. `lib/pair_transport.py` resolves a
stable two-controller pair for the release harness and derives Bulk
Serialization specs only for ISO phases.

## BLE-device DUT

`ble_device/` uses one HciController as the test instrument and treats another
board or product as the DUT. A DUT adapter can control the product through its
own UART, USB, RPC, GPIO or test firmware interface.

## Result rules

A feature advertised by the DUT must be exercised positively. If the harness
cannot create the state required to test an advertised feature, the release
result is incomplete/failing rather than a passing `SKIP`.

`N/A` is reserved for capability genuinely outside the DUT profile, for example
Direction Finding on an nRF52840.

The harness validates HCI/Link Layer behavior and BLE feature interaction. It
does not replace Bluetooth SIG qualification, PTS certification evidence or RF
laboratory measurements.
