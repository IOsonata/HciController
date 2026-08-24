#!/usr/bin/env python3
"""Host transports and discovery for the HciController hardware tests.

The test logic uses H:4 packet type values internally because they are also the
Bluetooth USB Bulk Serialization packet indicators.  A serial transport puts
that indicator on the UART/CDC byte stream.  Native USB removes it for legacy
Bluetooth USB transfers and keeps it when Bulk Serialization is selected.
"""

import errno
import sys
import time

H4_COMMAND = 0x01
H4_ACL = 0x02
H4_SCO = 0x03
H4_EVENT = 0x04
H4_ISO = 0x05

I_SYST_VID = 0xCAFE
PID_CDC_H4 = 0x4070
PID_NATIVE_HCI = 0x4071
PID_LOG_ONLY = 0x4072

BT_CLASS = 0xE0
BT_SUBCLASS = 0x01
BT_PROTOCOL = 0x01


class TransportError(Exception):
    pass


class TransportGone(TransportError):
    pass


class SelectionError(TransportError):
    pass


class TransportSpec:
    def __init__(self, kind, target, label=None, bulk_serialization=False):
        self.kind = kind
        self.target = target
        self.label = label or str(target)
        self.bulk_serialization = bool(bulk_serialization)

    def __str__(self):
        return self.label

    def open(self):
        if self.kind == "serial":
            return SerialH4Transport(self.target)
        if self.kind == "usb":
            return NativeUsbTransport(self.target,
                                      bulk_serialization=self.bulk_serialization)
        raise SelectionError("unknown transport %r" % self.kind)


def _upper(value):
    return (value or "").upper()


def _serial_identity(info):
    serial_number = getattr(info, "serial_number", None)
    if serial_number:
        return "serial:" + str(serial_number)
    location = getattr(info, "location", None)
    if location:
        return "location:" + str(location)
    return "device:" + str(getattr(info, "device", ""))


def _serial_score(info):
    vid = getattr(info, "vid", None)
    pid = getattr(info, "pid", None)
    manufacturer = _upper(getattr(info, "manufacturer", None))
    product = _upper(getattr(info, "product", None))
    interface = _upper(getattr(info, "interface", None))
    device = str(getattr(info, "device", ""))

    if vid == I_SYST_VID and pid in (PID_NATIVE_HCI, PID_LOG_ONLY):
        return None
    if "LOG" in interface:
        return None

    exact = vid == I_SYST_VID and pid == PID_CDC_H4
    is_i_syst = "I-SYST" in manufacturer or "I-SYST" in product
    hci_interface = "HCI" in interface and "LOG" not in interface

    if exact:
        score = 100
        if hci_interface:
            score += 30
        elif interface:
            return None
    elif is_i_syst and hci_interface:
        score = 70
    else:
        return None

    if sys.platform.startswith("darwin") and "/cu." in device:
        score += 5
    return score


def serial_candidates(ports=None):
    if ports is None:
        try:
            from serial.tools import list_ports
        except ImportError:
            return []
        ports = list_ports.comports()

    ranked = []
    for info in ports:
        score = _serial_score(info)
        if score is None:
            continue
        device = getattr(info, "device", None)
        if not device:
            continue
        label = "serial H:4 %s" % device
        interface = getattr(info, "interface", None)
        if interface:
            label += " (%s)" % interface
        ranked.append((score, _serial_identity(info), str(device),
                       TransportSpec("serial", str(device), label)))

    best = {}
    for item in ranked:
        key = item[1]
        if key not in best or item[0] > best[key][0] or \
                (item[0] == best[key][0] and item[2] < best[key][2]):
            best[key] = item

    return [item[3] for item in sorted(best.values(),
                                       key=lambda item: (-item[0], item[2]))]


def _safe_usb_string(device, name):
    try:
        value = getattr(device, name, None)
    except Exception:
        return ""
    return value or ""


def _usb_has_bt_interface(device):
    try:
        configurations = list(device)
    except Exception:
        return False
    for config in configurations:
        try:
            interfaces = list(config)
        except Exception:
            continue
        for interface in interfaces:
            if (getattr(interface, "bInterfaceClass", None) == BT_CLASS and
                    getattr(interface, "bInterfaceSubClass", None) == BT_SUBCLASS and
                    getattr(interface, "bInterfaceProtocol", None) == BT_PROTOCOL):
                return True
    return False


def _usb_candidate(device):
    vid = getattr(device, "idVendor", None)
    pid = getattr(device, "idProduct", None)

    if vid == I_SYST_VID and pid in (PID_CDC_H4, PID_LOG_ONLY):
        return None

    if not _usb_has_bt_interface(device):
        return None

    manufacturer = _safe_usb_string(device, "manufacturer")
    product = _safe_usb_string(device, "product")
    serial_number = _safe_usb_string(device, "serial_number")
    known = vid == I_SYST_VID and pid == PID_NATIVE_HCI
    is_i_syst = "I-SYST" in _upper(manufacturer) or "I-SYST" in _upper(product)

    if not known and not is_i_syst:
        return None

    score = 100 if known else 70
    label = "native USB %04X:%04X" % (vid, pid)
    if serial_number:
        label += " serial=%s" % serial_number
    elif product:
        label += " %s" % product
    return score, str(serial_number), device, label


def _enumerate_usb_devices():
    try:
        import usb.core
    except ImportError as err:
        raise SelectionError(
            "PyUSB is missing; install pyusb and a libusb backend") from err

    try:
        return list(usb.core.find(find_all=True) or [])
    except usb.core.NoBackendError as err:
        raise SelectionError(
            "PyUSB cannot find a libusb backend; install libusb") from err
    except usb.core.USBError as err:
        raise SelectionError("native USB enumeration failed: %s" % err) from err
    except Exception as err:
        raise SelectionError("native USB enumeration failed: %s" % err) from err


def usb_candidates(devices=None, bulk_serialization=False):
    if devices is None:
        devices = _enumerate_usb_devices()

    ranked = []
    for device in devices:
        candidate = _usb_candidate(device)
        if candidate is None:
            continue
        score, serial_number, target, label = candidate
        ranked.append((score, serial_number, label,
                       TransportSpec("usb", target, label,
                                     bulk_serialization=bulk_serialization)))
    ranked.sort(key=lambda item: (-item[0], item[1], item[2]))
    return [item[3] for item in ranked]


def _parse_vid_pid(text):
    parts = text.split(":")
    if len(parts) != 2:
        return None
    try:
        return int(parts[0], 16), int(parts[1], 16)
    except ValueError:
        return None


def _usb_matches_selector(spec, selector):
    device = spec.target
    pair = _parse_vid_pid(selector)
    if pair is not None:
        return (getattr(device, "idVendor", None),
                getattr(device, "idProduct", None)) == pair
    serial_number = _safe_usb_string(device, "serial_number")
    return bool(serial_number) and serial_number == selector


def _one_or_error(candidates, description):
    if not candidates:
        return None
    if len(candidates) == 1:
        return candidates[0]
    labels = "\n   ".join(str(candidate) for candidate in candidates)
    raise SelectionError("multiple %s controllers found:\n   %s\n"
                         "select one explicitly" % (description, labels))


def discover(kind="auto", usb_selector=None, bulk_serialization=False,
             ports=None, usb_devices=None):
    if kind not in ("auto", "serial", "usb"):
        raise SelectionError("transport must be auto, serial or usb")

    usb_error = None
    require_usb = kind == "usb" or usb_selector is not None or bulk_serialization

    if kind in ("auto", "usb"):
        try:
            usb_specs = usb_candidates(
                usb_devices, bulk_serialization=bulk_serialization)
        except SelectionError as err:
            if require_usb:
                raise
            usb_error = err
            usb_specs = []

        if usb_selector:
            usb_specs = [spec for spec in usb_specs
                         if _usb_matches_selector(spec, usb_selector)]
            if not usb_specs:
                raise SelectionError("no native USB HCI controller matches %s"
                                     % usb_selector)
        selected = _one_or_error(usb_specs, "native USB")
        if selected is not None:
            return selected
        if require_usb:
            raise SelectionError("no native USB HCI controller found")

    serial_specs = serial_candidates(ports)
    selected = _one_or_error(serial_specs, "serial H:4")
    if selected is not None:
        return selected
    if kind == "serial":
        try:
            import serial  # noqa: F401
        except ImportError:
            raise SelectionError("pyserial is missing; install pyserial")
    if usb_error is not None:
        raise usb_error
    return None


class SerialH4Transport:
    name = "serial-h4"

    def __init__(self, port):
        try:
            import serial
        except ImportError:
            raise SelectionError("pyserial is missing; install pyserial")
        self._serial_module = serial
        self.ser = serial.Serial(port, 1000000, timeout=0.05)
        self._rx = bytearray()
        try:
            self.ser.dtr = True
            self.ser.rts = True
        except (OSError, IOError):
            pass
        time.sleep(0.1)
        self.ser.reset_input_buffer()

    def close(self):
        self.ser.close()

    def write_packet(self, packet):
        try:
            self.ser.write(packet)
            self.ser.flush()
        except (self._serial_module.SerialException, OSError) as err:
            raise TransportGone(str(err))

    @staticmethod
    def _expected_length(data):
        if not data:
            return None

        kind = data[0]
        if kind == H4_EVENT:
            if len(data) < 3:
                return None
            return 3 + data[2]
        if kind == H4_ACL:
            if len(data) < 5:
                return None
            return 5 + int.from_bytes(data[3:5], "little")
        if kind == H4_ISO:
            if len(data) < 5:
                return None
            return 5 + (int.from_bytes(data[3:5], "little") & 0x3FFF)
        raise TransportError("bad H:4 packet indicator 0x%02X, stream out of sync"
                             % kind)

    def _take_packet(self):
        expected = self._expected_length(self._rx)
        if expected is None or len(self._rx) < expected:
            return None

        packet = bytes(self._rx[:expected])
        del self._rx[:expected]
        kind = packet[0]
        if kind == H4_EVENT:
            return kind, packet[1], packet[3:]
        return kind, None, packet[1:]

    def _read_pending(self):
        try:
            waiting = int(self.ser.in_waiting)
            if waiting <= 0:
                return False
            chunk = self.ser.read(waiting)
        except (self._serial_module.SerialException, OSError) as err:
            raise TransportGone(str(err))
        if not chunk:
            return False
        self._rx.extend(chunk)
        return True

    def read_packet(self, timeout=1.0):
        timeout = max(0.0, timeout)
        deadline = time.monotonic() + timeout

        while True:
            packet = self._take_packet()
            if packet is not None:
                return packet

            if self._read_pending():
                continue

            if timeout <= 0:
                return None
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            time.sleep(min(0.001, remaining))

    def has_pending_input(self):
        if self._rx:
            return True
        try:
            return bool(self.ser.in_waiting)
        except (AttributeError, OSError):
            return False


class NativeUsbTransport:
    name = "usb-native"

    def __init__(self, device, bulk_serialization=False):
        try:
            import usb.core
            import usb.util
        except ImportError:
            raise SelectionError(
                "PyUSB is missing; install pyusb and a libusb backend")

        self._core = usb.core
        self._util = usb.util
        self.device = device
        self.bulk_serialization = bool(bulk_serialization)
        self._detached = False
        self._prefetched = []
        self._event_rx = bytearray()
        self._bulk_rx = bytearray()
        self.interface_number = self._find_interface_number()

        try:
            try:
                active = self.device.is_kernel_driver_active(self.interface_number)
            except (NotImplementedError, AttributeError):
                active = False
            if active:
                self.device.detach_kernel_driver(self.interface_number)
                self._detached = True

            try:
                self.device.get_active_configuration()
            except self._core.USBError:
                self.device.set_configuration()

            self._util.claim_interface(self.device, self.interface_number)
            alt = 1 if self.bulk_serialization else 0
            self.device.set_interface_altsetting(interface=self.interface_number,
                                                 alternate_setting=alt)
            cfg = self.device.get_active_configuration()
            interface = self._util.find_descriptor(
                cfg, bInterfaceNumber=self.interface_number,
                bAlternateSetting=alt)
            if interface is None:
                raise TransportError("Bluetooth USB interface alternate %d missing"
                                     % alt)
            self._open_endpoints(interface)
        except Exception:
            self.close()
            raise

    def _find_interface_number(self):
        for config in self.device:
            for interface in config:
                if (getattr(interface, "bInterfaceClass", None) == BT_CLASS and
                        getattr(interface, "bInterfaceSubClass", None) == BT_SUBCLASS and
                        getattr(interface, "bInterfaceProtocol", None) == BT_PROTOCOL and
                        getattr(interface, "bAlternateSetting", 0) == 0):
                    return interface.bInterfaceNumber
        raise TransportError("device has no Bluetooth HCI USB interface")

    def _open_endpoints(self, interface):
        self.event_ep = None
        self.bulk_in_ep = None
        self.bulk_out_ep = None
        for endpoint in interface:
            ep_type = self._util.endpoint_type(endpoint.bmAttributes)
            direction = self._util.endpoint_direction(endpoint.bEndpointAddress)
            if ep_type == self._util.ENDPOINT_TYPE_INTR and \
                    direction == self._util.ENDPOINT_IN:
                self.event_ep = endpoint
            elif ep_type == self._util.ENDPOINT_TYPE_BULK and \
                    direction == self._util.ENDPOINT_IN:
                self.bulk_in_ep = endpoint
            elif ep_type == self._util.ENDPOINT_TYPE_BULK and \
                    direction == self._util.ENDPOINT_OUT:
                self.bulk_out_ep = endpoint

        if self.bulk_in_ep is None or self.bulk_out_ep is None:
            raise TransportError("Bluetooth USB bulk endpoints are missing")
        if not self.bulk_serialization and self.event_ep is None:
            raise TransportError("Bluetooth USB event endpoint is missing")

    def close(self):
        device = getattr(self, "device", None)
        if device is None:
            return
        interface_number = getattr(self, "interface_number", None)
        if interface_number is not None:
            try:
                self._util.release_interface(device, interface_number)
            except Exception:
                pass
            if getattr(self, "_detached", False):
                try:
                    device.attach_kernel_driver(interface_number)
                except Exception:
                    pass
        try:
            self._util.dispose_resources(device)
        except Exception:
            pass
        self.device = None

    def _timeout(self, err):
        return isinstance(err, self._core.USBTimeoutError)

    @staticmethod
    def _gone(err):
        return (getattr(err, "backend_error_code", None) == -4 or
                getattr(err, "errno", None) == errno.ENODEV)

    def _classify_usb_error(self, operation, err):
        if self._timeout(err):
            return TransportError("%s timed out" % operation)
        message = "%s: %s" % (operation, err)
        if self._gone(err):
            return TransportGone(message)
        return TransportError(message)

    def write_packet(self, packet):
        if not packet:
            raise TransportError("empty HCI packet")
        kind = packet[0]
        body = packet[1:]

        try:
            if self.bulk_serialization:
                written = self.bulk_out_ep.write(packet, timeout=1000)
                if written != len(packet):
                    raise TransportError("short native USB bulk write")
                return

            if kind == H4_COMMAND:
                written = self.device.ctrl_transfer(
                    0x21, 0x00, 0x0000, self.interface_number,
                    body, timeout=1000)
                if written != len(body):
                    raise TransportError("short native USB command transfer")
                return
            if kind == H4_ACL:
                written = self.bulk_out_ep.write(body, timeout=1000)
                if written != len(body):
                    raise TransportError("short native USB ACL write")
                return
            if kind == H4_ISO:
                raise TransportError(
                    "HCI ISO needs USB Bulk Serialization; rerun with --usb-bulk")
            raise TransportError("HCI packet type 0x%02X is not supported by native USB"
                                 % kind)
        except self._core.USBError as err:
            raise self._classify_usb_error("native USB write", err)

    @staticmethod
    def _parse_event(data):
        if len(data) < 2:
            raise TransportError("short native USB event")
        length = data[1]
        if len(data) != length + 2:
            raise TransportError("native USB event length %d, expected %d"
                                 % (len(data), length + 2))
        return H4_EVENT, data[0], data[2:]

    @staticmethod
    def _parse_acl(data):
        if len(data) < 4:
            raise TransportError("short native USB ACL packet")
        length = int.from_bytes(data[2:4], "little")
        if len(data) != length + 4:
            raise TransportError("native USB ACL length %d, expected %d"
                                 % (len(data), length + 4))
        return H4_ACL, None, data

    @staticmethod
    def _parse_iso(data):
        if len(data) < 4:
            raise TransportError("short native USB ISO packet")
        length = int.from_bytes(data[2:4], "little") & 0x3FFF
        if len(data) != length + 4:
            raise TransportError("native USB ISO length %d, expected %d"
                                 % (len(data), length + 4))
        return H4_ISO, None, data

    def _parse_serialized(self, data):
        if not data:
            raise TransportError("empty USB Bulk Serialization packet")
        kind = data[0]
        body = data[1:]
        if kind == H4_EVENT:
            return self._parse_event(body)
        if kind == H4_ACL:
            return self._parse_acl(body)
        if kind == H4_ISO:
            return self._parse_iso(body)
        raise TransportError("unexpected USB Bulk Serialization indicator 0x%02X"
                             % kind)

    @staticmethod
    def _event_expected(data):
        if len(data) < 2:
            return None
        return data[1] + 2

    @staticmethod
    def _acl_expected(data):
        if len(data) < 4:
            return None
        return int.from_bytes(data[2:4], "little") + 4

    @staticmethod
    def _serialized_expected(data):
        if not data:
            return None
        kind = data[0]
        body = data[1:]
        if kind == H4_EVENT:
            expected = NativeUsbTransport._event_expected(body)
        elif kind == H4_ACL:
            expected = NativeUsbTransport._acl_expected(body)
        elif kind == H4_ISO:
            if len(body) < 4:
                return None
            expected = (int.from_bytes(body[2:4], "little") & 0x3FFF) + 4
        else:
            raise TransportError(
                "unexpected USB Bulk Serialization indicator 0x%02X" % kind)
        return None if expected is None else expected + 1

    @staticmethod
    def _take_complete(buffer, expected_fn, maximum, label):
        expected = expected_fn(buffer)
        if expected is None:
            return None
        if expected > maximum:
            buffer.clear()
            raise TransportError("%s length %d exceeds %d"
                                 % (label, expected, maximum))
        if len(buffer) < expected:
            return None
        packet = bytes(buffer[:expected])
        del buffer[:expected]
        return packet

    def _read_complete(self, endpoint, buffer, size, timeout_ms,
                       expected_fn, maximum, label):
        packet = self._take_complete(buffer, expected_fn, maximum, label)
        if packet is not None:
            return packet

        data = self._read_endpoint(endpoint, size, timeout_ms)
        if data is None:
            return None
        buffer.extend(data)
        return self._take_complete(buffer, expected_fn, maximum, label)

    def _read_endpoint(self, endpoint, size, timeout_ms):
        if endpoint is getattr(self, "event_ep", None):
            mps = int(getattr(endpoint, "wMaxPacketSize", 0)) & 0x07FF
            if mps > 0:
                size = min(int(size), mps)
        try:
            data = endpoint.read(size, timeout=max(1, int(timeout_ms)))
            return bytes(data)
        except self._core.USBError as err:
            if self._timeout(err):
                return None
            raise self._classify_usb_error("native USB read", err)

    def read_packet(self, timeout=1.0):
        if self._prefetched:
            return self._prefetched.pop(0)

        deadline = time.monotonic() + max(0.0, timeout)
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0 and timeout > 0:
                return None
            slice_ms = 1 if timeout <= 0 else max(1, min(20, int(remaining * 1000)))

            if self.bulk_serialization:
                data = self._read_complete(
                    self.bulk_in_ep, self._bulk_rx, 1025, slice_ms,
                    self._serialized_expected, 1025,
                    "USB Bulk Serialization packet")
                if data is not None:
                    return self._parse_serialized(data)
                if timeout <= 0 or time.monotonic() >= deadline:
                    return None
                continue

            if self._event_rx:
                data = self._read_complete(
                    self.event_ep, self._event_rx, 260, slice_ms,
                    self._event_expected, 257, "native USB event")
                if data is not None:
                    return self._parse_event(data)
                if timeout <= 0 or time.monotonic() >= deadline:
                    return None
                continue

            if self._bulk_rx:
                data = self._read_complete(
                    self.bulk_in_ep, self._bulk_rx, 1024, slice_ms,
                    self._acl_expected, 1024, "native USB ACL packet")
                if data is not None:
                    return self._parse_acl(data)
                if timeout <= 0 or time.monotonic() >= deadline:
                    return None
                continue

            data = self._read_complete(
                self.event_ep, self._event_rx, 260, slice_ms,
                self._event_expected, 257, "native USB event")
            if data is not None:
                return self._parse_event(data)
            if self._event_rx:
                if timeout <= 0 or time.monotonic() >= deadline:
                    return None
                continue

            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            slice_ms = max(1, min(20, int(remaining * 1000)))
            data = self._read_complete(
                self.bulk_in_ep, self._bulk_rx, 1024, slice_ms,
                self._acl_expected, 1024, "native USB ACL packet")
            if data is not None:
                return self._parse_acl(data)
            if timeout <= 0:
                return None

    def has_pending_input(self):
        if self._prefetched:
            return True
        packet = self.read_packet(0.0)
        if packet is None:
            return False
        self._prefetched.append(packet)
        return True
