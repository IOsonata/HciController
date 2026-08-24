# Hardware and release test harness

`tests/harness/` contains the programs that talk to real BLE hardware. The
reusable HCI, transport and BLE procedure implementation is public under
`python/hcicontroller/`; the harness consumes that implementation directly
rather than maintaining a private copy.

```text
python/hcicontroller/            reusable HCI/BLE validation library

tests/harness/
    hcicontroller/               HciController release/focused hardware tests
    ble_device/                  arbitrary BLE-product DUT documentation
```

Install the Python package from the repository root before writing user-facing
validation programs:

```sh
python3 -m pip install -e ./python
```

The repository harness entry points also work directly from the source tree.
They locate `python/hcicontroller/` themselves, so no `PYTHONPATH` setting or
editable installation is required to run the HciController release tests.

## HciController release harness

The official two-controller release runner is:

```sh
python3 tests/harness/hcicontroller/release_test.py
```

Transport selection is explicit when required:

```sh
# Native Bluetooth USB HCI
python3 tests/harness/hcicontroller/release_test.py \
    --transport usb --a SERIAL_A --b SERIAL_B

# Serial/CDC H:4
python3 tests/harness/hcicontroller/release_test.py \
    --transport serial --a /dev/cu.usbmodemA --b /dev/cu.usbmodemB
```

`--transport auto` prefers native USB when compatible native controllers are
present and otherwise selects serial H:4.

The release harness treats inability to exercise an advertised release feature
as a failure/incomplete result rather than a passing skip. `N/A` is reserved for
features outside the nRF52840 target profile.

Focused entry points include:

```text
pair_smoke_test.py      profile + ACL role validation
probe_test.py           broad command/radio probe
cis_pair_test.py        CIS/ISO over H:4
cis_usb_pair_test.py    CIS/ISO over native USB Bulk Serialization
release_test.py         complete release acceptance run
```

## Native USB

Legacy Bluetooth USB HCI uses EP0 for commands, interrupt IN for events and bulk
IN/OUT for ACL. Bulk Serialization uses alternate setting 1 of the Bluetooth
interface and carries an HCI packet indicator on the bulk endpoint pair. The
focused native USB CIS/ISO path uses Bulk Serialization so HCI ISO traffic can
share the packetized bulk transport.

For more than two native controllers, select the required devices by USB serial
number. Do not rely on enumeration order in an automated fixture.

## Reusable Python library

The public library exposes the same controller access used by these release
tests:

- serial H:4 and native Bluetooth USB HCI discovery/transports;
- HCI command/event and ACL handling;
- two-controller coordination;
- connected-role feature helpers;
- periodic advertising, PAST and PAwR procedures;
- CIS/BIS and HCI ISO helpers;
- controller capability reads;
- `DutControl` for product-specific automation;
- result accounting.

See [`../../python/README.md`](../../python/README.md) for direct use. A product
validation program can use this library for exact HCI procedure control or use
Bumble when it needs a complete Bluetooth host stack.

## BLE product DUT use

`tests/harness/ble_device/` describes using an HciController dongle as the radio
test instrument while another board or product is the DUT. The DUT does not
need to use IOsonata or HciController firmware.

A product-specific adapter can implement `hcicontroller.DutControl` over UART,
USB, RPC, GPIO or a dedicated test-firmware interface. BLE observations and
peer-side protocol actions remain on the HciController side.
