# BLE test harnesses

This directory contains automated BLE test harnesses built around the
HciController dongle.

It is intentionally separate from `tests/hardware/`.

`tests/hardware/` contains low-level HCI tools for developing and debugging the
HciController firmware itself. `tests/harness/` contains reusable test systems
that coordinate one or more radios and produce feature/compliance results for a
DUT.

```text
tests/harness/
    lib/             reusable HCI, pair, capability and result helpers
    hcicontroller/   HciController Controller release/feature validation
    ble_device/      arbitrary BLE-device DUT validation using a dongle
```

## Test instrument and DUT

The HciController dongle is the test instrument. The DUT depends on the
harness:

* `hcicontroller/`: two HciController dongles are used. Each one is a DUT and
  peer for the other so Central, Peripheral, periodic and isochronous roles can
  be exercised over the air.
* `ble_device/`: one HciController dongle is the test instrument. The DUT is an
  arbitrary BLE product or target board. An optional DUT adapter can reset the
  target, change its test mode and read DUT-side results so the run is fully
  automated.

The second form is not IOsonata-specific. An IOsonata BLE-stack validation
firmware can use it, but so can a customer's BLE product.

## Result rules

A feature that the DUT advertises as supported must be exercised positively.
If the harness cannot create the state required to test an advertised feature,
that is an incomplete/failing release result rather than a passing `SKIP`.

`N/A` is reserved for a capability that is genuinely outside the DUT profile,
for example Direction Finding on an nRF52840.

These harnesses validate HCI/Link Layer behavior and BLE feature interaction.
They do not replace Bluetooth SIG qualification, PTS certification evidence or
RF laboratory measurements such as sensitivity and transmitter spectrum.
