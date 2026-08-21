#!/usr/bin/env python3
"""Keep the Python harness counter schema aligned with firmware."""

import ast
import os
import re
import sys


def fail(message):
    raise SystemExit("[!!] " + message)


def read(path):
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read()


def c_define(text, name):
    match = re.search(r"^#define\s+%s\s+(\d+)U?\s*$" % re.escape(name),
                      text, re.MULTILINE)
    if match is None:
        fail("missing %s" % name)
    return int(match.group(1))


def assignment(tree, name):
    for node in tree.body:
        if not isinstance(node, ast.Assign) or len(node.targets) != 1:
            continue
        target = node.targets[0]
        if isinstance(target, ast.Name) and target.id == name:
            return ast.literal_eval(node.value)
    fail("missing Python assignment %s" % name)


def diagnostic_trace_tail(header):
    match = re.search(r"^\s*\*\s+(\d+)-(\d+)\s+UsbEventAckTrace\b",
                      header, re.MULTILINE)
    if match is None:
        return None
    if "diagnostic trace, not monotonically increasing counters" not in header:
        fail("UsbEventAckTrace is not documented as a diagnostic trace")
    return int(match.group(1)), int(match.group(2))


def main(argv):
    if len(argv) != 2:
        print("usage: counter_schema.py REPO_ROOT")
        return 2

    root = os.path.abspath(argv[1])
    header = read(os.path.join(root, "include", "hci_counters.h"))
    dcd = read(os.path.join(root, "nRF52840", "src", "dcd_nrf5x_hci.c"))
    harness_lib = os.path.join(root, "tests", "harness", "lib")
    wrapper = read(os.path.join(harness_lib, "hci_ble_test.py"))
    implementation = read(os.path.join(harness_lib, "hci_ble_test_impl.py"))

    firmware_version = c_define(header, "HCI_COUNTERS_VERSION")
    firmware_count = c_define(header, "HCI_COUNTERS_COUNT")

    wrapper_tree = ast.parse(wrapper)
    impl_tree = ast.parse(implementation)
    host_version = assignment(wrapper_tree, "COUNTER_VERSION")
    extra_names = assignment(wrapper_tree, "_COUNTER_EXTRA_NAMES")
    base_names = assignment(impl_tree, "COUNTER_NAMES")
    pool_first = assignment(impl_tree, "POOL_FIRST_INDEX")
    pool_names = assignment(impl_tree, "POOL_NAMES")

    if host_version > firmware_version:
        fail("harness reads counter version %d but firmware emits older version %d"
             % (host_version, firmware_version))

    if len(base_names) != pool_first:
        fail("base counter names stop at %d but pool begins at %d"
             % (len(base_names), pool_first))

    extra_first = pool_first + len(pool_names)
    actual_indices = [entry[0] for entry in extra_names]
    host_count = actual_indices[-1] + 1 if actual_indices else extra_first
    expected_indices = list(range(extra_first, host_count))
    if actual_indices != expected_indices:
        fail("Python extra counter indices %s are not contiguous from %d"
             % (actual_indices, extra_first))

    if any(not isinstance(entry[1], str) or not entry[1] for entry in extra_names):
        fail("every appended counter needs a display name")

    if host_version == firmware_version:
        if host_count != firmware_count:
            fail("counter schema v%d names %d fields but firmware emits %d"
                 % (host_version, host_count, firmware_count))
        print("[ok] counter schema v%d covers all %d firmware fields"
              % (firmware_version, firmware_count))
        return 0

    trace = diagnostic_trace_tail(header)
    if trace is None:
        fail("harness reads counter version %d but firmware emits version %d"
             % (host_version, firmware_version))

    trace_first, trace_last = trace
    trace_count = trace_last - trace_first + 1
    if firmware_version != host_version + 1:
        fail("counter schema jumped from host v%d to firmware v%d"
             % (host_version, firmware_version))
    if trace_first != host_count or trace_last + 1 != firmware_count:
        fail("diagnostic trace %d-%d does not exactly follow the %d host fields"
             % (trace_first, trace_last, host_count))

    dcd_trace_count = c_define(dcd, "HCI_USB_EVENT_ACK_TRACE_DEPTH")
    if dcd_trace_count != trace_count:
        fail("DCD ACK trace depth %d disagrees with counter tail length %d"
             % (dcd_trace_count, trace_count))

    print("[ok] counter schema v%d covers %d named fields; firmware v%d "
          "appends %d diagnostic ACK trace words"
          % (host_version, host_count, firmware_version, trace_count))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
