#!/usr/bin/env python3
"""Host regression for LE Coded PHY HCI request/result encodings."""

import _bootstrap  # noqa: F401
from hcicontroller import connected_features_pair as pair


class _Book:
    def __init__(self):
        self.rows = []

    def passed(self, *args):
        self.rows.append(args)


def main():
    if pair.PHY_CODED_MASK != 0x04:
        raise AssertionError("LE Set PHY Coded request mask must be 0x04")
    if pair.PHY_CODED != 0x03:
        raise AssertionError("HCI current-PHY Coded enum must be 0x03")
    if pair.PHY_ALL != 0x07:
        raise AssertionError("all-PHY request mask must be 0x07")

    calls = []
    saved = (
        pair._request_phy,
        pair._wait_peer_phy,
        pair._read_phy_exact,
        pair._verify_acl_on_phy,
    )
    try:
        pair._request_phy = lambda hci, handle, phy, expected_phy=None, \
            phy_options=pair.PHY_OPTION_NO_PREFERENCE: calls.append(
                ("request", hci, handle, phy, expected_phy, phy_options)
            )
        pair._wait_peer_phy = lambda hci, handle, phy, timeout=5.0: calls.append(
            ("peer", hci, handle, phy)
        )
        pair._read_phy_exact = lambda hci, handle, phy: calls.append(
            ("read", hci, handle, phy)
        )
        pair._verify_acl_on_phy = lambda central, peripheral, central_handle, \
            peripheral_handle, marker_name: calls.append(
                (
                    "acl",
                    central,
                    peripheral,
                    central_handle,
                    peripheral_handle,
                    marker_name,
                )
            )

        book = _Book()
        pair._run_coded_phy(
            book,
            "A Central -> B Peripheral",
            "central",
            "peripheral",
            0x0123,
            0x0456,
            pair.PHY_OPTION_CODED_S2,
            "S=2 preference",
        )
    finally:
        (
            pair._request_phy,
            pair._wait_peer_phy,
            pair._read_phy_exact,
            pair._verify_acl_on_phy,
        ) = saved

    expected_prefix = [
        ("request", "central", 0x0123, 0x04, 0x03, 0x0001),
        ("peer", "peripheral", 0x0456, 0x03),
        ("read", "central", 0x0123, 0x03),
        ("read", "peripheral", 0x0456, 0x03),
    ]
    if calls[:4] != expected_prefix:
        raise AssertionError("Coded PHY request/result encoding sequence drifted")

    if not any(call[0] == "acl" for call in calls):
        raise AssertionError("Coded PHY gate no longer proves ACL traffic")

    restore = [call for call in calls if call[0] == "request"][-1]
    if restore[3] != pair._base.PHY_1M or restore[4] != pair._base.PHY_1M:
        raise AssertionError("Coded PHY gate no longer restores 1M")

    if len(book.rows) != 1 or "Coded S=2 preference" not in book.rows[0][1]:
        raise AssertionError("Coded PHY pass row is missing")

    print("[ok] Coded PHY uses request mask 0x04 and result enum 0x03")
    print("[ok] Coded PHY gate proves ACL traffic before restoring 1M")


if __name__ == "__main__":
    main()
