#!/usr/bin/env python3
"""Public Python API regression checks that require no HciController hardware."""

import _bootstrap  # noqa: F401
from hcicontroller import DutControl, ResultBook


def main():
    empty = ResultBook("empty")
    assert not empty.release_passed()
    assert empty.exit_code() == 1
    print("[ok] empty result book cannot report a release PASS")

    passed = ResultBook("pass")
    passed.passed("API", "one procedure")
    assert passed.release_passed()
    assert passed.exit_code() == 0
    print("[ok] recorded PASS result produces a release PASS")

    incomplete = ResultBook("incomplete")
    incomplete.incomplete("API", "one procedure")
    assert not incomplete.release_passed()
    assert incomplete.exit_code() == 1
    print("[ok] incomplete result keeps the release result failed")

    failed = ResultBook("fail")
    failed.failed("API", "one procedure")
    assert not failed.release_passed()
    assert failed.exit_code() == 1
    print("[ok] failed result keeps the release result failed")

    dut = DutControl()
    assert dut.set_test_case("optional-mode", {"value": 1}) is None
    assert dut.read_result() is None
    assert dut.address() is None
    assert dut.stop() is None
    print("[ok] optional DUT hooks are safe no-ops by default")

    print("python_api_test: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
