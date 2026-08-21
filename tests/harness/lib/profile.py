#!/usr/bin/env python3
"""Generic Controller capability reads for BLE harnesses."""

OP_RESET = 0x0C03
OP_READ_CONN_ACCEPT_TIMEOUT = 0x0C15
OP_WRITE_CONN_ACCEPT_TIMEOUT = 0x0C16
OP_READ_LOCAL_VERSION = 0x1001
OP_READ_LOCAL_SUPPORTED_COMMANDS = 0x1002
OP_LE_READ_LOCAL_SUPPORTED_FEATURES = 0x2003
OP_LE_READ_SUPPORTED_STATES = 0x201C
OP_LE_READ_ALL_LOCAL_SUPPORTED_FEATURES = 0x2087
OP_LE_READ_MIN_SUPPORTED_CONN_INTERVAL = 0x20A3


def command_supported(commands, octet, bit):
    return len(commands) > octet and bool(commands[octet] & (1 << bit))


def read_controller_capabilities(hci, reset=True):
    """Read capability data without applying any product-specific policy."""
    if reset:
        hci.command(OP_RESET)

    status, version = hci.command(OP_READ_LOCAL_VERSION, allow_fail=True)
    if status != 0:
        raise RuntimeError("Read Local Version failed with 0x%02X" % status)

    status, commands = hci.command(
        OP_READ_LOCAL_SUPPORTED_COMMANDS, allow_fail=True
    )
    if status != 0:
        raise RuntimeError(
            "Read Local Supported Commands failed with 0x%02X" % status
        )

    status, features = hci.command(
        OP_LE_READ_LOCAL_SUPPORTED_FEATURES, allow_fail=True
    )
    if status != 0:
        raise RuntimeError(
            "LE Read Local Supported Features failed with 0x%02X" % status
        )

    capabilities = {
        "version": version,
        "commands": commands,
        "features": features,
        "states": None,
        "all_local_features": None,
        "minimum_connection_interval": None,
        "connection_accept_timeout": None,
    }

    status, states = hci.command(OP_LE_READ_SUPPORTED_STATES, allow_fail=True)
    if status == 0:
        capabilities["states"] = states

    if command_supported(commands, 47, 2):
        status, data = hci.command(
            OP_LE_READ_ALL_LOCAL_SUPPORTED_FEATURES, allow_fail=True
        )
        if status == 0:
            capabilities["all_local_features"] = data

    if command_supported(commands, 48, 7):
        status, data = hci.command(
            OP_LE_READ_MIN_SUPPORTED_CONN_INTERVAL, allow_fail=True
        )
        if status == 0:
            capabilities["minimum_connection_interval"] = data

    # CIS Peripheral support makes these timeout commands conditionally
    # mandatory. If both are advertised, exercise them without changing the
    # controller configuration: read the current value and write it back.
    if (command_supported(commands, 7, 2)
            and command_supported(commands, 7, 3)):
        status, data = hci.command(OP_READ_CONN_ACCEPT_TIMEOUT, allow_fail=True)
        if status == 0 and len(data) == 2:
            write_status, write_data = hci.command(
                OP_WRITE_CONN_ACCEPT_TIMEOUT, data, allow_fail=True
            )
            if write_status == 0 and not write_data:
                capabilities["connection_accept_timeout"] = data

    return capabilities
