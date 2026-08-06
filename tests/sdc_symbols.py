#!/usr/bin/env python3
"""
Check that every SDC command the dispatch table calls is in the library.

This firmware links libsoftdevice_controller_multirole and only that. An HCI
controller exposes the whole controller to its host and the host chooses roles
at run time, so there is no variant to select between and no row in
src/hci_sdc_nrfxlib.cpp is conditional.

That makes the useful question a different one. Not "which rows should be
compiled in", which has one answer, but "does the library this release ships
with still define everything the table calls". An nrfxlib upgrade that drops or
renames a function is otherwise a link error naming one symbol with no
indication of how many others went with it.

    python3 tests/sdc_symbols.py
    python3 tests/sdc_symbols.py /path/to/libsoftdevice_controller_multirole.a

It reads the archive symbol index directly, so it needs no toolchain. The
alternative, arm-none-eabi-nm, is only on the machine that has the cross
compiler installed, and this question comes up before the first build as often
as after it.

Exits non-zero when the table calls something the archive does not define.
"""

import os
import re
import struct
import sys

# Named absent on purpose. The multirole library does not define this one, the
# table has no row for it, and the supported commands bitmap leaves its bit
# clear. Reported rather than ignored, so that an nrfxlib release which starts
# providing it is noticed instead of silently staying unused.
KNOWN_ABSENT = ["sdc_hci_cmd_le_read_supported_states"]

DEFAULT_LIB = ("../external/sdk-nrfxlib/softdevice_controller/lib/nrf52/"
               "hard-float/libsoftdevice_controller_multirole.a")

TABLE_SOURCE = ("src", "hci_sdc_nrfxlib.cpp")


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


def strip_noise(text):
    """
    Comments and #include lines, removed before names are collected. Both
    mention sdc_hci_cmd_ names that are not calls: the headers are called
    sdc_hci_cmd_le.h and the like, and the commentary discusses commands the
    table does not carry.
    """
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    text = re.sub(r"^\s*#\s*include[^\n]*", " ", text, flags=re.M)
    return text


def table_calls(path):
    """
    Every sdc_hci_cmd_ function named in the dispatch table source. Names reach
    the SDC call either directly or as a macro argument, so both a following
    parenthesis and a following comma count. Type names share the prefix and
    end in _t, so they are dropped.
    """
    with open(path) as handle:
        text = strip_noise(handle.read())

    found = set()
    for name in re.findall(r"\b(sdc_hci_cmd_[a-z0-9_]+)\s*[(,)]", text):
        if name.endswith("_t"):
            continue
        found.add(name)
    return sorted(found)


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
        print("Pass the path to a libsoftdevice_controller_multirole.a.")
        return 2

    source = os.path.join(root, *TABLE_SOURCE)
    if not os.path.exists(source):
        print("No dispatch table at %s" % source)
        return 2

    try:
        symbols = archive_symbols(lib)
    except ValueError as error:
        print(error)
        return 2

    calls = table_calls(source)
    offered = sorted(s for s in symbols if s.startswith("sdc_hci_cmd_"))
    missing = [c for c in calls
               if c not in symbols and c not in KNOWN_ABSENT]

    print("%s" % lib)
    print("%d symbols, %d of them HCI commands" % (len(symbols), len(offered)))
    print("%s calls %d of them" % (os.path.join(*TABLE_SOURCE), len(calls)))
    print()

    for name in KNOWN_ABSENT:
        if name in symbols:
            print("  %-56s now present, the table could carry it" % name)
        else:
            print("  %-56s absent, as expected" % name)

    print()
    if missing:
        for name in missing:
            print("  MISSING  %s" % name)
        print()
        print("%d command(s) the table calls are not in this library. Building "
              "against it would fail to link. Either the nrfxlib release "
              "dropped them or the table gained a row that was never checked."
              % len(missing))
        return 1

    print("Every command the table calls is in this library.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
