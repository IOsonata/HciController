#!/usr/bin/env python3
"""Transport-neutral two-controller selection for the official harness."""

import struct
import time

import hci_transport


_USB_READY_OPCODE = 0x1009
_USB_READY_EVENT_COMPLETE = 0x0E
_USB_READY_EVENT_STATUS = 0x0F
_USB_READY_TIMEOUT = 5.0
_USB_READY_ATTEMPT_TIMEOUT = 0.5
_USB_READY_RETRY_DELAY = 0.1


def _usb_selector(spec):
    identity = getattr(spec, "usb_identity", None) or {}
    serial_number = identity.get("serial")
    if not serial_number:
        device = spec.target
        serial_number = getattr(device, "serial_number", None)
    if serial_number:
        return str(serial_number)
    raise hci_transport.SelectionError(
        "native USB controller %s has no serial number; pair selection needs a stable selector"
        % spec
    )


def spec_selector(spec):
    """Return the selector another harness process can use for this controller."""
    if spec.kind == "serial":
        return str(spec.target)
    if spec.kind == "usb":
        return _usb_selector(spec)
    raise hci_transport.SelectionError("unknown transport %r" % spec.kind)


def transport_cli_args(spec):
    """CLI arguments selecting one already-resolved controller."""
    if spec.kind == "serial":
        return ["--transport", "serial", "--port", str(spec.target)]
    if spec.kind == "usb":
        return ["--transport", "usb", "--usb", _usb_selector(spec)]
    raise hci_transport.SelectionError("unknown transport %r" % spec.kind)


def bulk_spec(spec):
    """Select Bluetooth USB Bulk Serialization for ISO; serial H:4 is unchanged."""
    if spec.kind != "usb":
        return spec
    if spec.bulk_serialization:
        return spec
    return hci_transport.TransportSpec(
        "usb",
        spec.target,
        "%s bulk serialization" % spec,
        bulk_serialization=True,
        usb_identity=spec.usb_identity,
    )


def _usb_matches(spec, selector):
    serial_number = getattr(spec.target, "serial_number", None)
    if serial_number and str(serial_number) == selector:
        return True

    parts = selector.split(":")
    if len(parts) != 2:
        return False
    try:
        vid = int(parts[0], 16)
        pid = int(parts[1], 16)
    except ValueError:
        return False
    return (getattr(spec.target, "idVendor", None),
            getattr(spec.target, "idProduct", None)) == (vid, pid)


def _matches(spec, selector):
    if spec.kind == "serial":
        return str(spec.target) == selector
    if spec.kind == "usb":
        return _usb_matches(spec, selector)
    return False


def _select(candidates, selector, label):
    if selector is None:
        return None
    matches = [spec for spec in candidates if _matches(spec, selector)]
    if not matches:
        raise hci_transport.SelectionError(
            "%s controller %s was not found" % (label, selector)
        )
    if len(matches) != 1:
        raise hci_transport.SelectionError(
            "%s selector %s matched %d controllers" %
            (label, selector, len(matches))
        )
    return matches[0]


def _candidates(kind="auto", bulk_serialization=False,
                ports=None, usb_devices=None):
    if kind not in ("auto", "serial", "usb"):
        raise hci_transport.SelectionError(
            "transport must be auto, serial or usb"
        )

    usb_error = None
    usb = []
    if kind in ("auto", "usb"):
        try:
            usb = hci_transport.usb_candidates(
                usb_devices, bulk_serialization=bulk_serialization
            )
        except hci_transport.SelectionError as err:
            if kind == "usb":
                raise
            usb_error = err

        if kind == "usb":
            if usb:
                return usb
            raise hci_transport.SelectionError(
                "no native USB HCI controllers found"
            )

    serial = []
    if kind in ("auto", "serial"):
        serial = hci_transport.serial_candidates(ports)
        if kind == "serial":
            if serial:
                return serial
            raise hci_transport.SelectionError(
                "no serial H:4 HciController ports found"
            )

    candidates = usb + serial
    if candidates:
        return candidates
    if usb_error is not None:
        raise usb_error
    return []


def _probe_native_usb_ready(spec):
    """Open one fresh native USB session and prove Read BD_ADDR round-trips."""
    transport = None
    try:
        transport = spec.open()
        command = struct.pack(
            "<BHB", hci_transport.H4_COMMAND, _USB_READY_OPCODE, 0
        )
        transport.write_packet(command)
        deadline = time.monotonic() + _USB_READY_ATTEMPT_TIMEOUT

        while time.monotonic() < deadline:
            remaining = deadline - time.monotonic()
            packet = transport.read_packet(min(0.1, max(0.0, remaining)))
            if packet is None:
                continue

            kind, code, body = packet
            if kind != hci_transport.H4_EVENT:
                continue

            if code == _USB_READY_EVENT_COMPLETE and len(body) >= 4 and \
                    int.from_bytes(body[1:3], "little") == _USB_READY_OPCODE:
                if body[3] != 0:
                    return False, "Read BD_ADDR returned 0x%02X" % body[3]
                if len(body) < 10:
                    return False, "short Read BD_ADDR Command Complete"
                return True, ""

            if code == _USB_READY_EVENT_STATUS and len(body) >= 4 and \
                    int.from_bytes(body[2:4], "little") == _USB_READY_OPCODE:
                return False, "Read BD_ADDR returned Command Status 0x%02X" % body[0]

        return False, "no event for opcode 0x%04X" % _USB_READY_OPCODE
    except (hci_transport.TransportError, OSError) as err:
        return False, str(err)
    finally:
        if transport is not None:
            try:
                transport.close()
            except Exception:
                pass


def _wait_native_usb_ready(spec):
    if spec.kind != "usb":
        return

    deadline = time.monotonic() + _USB_READY_TIMEOUT
    attempt = 0
    last_error = "native USB did not answer"

    while True:
        attempt += 1
        ready, detail = _probe_native_usb_ready(spec)
        if ready:
            if attempt > 1:
                print(
                    "HOST-RECOVERY: %s became HCI-ready after %u attempt(s)"
                    % (spec, attempt)
                )
            return

        last_error = detail
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise hci_transport.SelectionError(
                "%s enumerated but did not become HCI-ready after wake: %s"
                % (spec, last_error)
            )

        time.sleep(min(_USB_READY_RETRY_DELAY, remaining))


def resolve_pair(first=None, second=None, kind="auto",
                 bulk_serialization=False, ports=None, usb_devices=None):
    """Resolve exactly two controllers, allowing mixed transports in auto mode."""
    candidates = _candidates(
        kind,
        bulk_serialization=bulk_serialization,
        ports=ports,
        usb_devices=usb_devices,
    )

    first_spec = _select(candidates, first, "first")
    second_spec = _select(candidates, second, "second")

    if first_spec is None and second_spec is None:
        if len(candidates) != 2:
            labels = ", ".join(str(spec) for spec in candidates) if candidates else "none"
            raise hci_transport.SelectionError(
                "need exactly two HciController controllers; detected %s" % labels
            )
        first_spec, second_spec = candidates
    else:
        remaining = [spec for spec in candidates
                     if spec is not first_spec and spec is not second_spec]
        if first_spec is None:
            if len(remaining) != 1:
                raise hci_transport.SelectionError(
                    "cannot choose the first controller; candidates are %s"
                    % (", ".join(str(spec) for spec in remaining)
                       if remaining else "none")
                )
            first_spec = remaining[0]
        if second_spec is None:
            remaining = [spec for spec in candidates if spec is not first_spec]
            if len(remaining) != 1:
                raise hci_transport.SelectionError(
                    "cannot choose the second controller; candidates are %s"
                    % (", ".join(str(spec) for spec in remaining)
                       if remaining else "none")
                )
            second_spec = remaining[0]

    if first_spec is second_spec:
        raise hci_transport.SelectionError(
            "the two HciController controllers must differ"
        )

    # Injected discovery lists are test snapshots. Real harness discovery uses
    # the host OS and must also prove that an enumerated native interface is a
    # usable HCI session. After macOS sleep the old USB object can remain visible
    # briefly while firmware deliberately disconnects and re-enumerates.
    if ports is None and usb_devices is None:
        _wait_native_usb_ready(first_spec)
        _wait_native_usb_ready(second_spec)

    return first_spec, second_spec
