#!/usr/bin/env python3
"""HciController hardware-test CLI with probe sequencing guards.

The complete hardware-test implementation lives in hci_ble_test_impl.py.  This
module keeps the public CLI/import surface unchanged while correcting probe-only
ordering that depends on asynchronous controller state and adding host transport
selection for serial H:4 and native Bluetooth USB HCI.

* LE Create Connection Cancel is not considered finished until its terminal LE
  connection-complete event has arrived.  A following initiator must not start
  in the gap between Command Complete and that terminal event.
* the optional phone pairing wait runs after the ordinary connection-scoped
  command rows, immediately before Disconnect, so a phone that leaves during
  the interactive wait cannot invalidate the commands the run meant to test;
* once the current connection has gone down, later connection-scoped rows are
  skipped instead of being sent at a dead handle and reported as unrelated
  Unknown Connection Identifier failures;
* --only narrows the connection-scoped rows too, so it really isolates a
  command after the connection preamble rather than replaying every link row;
* LE Set PHY is not complete at Command Status.  The probe waits for its LE PHY
  Update Complete event before another link-layer control procedure is started;
* LE Read Remote Transmit Power Level is not complete at Command Status.  The
  probe keeps LE Transmit Power Reporting unmasked and waits for that terminal
  event before another power-control procedure is allowed to start;
* VS Write Remote TX Power treats Controller Busy as transient while an
  earlier power-control procedure finishes. It retries for a bounded interval
  and still reports 0x3A as a failure if the controller remains busy;
* a broad probe that reaches an advertised command and gets a status not listed
  for that row is a failed validation run, not a successful diagnostic run;
* advertise retains the completed connection's ATT statistics after a remote
  disconnect instead of erasing the evidence before printing its verdict.
"""

import argparse
import builtins
import re
import struct
import sys
import time

import hci_transport as _transport
import hci_ble_test_impl as _impl
from hci_ble_test_impl import *


_original_cmd_probe = _impl.cmd_probe
_original_cmd_advertise = _impl.cmd_advertise
_original_att_server = _impl.AttServer
_original_probe_available = _impl.probe_available
_original_probe_wait_for_ltk = _impl.probe_wait_for_ltk
_original_probe_wait_for_peer = _impl.probe_wait_for_peer

_active_probe_hci = None
_deferred_pairing = None
_cancel_pending = False
_last_advertise_att = None
_PROBE_REFUSAL_RE = re.compile(r"\b(\d+)\s+refused otherwise\b")
_ADVERTISE_ACL_RE = re.compile(
    r"^ACL received (\d+), ATT requests \d+, responses \d+, notifications \d+$")

_STATUS_CONTROLLER_BUSY = 0x3A
_OP_LE_SET_PHY = 0x2032
_OP_LE_READ_REMOTE_TX_POWER = 0x2077
_OP_VS_WRITE_REMOTE_TX_POWER = 0xFD0A
_LE_PHY_UPDATE_COMPLETE = 0x0C
_LE_TRANSMIT_POWER_REPORTING = 0x21
_LE_EVENT_MASK_PHY_UPDATE = 1 << 11
_LE_EVENT_MASK_TRANSMIT_POWER_REPORTING = 1 << 32
_POWER_REPORT_REASON_READ_REMOTE = 0x02
_PHY_UPDATE_TIMEOUT = 5.0
_REMOTE_TX_POWER_TIMEOUT = 5.0
_FD0A_BUSY_RETRIES = 10
_FD0A_BUSY_DELAY = 0.1

_transport_kind = "auto"
_usb_selector = None
_usb_bulk = False
_transport_error = None


class Hci(_impl.Hci):
    """Existing HCI test engine with a replaceable wire transport."""

    def __init__(self, port, raw=False):
        if isinstance(port, _transport.TransportSpec):
            wire = port.open()
        else:
            wire = _transport.SerialH4Transport(port)

        self.raw = raw
        self.transport = wire
        self.ser = getattr(wire, "ser", None)
        self.pending = []
        self.ltk_request = None
        self.encryption = None
        self.acl_credits = 0
        self.acl_advertised = 0
        self.acl_completed = 0
        self.link_down = None
        self.acl_size = 27
        self.unsupported = []

    def close(self):
        self.transport.close()

    def write_packet(self, data):
        if self.raw:
            print("   tx", data.hex(" "))
        try:
            self.transport.write_packet(data)
        except _transport.TransportGone as err:
            raise _impl.HciGone(str(err))
        except _transport.TransportError as err:
            raise _impl.HciError(str(err))

    def read_exact(self, count, deadline):
        try:
            reader = getattr(self.transport, "read_exact", None)
            if reader is None:
                raise _transport.TransportError(
                    "read_exact is only valid on a byte-stream transport")
            return reader(count, deadline)
        except _transport.TransportGone as err:
            raise _impl.HciGone(str(err))
        except _transport.TransportError as err:
            raise _impl.HciError(str(err))

    def read_wire(self, timeout=1.0):
        try:
            packet = self.transport.read_packet(timeout)
        except _transport.TransportGone as err:
            raise _impl.HciGone(str(err))
        except _transport.TransportError as err:
            raise _impl.HciError(str(err))

        if packet is None:
            return None

        kind, code, body = packet
        if kind == _impl.H4_EVENT:
            if self.raw:
                print("   rx evt %02x" % code, body.hex(" "))
            self.on_event(code, body)
        elif kind == _impl.H4_ACL:
            if self.raw:
                print("   rx acl", body[:4].hex(" "), body[4:].hex(" "))
        elif kind == _impl.H4_ISO:
            if self.raw:
                print("   rx iso", body[:4].hex(" "), body[4:].hex(" "))
        return packet

    def has_pending_input(self):
        try:
            return self.transport.has_pending_input()
        except _transport.TransportGone as err:
            raise _impl.HciGone(str(err))
        except _transport.TransportError as err:
            raise _impl.HciError(str(err))


class AttServer(_original_att_server):
    """Remember the last connection so a remote disconnect cannot erase it."""

    def __init__(self, *args, **kwargs):
        global _last_advertise_att
        super().__init__(*args, **kwargs)
        _last_advertise_att = self


_impl.Hci = Hci
_impl.AttServer = AttServer

# Counter block v5 appended four PAwR checkpoints after the two SDC pool
# figures. Version 6 appends the Controller-to-Host ACL fetch/accept checkpoints
# at 38 and 39. Version 7 appends the nRF52840 legacy USB Event-IN checkpoints
# at 40 through 48. Version 8 appends the packed EP1 acknowledgement trace at
# 49 through 61. Those words are snapshots, not monotonic counters, so they are
# decoded separately and never included in delta calculations.
COUNTER_VERSION = 8
_impl.COUNTER_VERSION = COUNTER_VERSION
_COUNTER_EXTRA_NAMES = (
    (34, "PAwR delayed candidates"),
    (35, "PAwR delayed handler calls"),
    (36, "PAwR synthetic suppressed"),
    (37, "PAwR SDC completions"),
    (38, "controller ACL packets fetched"),
    (39, "host ACL packets accepted"),
    (40, "USB Event-IN host transactions"),
    (41, "USB Event-IN DMA completions"),
    (42, "USB Event-IN continuations"),
    (43, "USB Event-IN logical completions"),
    (44, "USB Event-IN bad AMOUNT"),
    (45, "USB Event-IN stale status"),
    (46, "USB Event-IN/EPOUT2 collisions"),
    (47, "USB Event-IN late status"),
    (48, "USB Event-IN/EPOUT2 END overlap"),
)
_COUNTER_TRACE_FIRST = 49
_COUNTER_TRACE_COUNT = 13


def _print_event_ack_trace(values):
    """Decode the version-8 packed EP1 ACK trace without treating it as counters."""
    trace = values[_COUNTER_TRACE_FIRST:
                   _COUNTER_TRACE_FIRST + _COUNTER_TRACE_COUNT]
    if not trace or not any(trace):
        return

    print("   USB Event-IN ACK trace (oldest to newest):")
    for slot, word in enumerate(trace):
        chunk = (word >> 24) & 0xFF
        prefix = bytes((word & 0xFF,
                        (word >> 8) & 0xFF,
                        (word >> 16) & 0xFF))
        print("      %2d: chunk %3d  data %s"
              % (slot, chunk, prefix.hex(" ")))


def print_counters(values, baseline=None):
    if values is None:
        print("This controller does not carry the counter readout.")
        return

    names = (_impl.COUNTER_NAMES + _impl.POOL_NAMES
             + [name for _, name in _COUNTER_EXTRA_NAMES])
    width = max(len(name) for name in names)

    for i, name in enumerate(_impl.COUNTER_NAMES):
        if i >= len(values):
            break
        if baseline is not None:
            before = baseline[i] if i < len(baseline) else 0
            delta = values[i] - before
            if delta == 0:
                continue
            print("   %-*s %10d  (+%d)" % (width, name, values[i], delta))
        elif values[i]:
            print("   %-*s %10d" % (width, name, values[i]))

    any_counter = any(values[:len(_impl.COUNTER_NAMES)])
    for index, name in _COUNTER_EXTRA_NAMES:
        if index >= len(values):
            continue
        value = values[index]
        any_counter = any_counter or value != 0
        if baseline is not None:
            before = baseline[index] if index < len(baseline) else 0
            delta = value - before
            if delta == 0:
                continue
            print("   %-*s %10d  (+%d)" % (width, name, value, delta))
        elif value:
            print("   %-*s %10d" % (width, name, value))

    if baseline is None and not any_counter:
        print("   all zero")

    _impl.print_pool(values)
    if baseline is None:
        _print_event_ack_trace(values)


_impl.print_counters = print_counters


def cmd_advertise(hci, args):
    """Keep the finished ATT statistics when the peer disconnects first."""
    global _last_advertise_att

    _last_advertise_att = None
    acl_rx = None
    had_module_print = "print" in _impl.__dict__
    previous_print = _impl.__dict__.get("print")

    def advertise_print(*values, **kwargs):
        nonlocal acl_rx
        text = " ".join(str(value) for value in values)
        match = _ADVERTISE_ACL_RE.match(text)
        if match is not None:
            acl_rx = int(match.group(1))
            return
        if text.startswith("Controller ACL credits back to "):
            return
        if text in ("Bidirectional ACL and controller flow control both work.",
                    "Data flowed one way only. Check the ACL transmit path."):
            return
        if text.startswith("Peer wrote ") and text.endswith(" bytes in total."):
            return
        builtins.print(*values, **kwargs)

    _impl.print = advertise_print
    try:
        result = _original_cmd_advertise(hci, args)
    finally:
        if had_module_print:
            _impl.print = previous_print
        else:
            del _impl.print

    att = _last_advertise_att
    if acl_rx is None or att is None:
        return result

    builtins.print("ACL received %d, ATT requests %d, responses %d, notifications %d"
                   % (acl_rx, att.requests, att.responses, att.notifications))
    if att.written:
        builtins.print("Peer wrote %d bytes in total." % len(att.written))
    builtins.print("Controller ACL credits back to %d" % hci.acl_credits)
    if att.responses and hci.acl_credits > 0:
        builtins.print("Bidirectional ACL and controller flow control both work.")
        return 0
    builtins.print("Data flowed one way only. Check the ACL transmit path.")
    return 1


_impl.cmd_advertise = cmd_advertise


def _find_selected_transport():
    global _transport_error
    try:
        return _transport.discover(_transport_kind,
                                   usb_selector=_usb_selector,
                                   bulk_serialization=_usb_bulk)
    except _transport.TransportError as err:
        _transport_error = str(err)
        print("Transport discovery: %s" % err)
        return None


_impl.find_port = _find_selected_transport


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


def _wait_for_phy_update(hci, handle, timeout=_PHY_UPDATE_TIMEOUT):
    """Wait for the LE PHY Update Complete event belonging to LE Set PHY."""
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
        if (kind == _impl.H4_EVENT and code == _impl.EVT_LE_META
                and len(body) >= 6 and body[0] == _LE_PHY_UPDATE_COMPLETE):
            event_handle = struct.unpack("<H", body[2:4])[0] & 0x0FFF
            if event_handle == handle:
                _restore_packets(hci, deferred)
                return body[1], body[4], body[5]
        deferred.append(packet)

    _restore_packets(hci, deferred)
    return None


def _wait_for_remote_tx_power_report(hci, handle, timeout=_REMOTE_TX_POWER_TIMEOUT):
    """Wait for the terminal event belonging to LE Read Remote TX Power."""
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
        if (kind == _impl.H4_EVENT and code == _impl.EVT_LE_META
                and len(body) >= 5
                and body[0] == _LE_TRANSMIT_POWER_REPORTING):
            event_handle = struct.unpack("<H", body[2:4])[0] & 0x0FFF
            if (event_handle == handle
                    and body[4] == _POWER_REPORT_REASON_READ_REMOTE):
                _restore_packets(hci, deferred)
                return body[1]
        deferred.append(packet)

    _restore_packets(hci, deferred)
    return None


def _poll_ready_events(hci):
    """Process already-buffered wire events without adding a blocking poll."""
    while hci.has_pending_input():
        packet = hci.read_wire(0.1)
        if packet is None:
            break
        hci.pending.append(packet)


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

    original_commands = _impl.hci_commands.COMMANDS
    if args.only:
        selected = set(int(value, 0) for value in
                       args.only.replace(" ", "").split(","))
        _impl.hci_commands.COMMANDS = tuple(
            command for command in original_commands
            if command.opcode in selected
        )

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

        if opcode == _impl.OP_LE_SET_EVENT_MASK and len(payload) == 8:
            mask = int.from_bytes(payload, "little")
            mask |= (_LE_EVENT_MASK_PHY_UPDATE
                     | _LE_EVENT_MASK_TRANSMIT_POWER_REPORTING)
            payload = mask.to_bytes(8, "little")

        if opcode == _impl.OP_DISCONNECT and len(payload) >= 2:
            handle = struct.unpack("<H", payload[:2])[0]
            if _link_down_for(hci, handle):
                if allow_fail:
                    return 0x02, b""
                raise _impl.HciError("connection 0x%04X is already down" % handle)

        result = raw_command(opcode, payload, timeout=timeout,
                             allow_fail=allow_fail)

        if opcode == _OP_LE_SET_PHY and result[0] == 0 and len(payload) >= 2:
            handle = struct.unpack("<H", payload[:2])[0] & 0x0FFF
            terminal = _wait_for_phy_update(hci, handle)
            if terminal is None:
                raise _impl.HciError(
                    "no LE PHY Update Complete for 0x2032 handle 0x%04X"
                    % handle)
            terminal_status, tx_phy, rx_phy = terminal
            builtins.print(
                "     0x2032 PHY update complete: %s, TX 0x%02X RX 0x%02X"
                % (_impl.status_text(terminal_status), tx_phy, rx_phy))
            if terminal_status != 0:
                result = terminal_status, b""

        if (opcode == _OP_LE_READ_REMOTE_TX_POWER and result[0] == 0
                and len(payload) >= 2):
            handle = struct.unpack("<H", payload[:2])[0] & 0x0FFF
            terminal_status = _wait_for_remote_tx_power_report(hci, handle)
            if terminal_status is None:
                raise _impl.HciError(
                    "no LE Transmit Power Reporting reason 0x02 for "
                    "0x2077 handle 0x%04X" % handle)
            builtins.print(
                "     0x2077 remote TX power procedure complete: %s"
                % _impl.status_text(terminal_status))
            if terminal_status != 0:
                result = terminal_status, b""

        if (opcode == _OP_VS_WRITE_REMOTE_TX_POWER and allow_fail
                and result[0] == _STATUS_CONTROLLER_BUSY):
            builtins.print(
                "     0xFD0A Controller Busy; waiting for the prior "
                "power-control procedure")
            retries = 0
            while (result[0] == _STATUS_CONTROLLER_BUSY
                   and retries < _FD0A_BUSY_RETRIES):
                retries += 1
                time.sleep(_FD0A_BUSY_DELAY)
                result = raw_command(opcode, payload, timeout=timeout,
                                     allow_fail=True)
            if result[0] != _STATUS_CONTROLLER_BUSY:
                builtins.print(
                    "     0xFD0A busy cleared after %d %s; final %s"
                    % (retries, "retry" if retries == 1 else "retries",
                       _impl.status_text(result[0])))

        if opcode == _impl.OP_LE_CREATE_CONNECTION_CANCEL and result[0] == 0:
            _cancel_pending = True
            terminal = _wait_for_cancel_terminal(hci, 2.0)
            if terminal is not None:
                _cancel_pending = False

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
        _impl.hci_commands.COMMANDS = original_commands
        if had_module_print:
            _impl.print = previous_print
        else:
            del _impl.print
        _active_probe_hci = None
        _deferred_pairing = None
        _cancel_pending = False


_impl.probe_available = probe_available
_impl.probe_wait_for_peer = probe_wait_for_peer
_impl.probe_wait_for_ltk = probe_wait_for_ltk
_impl.cmd_probe = cmd_probe


def _transport_help_requested(argv):
    return "-h" in argv or "--help" in argv


def main():
    global _transport_kind, _usb_selector, _usb_bulk, _transport_error

    transport_parser = argparse.ArgumentParser(add_help=False)
    transport_parser.add_argument(
        "--transport", choices=("auto", "serial", "usb"), default="auto")
    transport_parser.add_argument(
        "--usb", metavar="VID:PID|SERIAL",
        help="select one native USB controller when more than one is attached")
    transport_parser.add_argument(
        "--usb-bulk", action="store_true",
        help="use Bluetooth USB Bulk Serialization (required for HCI ISO)")
    transport_args, remaining = transport_parser.parse_known_args(sys.argv[1:])

    if transport_args.transport == "serial" and \
            (transport_args.usb or transport_args.usb_bulk):
        print("--usb and --usb-bulk require --transport usb or auto")
        return 2
    if transport_args.transport == "usb" and \
            any(arg == "-p" or arg == "--port" or arg.startswith("--port=")
                for arg in remaining):
        print("-p/--port selects serial H:4; use --usb for native USB")
        return 2

    _transport_kind = transport_args.transport
    _usb_selector = transport_args.usb
    _usb_bulk = transport_args.usb_bulk
    _transport_error = None
    if _usb_selector or _usb_bulk:
        _transport_kind = "usb"

    if _transport_help_requested(remaining):
        print("Transport selection:")
        print("  --transport {auto,serial,usb}  auto prefers native USB, then CDC/UART H:4")
        print("  --usb VID:PID|SERIAL          select a native USB controller")
        print("  --usb-bulk                    use USB Bulk Serialization / HCI ISO")
        print("  -p/--port DEVICE              explicit serial H:4 port")
        print()

    original_argv = sys.argv
    sys.argv = [sys.argv[0]] + remaining
    try:
        return _impl.main()
    finally:
        sys.argv = original_argv


if __name__ == "__main__":
    sys.exit(main())
