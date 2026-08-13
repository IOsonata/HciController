# BLE device DUT harness

This harness uses an HciController dongle as the BLE test instrument and treats
another board or product as the DUT.

The DUT does not have to use IOsonata. The purpose is to make the HciController
dongle useful as a general BLE development and feature-validation instrument.
A customer should be able to buy the dongle, connect it to a workstation and
exercise their own BLE product with the same Python harness library used by the
HciController release tests.

## DUT control

Radio observations always come from the HciController dongle. For full
automation, a DUT adapter implements `harness/lib/dut.py` and controls the DUT
through whatever test interface the product provides: UART, USB, RPC, GPIO or a
dedicated test firmware protocol.

A BLE-stack development target can therefore run a small DUT firmware which
accepts commands such as:

```text
RESET
ADVERTISE <profile>
SCAN <profile>
CONNECT <peer>
SET_PHY <phy>
SET_DATA_LENGTH <octets>
START_PERIODIC_ADV <profile>
START_PAWR <profile>
START_CIS <profile>
START_BIS <profile>
GET_STATUS
GET_COUNTERS
```

The Python harness controls the peer side through the HciController dongle and
cross-checks the DUT-side result when the adapter provides one.

## Intended test profiles

A device profile states what the DUT claims to support. The harness exercises
only those applicable capabilities, but every claimed feature must have a
positive test. Examples include:

* connectable/scannable advertising
* Central and/or Peripheral role
* GATT service discovery and characteristic behavior
* pairing, bonding and privacy
* DLE and PHY changes
* periodic advertising/sync and PAST
* PAwR
* power control/path-loss behavior
* subrating and newer connection timing features
* CIS/BIS when the product exposes LE Audio/ISO capability
* reconnect/reset/persistence behavior
* throughput and long-duration transport stress

This is functional BLE protocol validation. Bluetooth SIG qualification and RF
laboratory measurements remain separate activities.
