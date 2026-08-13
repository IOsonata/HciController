# HciController release harness

This harness validates HciController as the DUT by using two HciController
dongles over the air.

The two radios are intentionally symmetric. A phase may assign dongle A as
Central and B as Peripheral, reset both, then reverse the roles. Periodic and
isochronous phases likewise assign the required advertiser/scanner,
sender/receiver, broadcaster/sink roles explicitly.

The release runner is capability-driven:

1. Read the capability profile from both Controllers.
2. Verify that both report the expected release profile.
3. Build the applicable feature plan from the advertised capability set.
4. Exercise every applicable feature in a real valid radio state.
5. Mark hardware/SDC exclusions as `N/A`, never as a passing `SKIP`.
6. Fail the release when an advertised feature cannot be exercised positively.

Focused tests such as CIS remain useful for debugging one phase. The release
runner should call the same shared helpers rather than carry a second
implementation of connection, event or ISO handling.

The final nRF52840 release matrix is expected to cover at least:

* Central and Peripheral ACL roles, in both assignments
* DLE, PHY update, 2M and Coded PHY
* power control, path loss, SCA and subrating
* Extended Feature Set
* Frame Space Update and Connection Rate / Shorter Connection Intervals
* Periodic Advertising and periodic sync
* PAST sender/receiver in both roles
* PAwR advertiser/scanner
* CIS Central/Peripheral with HCI ISO data
* BIS Source/Sink with HCI ISO data
* reset/recovery and transport stress

Direction Finding, Channel Sounding and encrypted ISO are profile exclusions on
nRF52840 and must be reported as `N/A` with the reason.
