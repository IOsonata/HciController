#!/usr/bin/env python3
"""Host regression for bounded CIS ACL establishment retries."""

import _bootstrap  # noqa: F401
from hcicontroller import hci_cis_pair_test as cis


class FakeHci:
    instances = []

    def __init__(self, target, raw=False):
        self.target = target
        self.raw = raw
        self.closed = False
        FakeHci.instances.append(self)

    def close(self):
        self.closed = True


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def fake_prepare(hci):
    suffix = 1 if hci.target == "central" else 2
    return bytes([suffix, 2, 3, 4, 5, 6]), 1, "test"


def run_timeout_retry():
    originals = (
        cis.IsoHci,
        cis.prepare,
        cis.start_advertising,
        cis.start_connection,
        cis.wait_acl_pair,
        cis.cleanup,
    )
    calls = {"wait": 0, "cleanup": 0}
    FakeHci.instances = []

    def wait_acl_pair(central, peripheral):
        calls["wait"] += 1
        if calls["wait"] == 1:
            raise cis.HciError(
                "timed out waiting for both ACL connection events"
            )
        return 0x0012, 0x0034

    def cleanup(*args):
        calls["cleanup"] += 1

    cis.IsoHci = FakeHci
    cis.prepare = fake_prepare
    cis.start_advertising = lambda *args: None
    cis.start_connection = lambda *args: None
    cis.wait_acl_pair = wait_acl_pair
    cis.cleanup = cleanup
    try:
        result = cis.establish_acl_pair("central", "peripheral")
        central, peripheral, central_acl, peripheral_acl = result
        check(calls["wait"] == 2, "timeout did not retry exactly once")
        check(calls["cleanup"] == 1, "failed setup was not cleaned")
        check(len(FakeHci.instances) == 4, "retry did not open a fresh pair")
        check(FakeHci.instances[0].closed and FakeHci.instances[1].closed,
              "failed pair was not closed")
        check(not central.closed and not peripheral.closed,
              "successful pair was closed")
        check(central_acl == 0x0012 and peripheral_acl == 0x0034,
              "successful ACL handles were not returned")
        central.close()
        peripheral.close()
    finally:
        (
            cis.IsoHci,
            cis.prepare,
            cis.start_advertising,
            cis.start_connection,
            cis.wait_acl_pair,
            cis.cleanup,
        ) = originals


def run_nonretryable_failure():
    originals = (
        cis.IsoHci,
        cis.prepare,
        cis.start_advertising,
        cis.start_connection,
        cis.wait_acl_pair,
        cis.cleanup,
    )
    calls = {"wait": 0, "cleanup": 0}
    FakeHci.instances = []

    def wait_acl_pair(central, peripheral):
        calls["wait"] += 1
        raise cis.HciError(
            "peripheral ACL connection reported role 0, expected 1"
        )

    def cleanup(*args):
        calls["cleanup"] += 1

    cis.IsoHci = FakeHci
    cis.prepare = fake_prepare
    cis.start_advertising = lambda *args: None
    cis.start_connection = lambda *args: None
    cis.wait_acl_pair = wait_acl_pair
    cis.cleanup = cleanup
    try:
        try:
            cis.establish_acl_pair("central", "peripheral")
        except cis.HciError:
            pass
        else:
            raise AssertionError("non-retryable ACL setup failure was hidden")
        check(calls["wait"] == 1, "non-retryable failure was retried")
        check(calls["cleanup"] == 1, "non-retryable setup was not cleaned")
        check(len(FakeHci.instances) == 2, "non-retryable failure opened another pair")
        check(all(hci.closed for hci in FakeHci.instances),
              "failed pair was not closed")
    finally:
        (
            cis.IsoHci,
            cis.prepare,
            cis.start_advertising,
            cis.start_connection,
            cis.wait_acl_pair,
            cis.cleanup,
        ) = originals


def main():
    check(cis._retryable_acl_establishment(
        cis.HciError("central ACL connection failed: 0x3E")), "0x3E not retryable")
    check(cis._retryable_acl_establishment(
        cis.HciError("timed out waiting for both ACL connection events")),
        "ACL event timeout not retryable")
    check(not cis._retryable_acl_establishment(
        cis.HciError("CIS data mismatch")), "CIS failure became retryable")
    run_timeout_retry()
    run_nonretryable_failure()
    print("CIS ACL establishment retry tests passed.")


if __name__ == "__main__":
    main()
