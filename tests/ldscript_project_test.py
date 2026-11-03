#!/usr/bin/env python3
"""Verify HciController Eclipse managed linker selections for IOsonata 0.13.

This checks project paths and names without claiming compile or hardware validation.
The IOsonata sibling checkout is optional for offline source-only testing.
"""

from pathlib import Path
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
EXPECTED = {
    "nRF52840": {
        "Debug": "nRF52/gcc_nrf52840_xxaa.ld",
        "Release": "nRF52/gcc_nrf52840_xxaa.ld",
        "Release_MBR": "nRF52/gcc_nrf52840_xxaa_mbr.ld",
    },
    "nRF5340_Net": {
        "Debug": "nRF53/gcc_nrf5340_xxaa_network.ld",
        "Release": "nRF53/gcc_nrf5340_xxaa_network.ld",
    },
    "nRF54LM20": {
        "Debug": "nRF54/gcc_nrf54lm20a_xxaa.ld",
        "Release": "nRF54/gcc_nrf54lm20a_xxaa.ld",
    },
}


class LinkerProjectTest(unittest.TestCase):
    def test_managed_linker_selections(self):
        for target, configs in EXPECTED.items():
            project = ROOT / target / "ioc" / ".cproject"
            root = ET.parse(project).getroot()
            found = {}
            for config in root.findall(".//cconfiguration"):
                meta = config.find(".//configuration")
                self.assertIsNotNone(meta)
                name = meta.get("name")
                values = []
                for option in config.iter("option"):
                    identity = option.get("superClass", "")
                    if "linker.scriptfile" not in identity:
                        continue
                    values.extend(item.get("value", "").strip('"')
                                  for item in option.findall("listOptionValue"))
                if values:
                    found[name] = values
            with self.subTest(target=target):
                self.assertEqual(set(found), set(configs))
            for name, identifier in configs.items():
                with self.subTest(target=target, config=name):
                    family, filename = identifier.split("/")
                    selection = ("../../../../IOsonata/ARM/Nordic/"
                                 + family + "/ldscript/" + filename)
                    self.assertEqual(found[name], [selection, selection])

                    # Build commands execute from ioc/<configuration>.
                    build = project.parent / name
                    resolved = (build / selection).resolve()
                    expected = ROOT.parent / "IOsonata" / "ARM" / "Nordic"
                    self.assertEqual(
                        resolved,
                        (expected / family / "ldscript" / filename).resolve(),
                    )
                    if (ROOT.parent / "IOsonata").is_dir():
                        self.assertTrue(resolved.is_file(), resolved)


if __name__ == "__main__":
    unittest.main()
