#!/usr/bin/env python3
"""Regression tests for Bluetooth Core conditional command requirements."""

import unittest

import _bootstrap  # noqa: F401
from hcicontroller.hci_core_conditions import (
    FEAT_ADV_CODING_SELECTION,
    validate_conditional_commands,
)


def set_bit(data, bit):
    data[bit // 8] |= 1 << (bit % 8)


class CoreConditionsTest(unittest.TestCase):
    def test_advertising_coding_requires_host_feature_command(self):
        commands = bytearray(64)
        features = bytearray(8)
        set_bit(features, FEAT_ADV_CODING_SELECTION)

        errors = []
        validate_conditional_commands(commands, features, errors)

        self.assertTrue(any("LE Set Host Feature" in err for err in errors))
        self.assertTrue(any(
            "LE Set Extended Advertising Parameters v2" in err
            for err in errors
        ))

    def test_advertising_coding_commands_satisfy_conditions(self):
        commands = bytearray(64)
        features = bytearray(8)
        set_bit(features, FEAT_ADV_CODING_SELECTION)
        commands[44] |= 1 << 1
        commands[46] |= 1 << 2

        errors = []
        validate_conditional_commands(commands, features, errors)

        self.assertEqual(errors, [])


if __name__ == "__main__":
    unittest.main()
