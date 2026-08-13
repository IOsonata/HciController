#!/usr/bin/env python3
"""HciController hardware-test CLI with probe sequencing guards.

The complete hardware-test implementation lives in hci_ble_test_impl.py.  This
module keeps the public CLI/import surface unchanged while correcting probe-only
ordering that depends on asynchronous controller state:

* LE Create Connection Cancel is not considered finished until its terminal LE
  connection-complete event has arrived.  A following initiator must not start
  in the gap between Command Complete and that terminal event.
* the optional phone pairing wait runs after the ordinary connection-scoped
  command rows, immediately before Disconnect, so a phone that leaves during
  the interactive wait cannot invalidate the commands the run meant to test;
* once the current connection has gone down, later connection-scoped rows are
  skipped instead of being sent at a dead handle and reported as unrelated
  Unknown Connection Identifier failures;
* a broad probe that reaches an advertised command and gets a status not listed
  for that row is a failed validation run, not a successful diagnostic run.
"""

import builtins
import re
import struct
import sys
import time

import hci_ble_test_impl as _impl
from hci_ble_test_impl import *


_original_cmd_probe = _impl.cmd_probe
_original_probe_available = _impl.probe_available
_original_probe_wait_for_ltk = _impl.probe_wait_for_ltk
_original_probe_wait_for_peer = _impl.probe_wait_for_peer

_active_probe_hci = None
_deferred_pairing = None
_cancel_pending = False
_PROBE_REFUSAL_RE = re.compile(r"\b(\d+)\s+refused otherwise\b")


def _link_down_for(hci, handle):
    """True only for a successful Disconnection Complete for this handle."""
    down = hci.link_down
    return down is not None and down[0] == 0 and down[1] == handle


def _restore_packets(hci, packets):
    """Put deferred packets back ahead of anything that arrived after them."""
    if packets:
        hci.pending = packets + hci.pending


def _wait_for_cancel_terminal(hci, timeout=2.0):
    """Consume the connection-complete event that terminates a cancelled initiator."""
    deferred = []
    deadline = time.time() + timeout

    while time.time() < deadline:
        if hci.pending:
            packet = hci.pending.pop(0)
        else:
            packet = hci.read_wire(min(0.2, max(0.0, deadline - time.time())))
        if packet is None:
            continue

        kind, code, body = packet
        if kind == _impl.H4_EVENT and code == _impl.EVT_LE_META:
            info = _impl.parse_connection(body)
            if info is not None:
                _restore_packets(hci, deferred)
                return info
        deferred.append(packet)

    _restore_packets(hci, deferred)
    return None


def _poll_ready_events(hci):
    """Process already-buffered wire events without adding a blocking poll."""
    try:
        ready = hci.ser.in_waiting
    except (AttributeError, OSError):
        return

    while ready:
        packet = hci.read_wire(0.1)
        if packet is None:
            break
        hci.pending.append(packet)
        try:
            ready = hci.ser.in_waiting
        except (AttributeError, OSError):
            break


def probe_wait_for_peer(hci, ctx, args):
    """Start a fresh link state record when the probe obtains a phone connection."""
    handle = _original_probe_wait_for_peer(hci, ctx, args)
    if handle is not None:
        hci.link_down = None
        hci.ltk_request = None
        hci.encryption = None
    return handle


def probe_wait_for_ltk(hci, handlers, smp, args):
    """Defer the interactive pairing window until the ordinary link rows finish."""
    global _deferred_pairing
    _deferred_pairing = (hci, handlers, smp, args)


def probe_available(command, args, live_handle, ctx):
    """Add asynchronous probe state to the implementation's static prerequisites."""
    global _cancel_pending, _deferred_pairing

    hci = _active_probe_hci

    if _cancel_pending and hci is not None:
        if _wait_for_cancel_terminal(hci, 3.0) is None:
            return False, "previous initiator cancellation has not reached its terminal event"
        _cancel_pending = False

    needs_connection = _impl.hci_commands.NEEDS_CONN in command.needs
    if needs_connection and hci is not None:
        _poll_ready_events(hci)
        if _link_down_for(hci, ctx.handle):
            _, _, reason = hci.link_down
            return False, (
                "handle 0x%04X disconnected before this row, reason 0x%02X %s"
                % (ctx.handle, reason, _impl.ERROR_NAMES.get(reason, ""))
            )

    # cmd_probe sorts Disconnect last.  The original implementation waited for
    # optional pairing before any connection-scoped row, which let an
    # interactive timeout or phone-side disconnect invalidate the whole group.
    # Run that wait here instead, after all ordinary rows have had their chance.
    if command.opcode == _impl.OP_DISCONNECT and _deferred_pairing is not None:
        pair_hci, handlers, smp, pair_args = _deferred_pairing
        _deferred_pairing = None
        if not _link_down_for(pair_hci, ctx.handle):
            _original_probe_wait_for_ltk(pair_hci, handlers, smp, pair_args)
        _poll_ready_events(pair_hci)
        if _link_down_for(pair_hci, ctx.handle):
            _, _, reason = pair_hci.link_down
            return False, (
                "handle 0x%04X disconnected during the pairing wait, reason 0x%02X %s"
                % (ctx.handle, reason, _impl.ERROR_NAMES.get(reason, ""))
            )

    return _original_probe_available(command, args, live_handle, ctx)


def cmd_probe(hci, args):
    """Run the broad probe with asynchronous sequencing and strict results."""
    global _active_probe_hci, _deferred_pairing, _cancel_pending

    _active_probe_hci = hci
    _deferred_pairing = None
    _cancel_pending = False
    raw_command = hci.command

    # The implementation keeps its counters local to cmd_probe.  Its summary is
    # therefore the one stable boundary available to this sequencing wrapper.
    # Forward every print unchanged while remembering the explicit
    # "refused otherwise" count. Those are statuses not listed in the command
    # row's expect= set, so any nonzero value is a validation failure.
    had_module_print = "print" in _impl.__dict__
    previous_print = _impl.__dict__.get("print")
    refused_otherwise = None

    def probe_print(*values, **kwargs):
        nonlocal refused_otherwise
        text = " ".join(str(value) for value in values)
        match = _PROBE_REFUSAL_RE.search(text)
        if match is not None:
            refused_otherwise = int(match.group(1))
        builtins.print(*values, **kwargs)

    def guarded_command(opcode, payload=b"", timeout=3.0, allow_fail=False):
        global _cancel_pending

        # Cleanup after a phone-side disconnect is deliberately quiet.  The
        # counted Disconnect row is already skipped by probe_available; the
        # unconditional final cleanup in the implementation need not put one
        # more command on a handle that no longer exists.
        if opcode == _impl.OP_DISCONNECT and len(payload) >= 2:
            handle = struct.unpack("<H", payload[:2])[0]
            if _link_down_for(hci, handle):
                if allow_fail:
                    return 0x02, b""
                raise _impl.HciError("connection 0x%04X is already down" % handle)

        result = raw_command(opcode, payload, timeout=timeout,
                             allow_fail=allow_fail)

        # Command Complete for LE Create Connection Cancel only says that the
        # cancel command was accepted.  The initiating procedure ends on the
        # later LE connection-complete event.  Starting another initiator
        # before that event is exactly what produced the false 0x0C on 0x2085.
        if opcode == _impl.OP_LE_CREATE_CONNECTION_CANCEL and result[0] == 0:
            _cancel_pending = True
            terminal = _wait_for_cancel_terminal(hci, 2.0)
            if terminal is not None:
                _cancel_pending = False

                # A successful connection means the cancel lost the race.  It
                # is not the case the probe asks for, but do not leave that
                # accidental connection around to contaminate later rows.
                status, handle = terminal[0], terminal[1]
                if status == 0:
                    raw_command(_impl.OP_DISCONNECT,
                                struct.pack("<HB", handle, 0x13),
                                timeout=1.0, allow_fail=True)
        return result

    hci.command = guarded_command
    _impl.print = probe_print
    try:
        result = _original_cmd_probe(hci, args)
        if result == 0 and refused_otherwise not in (None, 0):
            return 1
        return result
    finally:
        hci.command = raw_command
        if had_module_print:
            _impl.print = previous_print
        else:
            del _impl.print
        _active_probe_hci = None
        _deferred_pairing = None
        _cancel_pending = False


# The implementation's functions resolve these names through its module
# globals, so installing the guards here fixes both direct CLI use and callers
# that import cmd_probe from hci_ble_test.
_impl.probe_available = probe_available
_impl.probe_wait_for_peer = probe_wait_for_peer
_impl.probe_wait_for_ltk = probe_wait_for_ltk
_impl.cmd_probe = cmd_probe


def main():
    return _impl.main()


if __name__ == "__main__":
    sys.exit(main())
