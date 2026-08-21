# HciController release harness

This harness validates HciController as the DUT by using two HciController
dongles over the air. It is the official board/release test surface for this
repository.

The two radios are intentionally symmetric. A phase may assign dongle A as
Central and B as Peripheral, reset both, then reverse the roles. Periodic and
isochronous phases likewise assign advertiser/scanner, sender/receiver and
broadcaster/sink roles explicitly.

Main programs:

```text
pair_smoke_test.py      basic profile + both ACL role assignments
release_test.py         full release-strict feature/compliance run
probe_test.py           broad HCI command/radio probe
cis_pair_test.py        focused two-controller CIS/ISO over H:4
cis_usb_pair_test.py    focused CIS/ISO over native USB Bulk Serialization
```

Reusable protocol, transport and feature logic belongs in `../lib/`. Focused
programs here should call those helpers instead of carrying separate connection,
event, command or ISO implementations.

The release runner is capability-driven:

1. Read the capability profile from both Controllers.
2. Verify that both report the expected release profile.
3. Build the applicable feature plan from the advertised capability set.
4. Exercise every applicable feature in a real valid radio state.
5. Mark hardware/SDC exclusions as `N/A`, never as a passing `SKIP`.
6. Fail the release when an advertised feature cannot be exercised positively.

The nRF52840 release matrix covers Central/Peripheral ACL roles, DLE, PHY,
power control/path loss, SCA, subrating, Extended Feature Set, newer connection
timing procedures, periodic advertising/sync, PAST, PAwR, CIS, BIS,
reset/recovery and long-running ACL/ISO/event stress.

Direction Finding, Channel Sounding and encrypted ISO are profile exclusions on
nRF52840 and must be reported as `N/A` with the reason.
