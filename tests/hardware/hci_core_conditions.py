#!/usr/bin/env python3
"""Shared Bluetooth Core conditional HCI checks used by release profiles.

The nRF52840 release currently reports Core 6.2, but the Bluetooth 5.4 feature
bits it advertises still activate the same conditional command requirements.
Keep those inherited checks here so an old Core-5.4 product profile cannot be
mistaken for the current release test.
"""

EXPECTED_LE_SUPPORTED_STATES = bytes.fromhex("ff ff ff ff ff 03 00 00")

# LE FeatureSet bit numbers, Core 5.4 Vol 6 Part B Table 4.7.
FEAT_CONN_PARAM_REQ = 1
FEAT_SCA_UPDATES = 26
FEAT_CIS_CENTRAL = 28
FEAT_CIS_PERIPHERAL = 29
FEAT_ISO_BROADCASTER = 30
FEAT_SYNC_RECEIVER = 31
FEAT_POWER_CONTROL_1 = 33
FEAT_POWER_CONTROL_2 = 34
FEAT_PATH_LOSS = 35
FEAT_CONN_SUBRATING = 37
FEAT_CHANNEL_CLASSIFICATION = 39
FEAT_ADV_CODING_SELECTION = 40
FEAT_PAWR_ADVERTISER = 43
FEAT_PAWR_SCANNER = 44


def command_bit(commands, octet, bit):
    return bool(commands[octet] & (1 << bit))


def feature(features, bit):
    return bool(features[bit // 8] & (1 << (bit % 8)))


def require_command_bit(commands, octet, bit, opcode, name, errors, condition=None):
    if command_bit(commands, octet, bit):
        return
    prefix = (condition + ": ") if condition else ""
    errors.append(
        "%s%s (0x%04X) missing from Supported Commands octet %d bit %d"
        % (prefix, name, opcode, octet, bit)
    )


def require_group(commands, condition, rows, errors):
    for octet, bit, opcode, name in rows:
        require_command_bit(commands, octet, bit, opcode, name, errors, condition)


def validate_conditional_commands(commands, features, errors):
    """Evaluate inherited Core 5.4 conditions driven directly by LE features."""

    conn_param = feature(features, FEAT_CONN_PARAM_REQ)
    sca = feature(features, FEAT_SCA_UPDATES)
    cis_central = feature(features, FEAT_CIS_CENTRAL)
    cis_peripheral = feature(features, FEAT_CIS_PERIPHERAL)
    iso_broadcaster = feature(features, FEAT_ISO_BROADCASTER)
    sync_receiver = feature(features, FEAT_SYNC_RECEIVER)
    power1 = feature(features, FEAT_POWER_CONTROL_1)
    power2 = feature(features, FEAT_POWER_CONTROL_2)
    path_loss = feature(features, FEAT_PATH_LOSS)
    subrating = feature(features, FEAT_CONN_SUBRATING)
    channel_class = feature(features, FEAT_CHANNEL_CLASSIFICATION)
    adv_coding = feature(features, FEAT_ADV_CODING_SELECTION)
    pawr_adv = feature(features, FEAT_PAWR_ADVERTISER)
    pawr_scan = feature(features, FEAT_PAWR_SCANNER)

    if power1 != power2:
        errors.append("LE Power Control Request feature bits 33 and 34 disagree")
    power_control = power1 and power2

    if conn_param:
        require_group(commands, "C.6", (
            (33, 4, 0x2020, "LE Remote Connection Parameter Request Reply"),
            (33, 5, 0x2021, "LE Remote Connection Parameter Request Negative Reply"),
        ), errors)

    if cis_central:
        require_group(commands, "C.39 CIS Central", (
            (41, 7, 0x2062, "LE Set CIG Parameters"),
            (42, 0, 0x2063, "LE Set CIG Parameters Test"),
            (42, 1, 0x2064, "LE Create CIS"),
            (42, 2, 0x2065, "LE Remove CIG"),
        ), errors)

    if cis_peripheral:
        require_group(commands, "C.40 CIS Peripheral", (
            (42, 3, 0x2066, "LE Accept CIS Request"),
            (42, 4, 0x2067, "LE Reject CIS Request"),
        ), errors)

    if iso_broadcaster:
        require_group(commands, "C.41 Isochronous Broadcaster", (
            (42, 5, 0x2068, "LE Create BIG"),
            (42, 6, 0x2069, "LE Create BIG Test"),
            (42, 7, 0x206A, "LE Terminate BIG"),
        ), errors)

    if sync_receiver:
        require_group(commands, "C.42 Synchronized Receiver", (
            (43, 0, 0x206B, "LE BIG Create Sync"),
            (43, 1, 0x206C, "LE BIG Terminate Sync"),
        ), errors)

    if sca and (cis_central or cis_peripheral):
        require_group(commands, "C.44 SCA Updates with CIS", (
            (43, 2, 0x206D, "LE Request Peer SCA"),
        ), errors)

    tx_iso = cis_central or cis_peripheral or iso_broadcaster
    rx_iso = cis_central or cis_peripheral or sync_receiver
    any_iso = tx_iso or sync_receiver

    if tx_iso:
        require_group(commands, "C.45 ISO transmit", (
            (41, 6, 0x2061, "LE Read ISO TX Sync"),
            (43, 5, 0x2070, "LE ISO Transmit Test"),
        ), errors)

    if rx_iso:
        require_group(commands, "C.46 ISO receive", (
            (43, 6, 0x2071, "LE ISO Receive Test"),
            (43, 7, 0x2072, "LE ISO Read Test Counters"),
        ), errors)

    if any_iso:
        require_group(commands, "C.47 ISO data path/test", (
            (43, 3, 0x206E, "LE Setup ISO Data Path"),
            (43, 4, 0x206F, "LE Remove ISO Data Path"),
            (44, 0, 0x2073, "LE ISO Test End"),
        ), errors)

    if cis_central or cis_peripheral or subrating:
        require_group(commands, "C.49 Host-set feature bits", (
            (44, 1, 0x2074, "LE Set Host Feature"),
        ), errors)

    if tx_iso:
        require_group(commands, "C.55 ISO HCI buffers", (
            (41, 5, 0x2060, "LE Read Buffer Size v2"),
        ), errors)

    if power_control:
        require_group(commands, "C.51 LE Power Control Request", (
            (44, 3, 0x2076, "LE Enhanced Read Transmit Power Level"),
            (44, 4, 0x2077, "LE Read Remote Transmit Power Level"),
            (44, 7, 0x207A, "LE Set Transmit Power Reporting Enable"),
        ), errors)

    if path_loss:
        require_group(commands, "C.52 LE Path Loss Monitoring", (
            (44, 5, 0x2078, "LE Set Path Loss Reporting Parameters"),
            (44, 6, 0x2079, "LE Set Path Loss Reporting Enable"),
        ), errors)

    if subrating:
        require_group(commands, "C.57 Connection Subrating", (
            (46, 0, 0x207D, "LE Set Default Subrate"),
            (46, 1, 0x207E, "LE Subrate Request"),
        ), errors)

    if channel_class:
        print("C.58 active: Channel Classification advertised")

    if adv_coding:
        require_group(commands, "C.66 Advertising Coding Selection", (
            (46, 2, 0x207F, "LE Set Extended Advertising Parameters v2"),
        ), errors)

    if pawr_adv:
        require_group(commands, "C.67 PAwR Advertiser", (
            (46, 5, 0x2082, "LE Set Periodic Advertising Subevent Data"),
            (47, 0, 0x2085, "LE Extended Create Connection v2"),
            (47, 1, 0x2086, "LE Set Periodic Advertising Parameters v2"),
        ), errors)

    if pawr_scan:
        require_group(commands, "C.68 PAwR Scanner", (
            (46, 6, 0x2083, "LE Set Periodic Advertising Response Data"),
            (46, 7, 0x2084, "LE Set Periodic Sync Subevent"),
        ), errors)

    if pawr_adv or pawr_scan:
        print("C.69 active: Enhanced Connection Complete v2 event required")

    active = []
    for label, enabled in (
        ("C.6", conn_param),
        ("C.39", cis_central),
        ("C.40", cis_peripheral),
        ("C.41", iso_broadcaster),
        ("C.42", sync_receiver),
        ("C.44", sca and (cis_central or cis_peripheral)),
        ("C.45", tx_iso),
        ("C.46", rx_iso),
        ("C.47", any_iso),
        ("C.49", cis_central or cis_peripheral or subrating),
        ("C.51", power_control),
        ("C.52", path_loss),
        ("C.55", tx_iso),
        ("C.57", subrating),
        ("C.58", channel_class),
        ("C.66", adv_coding),
        ("C.67", pawr_adv),
        ("C.68", pawr_scan),
        ("C.69", pawr_adv or pawr_scan),
    ):
        if enabled:
            active.append(label)
    print("Active inherited Core conditions:", ", ".join(active))
