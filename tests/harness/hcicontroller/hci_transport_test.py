#!/usr/bin/env python3
import errno
import os
import sys
from types import SimpleNamespace

sys.path.insert(0, os.path.dirname(__file__))
import hci_transport as ht


def port(device, vid=None, pid=None, manufacturer=None, product=None,
         interface=None, serial_number=None, location=None):
    return SimpleNamespace(device=device, vid=vid, pid=pid,
                           manufacturer=manufacturer, product=product,
                           interface=interface, serial_number=serial_number,
                           location=location)


class Interface:
    def __init__(self, cls, sub, proto, number=0, alt=0):
        self.bInterfaceClass = cls
        self.bInterfaceSubClass = sub
        self.bInterfaceProtocol = proto
        self.bInterfaceNumber = number
        self.bAlternateSetting = alt


class Config(list):
    pass


class Device(list):
    def __init__(self, vid, pid, manufacturer='', product='', serial='', bt=True):
        interfaces = []
        if bt:
            interfaces.append(Interface(ht.BT_CLASS, ht.BT_SUBCLASS,
                                        ht.BT_PROTOCOL))
        super().__init__([Config(interfaces)])
        self.idVendor = vid
        self.idProduct = pid
        self.manufacturer = manufacturer
        self.product = product
        self.serial_number = serial


class FakeUsbError(Exception):
    def __init__(self, text, err_no=None, backend_error_code=None):
        super().__init__(text)
        self.errno = err_no
        self.backend_error_code = backend_error_code


class FakeUsbTimeoutError(FakeUsbError):
    pass


class FakeUsbCore:
    USBError = FakeUsbError
    USBTimeoutError = FakeUsbTimeoutError


class FakeEndpoint:
    def __init__(self, chunks=()):
        self.chunks = list(chunks)

    def read(self, size, timeout=1):
        del timeout
        if not self.chunks:
            raise FakeUsbTimeoutError('timed out')
        chunk = self.chunks.pop(0)
        assert len(chunk) <= size
        return chunk


def fake_native(event_chunks=(), bulk_chunks=(), bulk_serialization=False):
    transport = ht.NativeUsbTransport.__new__(ht.NativeUsbTransport)
    transport._core = FakeUsbCore
    transport.bulk_serialization = bulk_serialization
    transport._prefetched = []
    transport._event_rx = bytearray()
    transport._bulk_rx = bytearray()
    transport.event_ep = FakeEndpoint(event_chunks)
    transport.bulk_in_ep = FakeEndpoint(bulk_chunks)
    return transport


def main():
    ports = [
        port('/dev/cu.log', ht.I_SYST_VID, ht.PID_CDC_H4,
             'I-SYST inc.', 'I-SYST HCI Controller', 'HCI controller log',
             'A1', '1-1'),
        port('/dev/cu.hci', ht.I_SYST_VID, ht.PID_CDC_H4,
             'I-SYST inc.', 'I-SYST HCI Controller', 'Bluetooth HCI H:4',
             'A1', '1-1'),
    ]
    specs = ht.serial_candidates(ports)
    assert len(specs) == 1
    assert specs[0].target == '/dev/cu.hci'

    fallback = [
        port('/dev/cu.prod', 0x1234, 0x5678,
             'I-SYST inc.', 'I-SYST HCI Controller', 'Bluetooth HCI H:4',
             'B2', '1-2')
    ]
    specs = ht.serial_candidates(fallback)
    assert len(specs) == 1 and specs[0].target == '/dev/cu.prod'

    log_only = [
        port('/dev/cu.onlylog', ht.I_SYST_VID, ht.PID_LOG_ONLY,
             'I-SYST inc.', 'I-SYST HCI Controller', 'HCI controller log')
    ]
    assert ht.serial_candidates(log_only) == []

    native = Device(ht.I_SYST_VID, ht.PID_NATIVE_HCI,
                    'I-SYST inc.', 'I-SYST HCI Controller', 'NATIVE1')
    specs = ht.usb_candidates([native])
    assert len(specs) == 1 and specs[0].kind == 'usb'
    assert ht.discover('auto', ports=ports, usb_devices=[native]).kind == 'usb'
    assert ht.discover('serial', ports=ports, usb_devices=[native]).kind == 'serial'
    assert ht.discover('usb', 'CAFE:4071', ports=ports,
                       usb_devices=[native]).kind == 'usb'
    assert ht.discover('usb', 'NATIVE1', ports=ports,
                       usb_devices=[native]).kind == 'usb'

    production = Device(0x1234, 0x9999, 'I-SYST inc.',
                        'Production HCI Controller', 'PROD1')
    assert ht.usb_candidates([production])[0].kind == 'usb'

    foreign = Device(0x1234, 0x9999, 'Other vendor', 'Bluetooth adapter', 'X')
    assert ht.usb_candidates([foreign]) == []

    cdc_device = Device(ht.I_SYST_VID, ht.PID_CDC_H4,
                        'I-SYST inc.', 'I-SYST HCI Controller', 'CDC1')
    assert ht.usb_candidates([cdc_device]) == []

    def missing_usb():
        raise ht.SelectionError(
            'PyUSB is missing; install pyusb and a libusb backend')

    original_enumerate = ht._enumerate_usb_devices
    ht._enumerate_usb_devices = missing_usb
    try:
        # AUTO must still use CDC/UART H:4 when native USB support is
        # unavailable in this Python environment.
        assert ht.discover('auto', ports=ports).kind == 'serial'

        try:
            ht.discover('auto', ports=[])
        except ht.SelectionError as err:
            assert 'PyUSB is missing' in str(err)
        else:
            raise AssertionError('AUTO hid the missing PyUSB dependency')

        try:
            ht.discover('usb', ports=[])
        except ht.SelectionError as err:
            assert 'PyUSB is missing' in str(err)
        else:
            raise AssertionError('explicit USB hid the missing PyUSB dependency')
    finally:
        ht._enumerate_usb_devices = original_enumerate

    try:
        ht.discover('usb', ports=ports, usb_devices=[])
    except ht.SelectionError as err:
        assert 'no native USB HCI controller found' in str(err)
    else:
        raise AssertionError('explicit USB silently accepted no controller')

    two = [native, Device(ht.I_SYST_VID, ht.PID_NATIVE_HCI,
                          'I-SYST inc.', 'I-SYST HCI Controller', 'NATIVE2')]
    try:
        ht.discover('usb', ports=ports, usb_devices=two)
    except ht.SelectionError as err:
        assert 'multiple native USB' in str(err)
    else:
        raise AssertionError('multiple native controllers were silently selected')

    # A STALL/PIPE is an endpoint error, not proof that the USB device vanished.
    transport = ht.NativeUsbTransport.__new__(ht.NativeUsbTransport)
    transport._core = FakeUsbCore
    pipe = FakeUsbError('Pipe error', errno.EPIPE, -9)
    classified = transport._classify_usb_error('native USB write', pipe)
    assert isinstance(classified, ht.TransportError)
    assert not isinstance(classified, ht.TransportGone)
    assert 'Pipe error' in str(classified)

    # libusb NO_DEVICE is the condition that means the controller disappeared.
    gone = FakeUsbError('No such device', errno.ENODEV, -4)
    classified = transport._classify_usb_error('native USB read', gone)
    assert isinstance(classified, ht.TransportGone)

    timeout = FakeUsbTimeoutError('timed out')
    classified = transport._classify_usb_error('native USB read', timeout)
    assert isinstance(classified, ht.TransportError)
    assert not isinstance(classified, ht.TransportGone)
    assert 'timed out' in str(classified)

    # macOS/libusb may return a partial interrupt transfer when the short
    # polling timeout expires between USB transactions. Preserve the fragment
    # and finish exactly the HCI event length on the next read.
    event = bytes([0x3E, 39]) + bytes(range(39))
    transport = fake_native(event_chunks=[event[:11]])
    assert transport.read_packet(0.0) is None
    assert bytes(transport._event_rx) == event[:11]
    transport.event_ep.chunks.extend([event[11:27], event[27:]])
    packet = transport.read_packet(0.2)
    assert packet == (ht.H4_EVENT, 0x3E, event[2:])
    assert not transport._event_rx

    # The legacy ACL bulk endpoint needs the same protection. Split the HCI
    # header itself so the expected length is not known until the second read.
    acl_payload = bytes(range(16))
    acl = bytes([0x01, 0x20, len(acl_payload), 0x00]) + acl_payload
    transport = fake_native(bulk_chunks=[acl[:3], acl[3:8], acl[8:]])
    packet = transport.read_packet(0.2)
    assert packet == (ht.H4_ACL, None, acl)
    assert not transport._bulk_rx

    # Bulk Serialization already works on the board, but its host read uses
    # the same libusb API. Keep its packet indicator and fragmented payload
    # together if a platform ever exposes that transfer in pieces too.
    serialized = bytes([ht.H4_EVENT]) + event
    transport = fake_native(
        bulk_chunks=[serialized[:7], serialized[7:19], serialized[19:]],
        bulk_serialization=True)
    packet = transport.read_packet(0.2)
    assert packet == (ht.H4_EVENT, 0x3E, event[2:])
    assert not transport._bulk_rx

    print('hci_transport_test: PASS')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
