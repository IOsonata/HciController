#!/usr/bin/env python3
"""Transport-neutral two-controller selection for the official harness."""

import hci_transport


def _usb_selector(spec):
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
    if kind in ("auto", "usb"):
        try:
            usb = hci_transport.usb_candidates(
                usb_devices, bulk_serialization=bulk_serialization
            )
        except hci_transport.SelectionError as err:
            if kind == "usb":
                raise
            usb_error = err
            usb = []
        if usb:
            return usb
        if kind == "usb":
            raise hci_transport.SelectionError(
                "no native USB HCI controllers found"
            )

    serial = hci_transport.serial_candidates(ports)
    if serial:
        return serial
    if kind == "serial":
        raise hci_transport.SelectionError(
            "no serial H:4 HciController ports found"
        )
    if usb_error is not None:
        raise usb_error
    return []


def resolve_pair(first=None, second=None, kind="auto",
                 bulk_serialization=False, ports=None, usb_devices=None):
    """Resolve exactly two controllers of one transport kind."""
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
    if first_spec.kind != second_spec.kind:
        raise hci_transport.SelectionError(
            "the two controllers must use the same host transport"
        )
    return first_spec, second_spec
