#!/usr/bin/env python3
"""
Which SDC commands a SoftDevice Controller library actually defines.

The nrfxlib headers declare the whole HCI API, but a library variant only
contains the commands it was built with, so a declaration is no guarantee that
the symbol links. Several rows in the dispatch table therefore sit behind a
macro. This reads the archive and says what each of those macros should be.

    python3 tests/sdc_symbols.py
    python3 tests/sdc_symbols.py /path/to/libsoftdevice_controller_multirole.a

It parses the archive symbol index directly, so it needs no toolchain. The
alternative, arm-none-eabi-nm, is only on the machine that has the cross
compiler installed, and this question comes up before the first build as often
as after it.

Exits non-zero when a macro default in the source disagrees with the library,
so it can be run as a check.
"""

import os
import re
import struct
import sys

# Macro in src/hci_sdc_nrfxlib.cpp, and the symbols it needs. A macro with an
# empty list needs no SDC symbol at all.
GATES = [
    ("HCI_SDC_HAS_READ_SUPPORTED_STATES",
     ["sdc_hci_cmd_le_read_supported_states"]),
    ("HCI_SDC_HAS_READ_TRANSMIT_POWER",
     ["sdc_hci_cmd_le_read_transmit_power"]),
    ("HCI_SDC_HAS_READ_REMOTE_VERSION",
     ["sdc_hci_cmd_lc_read_remote_version_information"]),
    ("HCI_SDC_HAS_AUTH_PAYLOAD_TIMEOUT",
     ["sdc_hci_cmd_cb_read_authenticated_payload_timeout",
      "sdc_hci_cmd_cb_write_authenticated_payload_timeout"]),
    ("HCI_SDC_HAS_VS_READ_STATIC_ADDRESSES",
     ["sdc_hci_cmd_vs_zephyr_read_static_addresses"]),
    ("HCI_SDC_HAS_VS_READ_COUNTERS", []),
    ("HCI_SDC_HAS_VS_CARRIER_TEST",
     ["sdc_hci_cmd_vs_transmitter_carrier_test"]),
    ("HCI_SDC_HAS_VS_ZEPHYR_SET",
     ["sdc_hci_cmd_vs_zephyr_read_version_info",
      "sdc_hci_cmd_vs_zephyr_read_supported_commands",
      "sdc_hci_cmd_vs_zephyr_write_bd_addr",
      "sdc_hci_cmd_vs_zephyr_read_chip_temp",
      "sdc_hci_cmd_vs_zephyr_write_tx_power",
      "sdc_hci_cmd_vs_zephyr_read_tx_power"]),
    ("HCI_SDC_HAS_VS_KEY_HIERARCHY_ROOTS",
     ["sdc_hci_cmd_vs_zephyr_read_key_hierarchy_roots"]),
    ("HCI_SDC_HAS_VS_QOS",
     ["sdc_hci_cmd_vs_qos_conn_event_report_enable",
      "sdc_hci_cmd_vs_qos_channel_survey_enable",
      "sdc_hci_cmd_vs_read_average_rssi",
      "sdc_hci_cmd_vs_get_next_conn_event_counter",
      "sdc_hci_cmd_vs_conn_anchor_point_update_event_report_enable"]),
    ("HCI_SDC_HAS_LE_POWER_CONTROL",
     ["sdc_hci_cmd_cb_read_transmit_power_level",
      "sdc_hci_cmd_le_read_rf_path_compensation",
      "sdc_hci_cmd_le_write_rf_path_compensation",
      "sdc_hci_cmd_le_enhanced_read_transmit_power_level",
      "sdc_hci_cmd_le_read_remote_transmit_power_level",
      "sdc_hci_cmd_le_set_path_loss_reporting_params",
      "sdc_hci_cmd_le_set_path_loss_reporting_enable",
      "sdc_hci_cmd_le_set_transmit_power_reporting_enable"]),
    ("HCI_SDC_HAS_LE_REQUEST_PEER_SCA",
     ["sdc_hci_cmd_le_request_peer_sca"]),
    ("HCI_SDC_HAS_LE_SUBRATING",
     ["sdc_hci_cmd_le_set_default_subrate",
      "sdc_hci_cmd_le_subrate_request"]),
    ("HCI_SDC_HAS_LE_READ_ALL_REMOTE_FEATURES",
     ["sdc_hci_cmd_le_read_all_remote_features"]),
    ("HCI_SDC_HAS_LE_SET_HOST_CHANNEL_CLASSIFICATION",
     ["sdc_hci_cmd_le_set_host_channel_classification"]),
    ("HCI_SDC_HAS_VS_SET_ADV_RANDOMNESS",
     ["sdc_hci_cmd_vs_set_adv_randomness"]),
    ("HCI_SDC_HAS_VS_LLPM",
     ["sdc_hci_cmd_vs_llpm_mode_set",
      "sdc_hci_cmd_vs_conn_update"]),
    ("HCI_SDC_HAS_LE_PERIODIC_ADV",
     ["sdc_hci_cmd_le_set_periodic_adv_params",
      "sdc_hci_cmd_le_set_periodic_adv_data",
      "sdc_hci_cmd_le_set_periodic_adv_enable"]),
    ("HCI_SDC_HAS_LE_PERIODIC_SYNC",
     ["sdc_hci_cmd_le_periodic_adv_create_sync",
      "sdc_hci_cmd_le_periodic_adv_create_sync_cancel",
      "sdc_hci_cmd_le_periodic_adv_terminate_sync",
      "sdc_hci_cmd_le_add_device_to_periodic_adv_list",
      "sdc_hci_cmd_le_remove_device_from_periodic_adv_list",
      "sdc_hci_cmd_le_clear_periodic_adv_list",
      "sdc_hci_cmd_le_read_periodic_adv_list_size"]),
    ("HCI_SDC_HAS_LE_PERIODIC_SYNC_TRANSFER",
     ["sdc_hci_cmd_le_set_periodic_adv_receive_enable",
      "sdc_hci_cmd_le_periodic_adv_sync_transfer",
      "sdc_hci_cmd_le_periodic_adv_set_info_transfer",
      "sdc_hci_cmd_le_set_periodic_adv_sync_transfer_params",
      "sdc_hci_cmd_le_set_default_periodic_adv_sync_transfer_params"]),
    ("HCI_SDC_HAS_LE_PERIODIC_ADV_RSP",
     ["sdc_hci_cmd_le_set_periodic_adv_params_v2",
      "sdc_hci_cmd_le_set_periodic_adv_subevent_data"]),
    ("HCI_SDC_HAS_LE_PERIODIC_SYNC_RSP",
     ["sdc_hci_cmd_le_set_periodic_adv_response_data",
      "sdc_hci_cmd_le_set_periodic_sync_subevent"]),
]

# Not a dispatch table gate. HCI_NRF52840_QOS_CHANNEL_SURVEY in
# include/hci_nrf52840.h decides whether the module is configured in, and it
# needs a support function rather than a command. Checked here because a build
# that sets it without the symbol does not link.
SUPPORT = [
    ("HCI_NRF52840_QOS_CHANNEL_SURVEY", ["sdc_support_qos_channel_survey"]),
    ("HCI_NRF52840_LE_POWER_CONTROL",
     ["sdc_support_le_power_control_central",
      "sdc_support_le_power_control_peripheral",
      "sdc_support_le_path_loss_monitoring"]),
    ("HCI_NRF52840_SCA_UPDATE",
     ["sdc_support_sca_central", "sdc_support_sca_peripheral"]),
    ("HCI_NRF52840_CONNECTION_SUBRATING",
     ["sdc_support_connection_subrating_central",
      "sdc_support_connection_subrating_peripheral"]),
    # The unsuffixed sdc_support_extended_feature_set() is deprecated and is
    # absent from the nRF52 libraries. Only the per role pair is checked.
    ("HCI_NRF52840_EXTENDED_FEATURE_SET",
     ["sdc_support_extended_feature_set_central",
      "sdc_support_extended_feature_set_peripheral"]),
    ("HCI_NRF52840_PARALLEL_SCAN_INIT",
     ["sdc_support_parallel_scanning_and_initiating"]),
    ("HCI_NRF52840_PERIODIC_ADV", ["sdc_support_le_periodic_adv"]),
    ("HCI_NRF52840_PERIODIC_SYNC", ["sdc_support_le_periodic_sync"]),
    ("HCI_NRF52840_PERIODIC_SYNC_TRANSFER",
     ["sdc_support_periodic_adv_sync_transfer_sender_central",
      "sdc_support_periodic_adv_sync_transfer_sender_peripheral",
      "sdc_support_periodic_adv_sync_transfer_receiver_central",
      "sdc_support_periodic_adv_sync_transfer_receiver_peripheral"]),
    ("HCI_NRF52840_PERIODIC_ADV_RSP",
     ["sdc_support_le_periodic_adv_with_rsp"]),
    ("HCI_NRF52840_PERIODIC_SYNC_RSP",
     ["sdc_support_le_periodic_sync_with_rsp"]),
]

DEFAULT_LIB = ("../external/sdk-nrfxlib/softdevice_controller/lib/nrf52/"
               "hard-float/libsoftdevice_controller_multirole.a")


def archive_symbols(path):
    """
    Names in an ar archive's symbol index, which is every symbol its members
    define. GNU format: a first member called "/" holding a big endian count,
    that many offsets, then the names as NUL terminated strings.
    """
    with open(path, "rb") as handle:
        data = handle.read()

    if data[:8] != b"!<arch>\n":
        raise ValueError("%s is not an ar archive" % path)

    name = data[8:24].decode("ascii", "replace").strip()
    if name.startswith("__.SYMDEF"):
        raise ValueError("%s is a BSD archive, which this does not read. Use "
                         "nm on it instead." % path)
    if name != "/":
        raise ValueError("%s has no symbol index, so it was built without one "
                         "and nothing can be said about it" % path)

    size = int(data[56:66].decode("ascii").strip())
    body = data[68:68 + size]
    count = struct.unpack(">I", body[:4])[0]
    names = body[4 + count * 4:].split(b"\x00")
    return set(n.decode("ascii", "replace") for n in names if n)


def source_defaults(paths, macros):
    """The value each named macro defaults to, across the given sources."""
    text = ""
    for path in paths:
        try:
            with open(path) as handle:
                text += handle.read()
        except IOError:
            pass

    found = {}
    for macro in macros:
        match = re.search(r"^#define\s+%s\s+(\d+)\s*$" % macro, text,
                          re.MULTILINE)
        if match:
            found[macro] = int(match.group(1))
    return found


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)

    if len(sys.argv) > 2:
        print(__doc__.strip())
        return 2

    lib = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, DEFAULT_LIB)
    lib = os.path.normpath(lib)

    if not os.path.exists(lib):
        print("No library at %s" % lib)
        print("Pass the path to a libsoftdevice_controller_*.a.")
        return 2

    try:
        symbols = archive_symbols(lib)
    except ValueError as error:
        print(error)
        return 2

    commands = sorted(s for s in symbols if s.startswith("sdc_hci_cmd_"))
    print("%s" % lib)
    print("%d symbols, %d of them HCI commands" % (len(symbols), len(commands)))
    print()

    sources = [os.path.join(root, "src", "hci_sdc_nrfxlib.cpp"),
               os.path.join(root, "include", "hci_nrf52840.h")]
    defaults = source_defaults(sources,
                               [m for m, _ in GATES] +
                               [m for m, _ in SUPPORT])
    disagreed = 0

    for macro, needed in GATES + SUPPORT:
        if not needed:
            want = 1
            note = "needs no SDC symbol"
        else:
            absent = [s for s in needed if s not in symbols]
            want = 0 if absent else 1
            note = ("missing %s" % ", ".join(absent) if absent
                    else "all present")

        current = defaults.get(macro)
        if current is None:
            state = "not found in the source"
        elif current == want:
            state = "matches"
        else:
            state = "SOURCE SAYS %d" % current
            disagreed += 1

        print("  %-38s should be %d  %-34s %s"
              % (macro, want, note, state))

    print()
    if disagreed:
        print("%d macro default disagrees with this library. A wrong 1 is a "
              "link error; a wrong 0 is a command answered Unknown HCI Command "
              "that the controller could have run." % disagreed)
        return 1

    print("Every gate matches this library.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
