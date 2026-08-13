"""Catalog-row driving for the two-controller connection sweep."""

import hci_commands
from hci_ble_test import HciError
from hci_connection_pair_async import (
    OP_VS_REMOTE_TX_POWER, status_text, terminal_status, verify_fd0a,
)

DEDICATED = {
    0x2019: "covered by the pairing hardware test",
    0x2064: "covered by hci_cis_pair_test.py",
}


class Ctx:
    def __init__(self, handle, addr_type, role):
        self.handle = handle
        self.addr_type = addr_type
        self.role = role


def _undo_now(hci, row, ctx):
    if not row.undo_now or row.undo is None:
        return
    opcode, payload = row.undo
    payload = payload(ctx) if callable(payload) else payload
    status, _ = hci.command(opcode, payload, allow_fail=True)
    if status != 0:
        raise HciError("undo for 0x%04X returned %s"
                       % (row.opcode, status_text(status)))


def _run_row(label, hci, row, ctx, counts):
    status, data = hci.command(row.opcode, row.build(ctx), allow_fail=True)
    if status == 0:
        status = terminal_status(hci, row.opcode, ctx.handle)

    if status == 0:
        if row.opcode == OP_VS_REMOTE_TX_POWER:
            verify_fd0a(hci, ctx.handle)
        _undo_now(hci, row, ctx)
        counts["accepted"] += 1
        result = "%d byte return" % len(data) if data else "accepted"
        print("[ok] %-10s 0x%04X %-50s %s"
              % (label, row.opcode, row.name, result))
        return

    if status in row.expect:
        counts["expected"] += 1
        print("[ok] %-10s 0x%04X %-50s %s, expected"
              % (label, row.opcode, row.name, status_text(status)))
        return

    counts["failed"] += 1
    print("[!!] %-10s 0x%04X %-50s %s"
          % (label, row.opcode, row.name, status_text(status)))


def run_role(label, role, hci, handle, addr_type, selected, counts):
    ctx = Ctx(handle, addr_type, role)
    for row in hci_commands.COMMANDS:
        if row.opcode not in selected:
            continue
        if hci_commands.NEEDS_CONN not in row.needs:
            continue
        if hci_commands.NEEDS_CENTRAL in row.needs and role != 0:
            counts["skipped"] += 1
            print("[--] %-10s 0x%04X %-50s central only"
                  % (label, row.opcode, row.name))
            continue
        if row.opcode in DEDICATED:
            counts["dedicated"] += 1
            print("[--] %-10s 0x%04X %-50s %s"
                  % (label, row.opcode, row.name, DEDICATED[row.opcode]))
            continue
        if hci_commands.NEEDS_SYNC in row.needs:
            counts["skipped"] += 1
            print("[--] %-10s 0x%04X %-50s needs periodic sync"
                  % (label, row.opcode, row.name))
            continue
        _run_row(label, hci, row, ctx, counts)
