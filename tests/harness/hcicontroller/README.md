# HciController release harness

This directory contains the official HciController hardware and release tests.
The reusable controller, transport, and BLE procedure implementation lives in
`python/hcicontroller/` and is imported directly by these programs.

The runners are executable directly from a repository checkout. `_bootstrap.py`
adds the repository `python/` directory to the Python import path, so repository
testing does not require an external `PYTHONPATH` setting or an editable package
installation.

Main programs:

```text
pair_smoke_test.py      basic profile and both ACL role assignments
release_test.py         full release-strict feature/compliance run
probe_test.py           broad HCI command/radio probe
cis_pair_test.py        focused two-controller CIS/ISO over H:4
cis_usb_pair_test.py    focused CIS/ISO over native USB Bulk Serialization
```

The two radios are symmetric. Tests explicitly assign Central/Peripheral,
advertiser/scanner, sender/receiver, and broadcaster/sink roles as required by
each phase.

`release_test.py --transport auto` discovers the two controllers independently.
A release run may therefore use two native USB controllers, two serial H:4
controllers, or one of each. ISO phases automatically select Bluetooth USB Bulk
Serialization only for a controller using native USB; a serial controller stays
on H:4.

The release runner reads the capability profile from both controllers, verifies
the expected release profile, exercises every applicable capability in a valid
radio state, and fails when an advertised feature cannot be exercised
positively. Hardware or controller exclusions are reported as `N/A` with the
reason.

For user-written BLE product validation, install the same public package with:

```sh
python3 -m pip install -e ./python
```

See `python/README.md` for direct HCI use and
`tests/harness/ble_device/README.md` for product-DUT integration.
