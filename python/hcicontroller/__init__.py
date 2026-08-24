"""Host-side HCI control and BLE validation helpers for HciController.

The package keeps its low-level modules importable for advanced validation
procedures while exposing the commonly used controller, transport, DUT and
result helpers from the package root. Heavy modules that require pyserial are
loaded only when one of their exported objects is requested, so packet/event
parsers remain usable in host tests without hardware dependencies.
"""

from importlib import import_module
from pathlib import Path
import sys

_PACKAGE_DIR = str(Path(__file__).resolve().parent)
if _PACKAGE_DIR not in sys.path:
    # Several established modules still use sibling imports such as
    # ``import hci_events``. Keep those imports resolving inside this package
    # while the public package becomes the canonical implementation location.
    sys.path.insert(0, _PACKAGE_DIR)


def _alias_core_module(name):
    module = import_module(".%s" % name, __name__)
    # Assignment is deliberate rather than setdefault(). A compatibility
    # wrapper may already be executing under the historical bare name while
    # this package is imported. The canonical public module has to replace
    # that temporary wrapper before another module performs a sibling import.
    sys.modules[name] = module
    return module


# These are the modules imported by historical sibling name from the older HCI
# implementation. They have no mandatory hardware/serial dependency at import
# time, so canonicalizing them here keeps TransportSpec, HciError and command
# metadata to one module identity without pulling pyserial into parser tests.
for _name in (
    "hci_events",
    "hci_commands_catalog",
    "hci_commands",
    "hci_transport",
    "hci_cis_cleanup",
):
    _alias_core_module(_name)


_EXPORTS = {
    "Hci": ("hci_ble_test", "Hci"),
    "HciError": ("hci_ble_test", "HciError"),
    "HciGone": ("hci_ble_test", "HciGone"),
    "addr_bytes": ("hci_ble_test", "addr_bytes"),
    "addr_str": ("hci_ble_test", "addr_str"),
    "discover": ("hci_transport", "discover"),
    "TransportSpec": ("hci_transport", "TransportSpec"),
    "TransportError": ("hci_transport", "TransportError"),
    "TransportGone": ("hci_transport", "TransportGone"),
    "SelectionError": ("hci_transport", "SelectionError"),
    "resolve_pair": ("pair_transport", "resolve_pair"),
    "bulk_spec": ("pair_transport", "bulk_spec"),
    "read_controller_capabilities": ("profile", "read_controller_capabilities"),
    "DutControl": ("dut", "DutControl"),
    "Result": ("results", "Result"),
    "ResultBook": ("results", "ResultBook"),
    "PASS": ("results", "PASS"),
    "FAIL": ("results", "FAIL"),
    "NA": ("results", "NA"),
    "INCOMPLETE": ("results", "INCOMPLETE"),
}

__all__ = tuple(_EXPORTS)


def _load_module(name):
    module = sys.modules.get(name)
    if module is None:
        module = import_module(".%s" % name, __name__)
    sys.modules["%s.%s" % (__name__, name)] = module
    return module


def __getattr__(name):
    target = _EXPORTS.get(name)
    if target is None:
        raise AttributeError("module %r has no attribute %r" % (__name__, name))
    module_name, attribute = target
    value = getattr(_load_module(module_name), attribute)
    globals()[name] = value
    return value
