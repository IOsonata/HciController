"""Auracast broadcast source for HciController.

The first half builds the data a broadcast source puts on air. It needs no
hardware and no serial library, so it can be tested on its own:

  Broadcast Audio Announcement   extended advertising, UUID 0x1852 (BAP)
  Public Broadcast Announcement  extended advertising, UUID 0x1856 (PBP)
  Broadcast Name                 extended advertising, AD type 0x30
  Basic Audio Announcement       periodic advertising, UUID 0x1851, the BASE

The second half, AuracastSource, runs the HCI sequence on a controller:
extended advertising set, periodic advertising train carrying the BASE,
LE Create BIG with one BIS per channel, an ISO data path per BIS, and then it
sends one LC3 frame per BIS every SDU interval.
"""

import struct
import time

# --- UUIDs and AD types -----------------------------------------------------

UUID_BASIC_AUDIO_ANNOUNCEMENT = 0x1851
UUID_BROADCAST_AUDIO_ANNOUNCEMENT = 0x1852
UUID_PUBLIC_BROADCAST_ANNOUNCEMENT = 0x1856

AD_SERVICE_DATA_16 = 0x16
AD_BROADCAST_NAME = 0x30

# --- Public Broadcast Announcement feature bits ----------------------------

PBA_ENCRYPTED = 0x01
PBA_STANDARD_QUALITY = 0x02
PBA_HIGH_QUALITY = 0x04

# --- LC3 codec specific configuration, BAP assigned numbers ----------------

CODEC_FORMAT_LC3 = 0x06

CFG_SAMPLING_FREQUENCY = 0x01
CFG_FRAME_DURATION = 0x02
CFG_CHANNEL_ALLOCATION = 0x03
CFG_OCTETS_PER_FRAME = 0x04
CFG_BLOCKS_PER_SDU = 0x05

SAMPLING_FREQUENCY_CODE = {
    8000: 0x01, 11025: 0x02, 16000: 0x03, 22050: 0x04, 24000: 0x05,
    32000: 0x06, 44100: 0x07, 48000: 0x08, 88200: 0x09, 96000: 0x0A,
    176400: 0x0B, 192000: 0x0C, 384000: 0x0D,
}
FRAME_DURATION_CODE = {7500: 0x00, 10000: 0x01}

LOCATION_FRONT_LEFT = 0x00000001
LOCATION_FRONT_RIGHT = 0x00000002

# --- Metadata types ---------------------------------------------------------

META_PROGRAM_INFO = 0x03
META_LANGUAGE = 0x04
META_STREAMING_CONTEXTS = 0x02

CONTEXT_UNSPECIFIED = 0x0001
CONTEXT_CONVERSATIONAL = 0x0002
CONTEXT_MEDIA = 0x0004

# Name, sampling rate Hz, frame duration us, octets per frame. These are the
# broadcast reliability settings of the BAP presets.
PRESETS = {
    "16_2_2": (16000, 10000, 40),
    "24_2_2": (24000, 10000, 60),
    "32_2_2": (32000, 10000, 80),
    "48_2_2": (48000, 10000, 100),
    "48_4_2": (48000, 10000, 120),
}


def ltv(ad_type, value=b""):
    """Length, type, value."""
    value = bytes(value)
    if len(value) + 1 > 255:
        raise ValueError("LTV value too long")
    return bytes([len(value) + 1, ad_type]) + value


def codec_config(sample_rate_hz, frame_us, octets, blocks=None):
    """Codec_Specific_Configuration LTVs shared by a subgroup."""
    if sample_rate_hz not in SAMPLING_FREQUENCY_CODE:
        raise ValueError("unsupported sampling rate %r" % sample_rate_hz)
    if frame_us not in FRAME_DURATION_CODE:
        raise ValueError("frame duration must be 7500 or 10000 us")
    data = ltv(CFG_SAMPLING_FREQUENCY,
               bytes([SAMPLING_FREQUENCY_CODE[sample_rate_hz]]))
    data += ltv(CFG_FRAME_DURATION, bytes([FRAME_DURATION_CODE[frame_us]]))
    data += ltv(CFG_OCTETS_PER_FRAME, struct.pack("<H", octets))
    if blocks is not None:
        data += ltv(CFG_BLOCKS_PER_SDU, bytes([blocks]))
    return data


def channel_allocation(locations):
    """Audio_Channel_Allocation LTV for one BIS."""
    return ltv(CFG_CHANNEL_ALLOCATION, struct.pack("<I", locations))


def metadata(contexts=None, program_info=None, language=None):
    """Metadata LTVs."""
    data = b""
    if contexts is not None:
        data += ltv(META_STREAMING_CONTEXTS, struct.pack("<H", contexts))
    if program_info is not None:
        data += ltv(META_PROGRAM_INFO, program_info.encode("utf-8"))
    if language is not None:
        code = language.encode("ascii")
        if len(code) != 3:
            raise ValueError("language is a three letter ISO 639-3 code")
        data += ltv(META_LANGUAGE, code)
    return data


def service_data(uuid16, payload=b""):
    """Service Data - 16 bit UUID AD structure."""
    return ltv(AD_SERVICE_DATA_16, struct.pack("<H", uuid16) + bytes(payload))


def base(presentation_delay_us, subgroups):
    """Basic Audio Announcement service data, the BASE.

    subgroups is a list of dicts with:
      codec_config  bytes, codec specific configuration of the subgroup
      metadata      bytes, optional
      bis           list of (bis_index, bytes) with the per BIS configuration
    """
    if not 0 <= presentation_delay_us <= 0xFFFFFF:
        raise ValueError("presentation delay is 24 bits")
    if not 1 <= len(subgroups) <= 31:
        raise ValueError("a BASE carries 1 to 31 subgroups")

    data = struct.pack("<I", presentation_delay_us)[:3]
    data += bytes([len(subgroups)])
    for group in subgroups:
        cfg = bytes(group["codec_config"])
        meta = bytes(group.get("metadata", b""))
        bis = group["bis"]
        if not bis:
            raise ValueError("a subgroup needs at least one BIS")
        data += bytes([len(bis)])
        data += bytes([CODEC_FORMAT_LC3, 0, 0, 0, 0])
        data += bytes([len(cfg)]) + cfg
        data += bytes([len(meta)]) + meta
        for index, bis_cfg in bis:
            bis_cfg = bytes(bis_cfg)
            data += bytes([index, len(bis_cfg)]) + bis_cfg
    return service_data(UUID_BASIC_AUDIO_ANNOUNCEMENT, data)


def stereo_base(presentation_delay_us, sample_rate_hz, frame_us, octets,
                contexts=CONTEXT_MEDIA, program_info=None, language=None):
    """BASE of one subgroup holding a left and a right BIS."""
    group = {
        "codec_config": codec_config(sample_rate_hz, frame_us, octets),
        "metadata": metadata(contexts, program_info, language),
        "bis": [
            (1, channel_allocation(LOCATION_FRONT_LEFT)),
            (2, channel_allocation(LOCATION_FRONT_RIGHT)),
        ],
    }
    return base(presentation_delay_us, [group])


def broadcast_announcement(broadcast_id):
    """Broadcast Audio Announcement, UUID 0x1852, with the 24 bit ID."""
    if not 0 <= broadcast_id <= 0xFFFFFF:
        raise ValueError("Broadcast_ID is 24 bits")
    return service_data(UUID_BROADCAST_AUDIO_ANNOUNCEMENT,
                        struct.pack("<I", broadcast_id)[:3])


def public_announcement(features, meta=b""):
    """Public Broadcast Announcement, UUID 0x1856."""
    meta = bytes(meta)
    return service_data(UUID_PUBLIC_BROADCAST_ANNOUNCEMENT,
                        bytes([features, len(meta)]) + meta)


def broadcast_name(name):
    """Broadcast Name AD structure, 4 to 32 octets of UTF-8."""
    raw = name.encode("utf-8")
    if not 4 <= len(raw) <= 32:
        raise ValueError("Broadcast Name is 4 to 32 octets")
    return ltv(AD_BROADCAST_NAME, raw)


def extended_adv_data(broadcast_id, name, encrypted=False, high_quality=False):
    """Extended advertising data of an Auracast source."""
    features = PBA_HIGH_QUALITY if high_quality else PBA_STANDARD_QUALITY
    if encrypted:
        features |= PBA_ENCRYPTED
    return (broadcast_announcement(broadcast_id)
            + public_announcement(features)
            + broadcast_name(name))


def broadcast_code(text):
    """16 octet Broadcast_Code from a string, zero padded (BAP 3.2.1)."""
    raw = text.encode("utf-8")
    if len(raw) > 16:
        raise ValueError("Broadcast_Code is at most 16 octets")
    return raw + bytes(16 - len(raw))


# --- HCI side ---------------------------------------------------------------

OP_LE_SET_PERIODIC_ADV_PARAMS = 0x203E
OP_LE_SET_PERIODIC_ADV_DATA = 0x203F
OP_LE_CREATE_BIG = 0x2068
OP_LE_TERMINATE_BIG = 0x206A

LE_BIG_COMPLETE = 0x1B
LE_TERMINATE_BIG_COMPLETE = 0x1C

ADV_HANDLE = 0
BIG_HANDLE = 0
BIG_TERMINATE_LOCAL = 0x16
PERIODIC_INTERVAL = 0x0050          # 100 ms in 1.25 ms units
PERIODIC_DATA_MAX = 252             # LE Set Periodic Advertising Data

SDU_INTERVAL_US = {7500: 7500, 10000: 10000}


def periodic_data_commands(handle, data):
    """LE Set Periodic Advertising Data payloads: complete, or first,
    intermediate and last when the data does not fit one command."""
    data = bytes(data)
    if len(data) <= PERIODIC_DATA_MAX:
        return [bytes([handle, 0x03, len(data)]) + data]
    chunks = [data[i:i + PERIODIC_DATA_MAX]
              for i in range(0, len(data), PERIODIC_DATA_MAX)]
    out = []
    for i, chunk in enumerate(chunks):
        if i == 0:
            op = 0x00
        elif i == len(chunks) - 1:
            op = 0x02
        else:
            op = 0x01
        out.append(bytes([handle, op, len(chunk)]) + chunk)
    return out


def create_big_payload(num_bis, frame_us, sdu_octets, rtn, latency_ms, phy,
                       code=None):
    """LE Create BIG parameters."""
    payload = bytes([BIG_HANDLE, ADV_HANDLE, num_bis])
    payload += struct.pack("<I", SDU_INTERVAL_US[frame_us])[:3]
    payload += struct.pack("<HH", sdu_octets, latency_ms)
    payload += bytes([
        rtn,
        phy,            # 1 = LE 1M, 2 = LE 2M
        0,              # sequential packing
        0,              # unframed
        1 if code else 0,
    ])
    payload += code if code else bytes(16)
    return payload


class AuracastSource:
    """Runs an Auracast source on one controller.

    hci is an IsoHci, already open. Call start(), then stream(), then stop().
    """

    def __init__(self, hci, broadcast_id, name, preset="48_2_2",
                 presentation_delay_us=40000, rtn=4, latency_ms=65, phy=2,
                 password=None, program_info=None, language=None):
        if preset not in PRESETS:
            raise ValueError("unknown preset %r" % preset)
        self.hci = hci
        self.rate, self.frame_us, self.octets = PRESETS[preset]
        self.broadcast_id = broadcast_id
        self.name = name
        self.delay_us = presentation_delay_us
        self.rtn = rtn
        self.latency_ms = latency_ms
        self.phy = phy
        self.code = broadcast_code(password) if password else None
        self.program_info = program_info
        self.language = language
        self.bis_handles = []
        self._started = False

    def start(self):
        # Imported here so the encoders above work without serial support.
        from .hci_pair import prepare_controller
        from .periodic_features import (
            ADV_SID_PERIODIC,
            _command_ok,
            _configure_ext_set,
            _ext_adv_enable,
        )
        from .connected_features import HciError, wait_le_subevent
        from hci_cis_pair_test import ISO_DIRECTION_INPUT, setup_iso_path

        hci = self.hci
        _, addr_type, _ = prepare_controller(hci)

        adv_data = extended_adv_data(
            self.broadcast_id, self.name, encrypted=self.code is not None)
        _configure_ext_set(hci, ADV_HANDLE, addr_type, 0x0000,
                           ADV_SID_PERIODIC, adv_data)

        _command_ok(
            hci, OP_LE_SET_PERIODIC_ADV_PARAMS,
            struct.pack("<BHHH", ADV_HANDLE, PERIODIC_INTERVAL,
                        PERIODIC_INTERVAL, 0),
            "LE Set Periodic Advertising Parameters")

        base_ad = stereo_base(self.delay_us, self.rate, self.frame_us,
                              self.octets, program_info=self.program_info,
                              language=self.language)
        for payload in periodic_data_commands(ADV_HANDLE, base_ad):
            _command_ok(hci, OP_LE_SET_PERIODIC_ADV_DATA, payload,
                        "LE Set Periodic Advertising Data")

        _ext_adv_enable(hci, ADV_HANDLE, True)
        _command_ok(hci, 0x2040, bytes([1, ADV_HANDLE]),
                    "LE Set Periodic Advertising Enable")

        status, _ = hci.command(
            OP_LE_CREATE_BIG,
            create_big_payload(2, self.frame_us, self.octets, self.rtn,
                               self.latency_ms, self.phy, self.code),
            allow_fail=True)
        if status != 0:
            raise HciError("LE Create BIG returned 0x%02X" % status)

        body = wait_le_subevent(
            hci, LE_BIG_COMPLETE,
            predicate=lambda e: len(e) >= 21 and e[2] == BIG_HANDLE,
            timeout=10.0)
        if body is None:
            raise HciError("no LE Create BIG Complete event")
        if body[1] != 0:
            raise HciError("LE Create BIG Complete status 0x%02X" % body[1])
        count = body[18]
        self.bis_handles = [
            struct.unpack("<H", body[19 + 2 * i:21 + 2 * i])[0] & 0x0FFF
            for i in range(count)]
        for handle in self.bis_handles:
            setup_iso_path(hci, handle, ISO_DIRECTION_INPUT)
        self._started = True

    def stream(self, frames, seconds=None):
        """Send audio. frames() returns a list with one LC3 frame per BIS, or
        None when the audio is over. Paced on the SDU interval."""
        interval = self.frame_us / 1e6
        deadline = time.monotonic()
        seq = 0
        end = None if seconds is None else deadline + seconds
        while end is None or time.monotonic() < end:
            frame_set = frames()
            if frame_set is None:
                break
            for handle, sdu in zip(self.bis_handles, frame_set):
                self.hci.send_iso(handle, seq, sdu)
            seq = (seq + 1) & 0xFFFF
            deadline += interval
            # Keep reading events while waiting so Number Of Completed
            # Packets and the like do not pile up in the port buffer.
            while True:
                left = deadline - time.monotonic()
                if left <= 0:
                    break
                self.hci.read_packet(min(left, 0.002))

    def stop(self):
        from .bis_features import _remove_iso_path
        from .periodic_features import _stop_periodic

        for handle in self.bis_handles:
            _remove_iso_path(self.hci, handle, 0x01)
        if self._started:
            try:
                self.hci.command(
                    OP_LE_TERMINATE_BIG,
                    bytes([BIG_HANDLE, BIG_TERMINATE_LOCAL]),
                    allow_fail=True)
            except Exception:
                pass
        _stop_periodic(self.hci)
        self.bis_handles = []
        self._started = False
