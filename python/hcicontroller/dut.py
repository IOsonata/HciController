#!/usr/bin/env python3
"""Optional DUT-control interface for automated BLE-device harnesses."""


class DutControl:
    """
    Adapter between a BLE-device harness and product-specific DUT controls.

    A customer can implement these hooks over UART, USB, RPC, GPIO or a test
    firmware command channel. Harness code should use only this interface and
    HCI/radio observations from the HciController dongle.
    """

    def name(self):
        return self.__class__.__name__

    def reset(self):
        raise NotImplementedError

    def wait_ready(self, timeout=5.0):
        raise NotImplementedError

    def start_advertising(self):
        raise NotImplementedError

    def start_scanning(self):
        raise NotImplementedError

    def start_connectable(self):
        """Put the DUT in the mode the dongle should connect to."""
        return self.start_advertising()

    def start_central(self):
        """Put the DUT in a mode where it will initiate to the dongle."""
        raise NotImplementedError

    def address(self):
        """Return the DUT identity address when the adapter can provide it."""
        return None

    def set_test_case(self, name, parameters=None):
        """Optional extension point for DUT-specific feature modes."""
        raise NotImplementedError

    def read_result(self):
        """Optional DUT-side result/log data for cross-checking radio evidence."""
        return None

    def stop(self):
        pass
