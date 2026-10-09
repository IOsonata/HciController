#!/usr/bin/env python3
"""Static integrity checks for the nRF54LM20 IOcomposer port.

These checks validate project references and MCU/linker selection. They do
not substitute for an ARM compile/link or on-hardware verification.
"""
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "nRF54LM20"
PROJECT = PORT / "ioc" / ".project"
CPROJECT = PORT / "ioc" / ".cproject"


class Nrf54lm20ProjectTest(unittest.TestCase):
    def test_ioc_project_has_real_source_links(self):
        root = ET.parse(PROJECT).getroot()
        links = {}
        for link in root.findall("./linkedResources/link"):
            name = link.findtext("name")
            target = link.findtext("locationURI")
            if name:
                links[name] = target
        for name in ("include/board.h",
                     "include/hci_nrf54lm20.h",
                     "src/hci_nrf54lm20.cpp",
                     "src/main.cpp",
                     "src/hci_app.cpp",
                     "src/hci_sdc.cpp"):
            with self.subTest(link=name):
                self.assertIn(name, links)
                self.assertIsNotNone(links[name])
                target = links[name]
                if target.startswith("PARENT-1-PROJECT_LOC/"):
                    path = PORT / target.split("/", 1)[1]
                    self.assertTrue(path.is_file(), path)
                elif target.startswith("PARENT-2-PROJECT_LOC/"):
                    path = ROOT / target.split("/", 1)[1]
                    self.assertTrue(path.is_file(), path)

    def test_ioc_mcu_rt_os_and_controller_contract(self):
        root = ET.parse(CPROJECT).getroot()
        configs = {node.get("name") for node in root.findall(
            ".//cconfiguration/storageModule[@moduleId='org.eclipse.cdt.core.settings']")}
        self.assertEqual(configs, {"Debug", "Release"})
        content = CPROJECT.read_text()
        for token in ("NRF54LM20B_XXAA", "cortex-m33",
                      "TaktOS_M33", "ARM/cm33/ioc/",
                      "IOsonata_nRF54LM20x", "nrf54lm/hard-float",
                      "softdevice_controller_multirole",
                      "nrf54lm20a_xxaa_application.ld",
                      "nRF54LM20x/lib/include"):
            with self.subTest(token=token):
                self.assertIn(token, content)
        for forbidden in ("TaktOS_M4", "ARM/cm4/", "nRF52840",
                          "nrf52840", "S145", "nrf52/hard-float",
                          "Release_MBR"):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, content)

    def test_target_selection_and_mpsl_separation(self):
        startup = (ROOT / "src" / "main.cpp").read_text()
        source = (ROOT / "src" / "hci_nrf54lm20.cpp").read_text()
        self.assertIn("HciNrf54lm20Target()", startup)
        self.assertIn("HciNrf52840Target()", startup)
        self.assertIn("NRF54LM20B_XXAA", startup)
        for token in ("NRF_GRTC->TASKS_START", "SWI00_IRQHandler",
                      "RADIO_0_IRQHandler", "GRTC_3_IRQHandler",
                      "TIMER10_IRQHandler", "CLOCK_POWER_IRQHandler",
                      "sdc_enable", "HciSdcResourcesApply",
                      "mpsl_low_latency_acquire_callback",
                      "mpsl_low_latency_release_callback"):
            with self.subTest(token=token):
                self.assertIn(token, source)
        self.assertNotIn("SWI5_EGU5_IRQn", source)
        self.assertNotIn("NRF_POWER->USBREGSTATUS", source)


if __name__ == "__main__":
    unittest.main()
