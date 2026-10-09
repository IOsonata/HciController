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

    def test_board_policy_usb_dongle_vs_dual_uart(self):
        board = (PORT / "src" / "board.h").read_text()
        source = (ROOT / "src" / "main.cpp").read_text()
        udg = board.split("#if BOARD == UDG_NRF54LM20", 1)[1].split(
            "#elif BOARD == NORDIC_DK_NRF54LM20", 1)[0]
        wildthing = board.split("#elif BOARD == WILDTHING51_NRF54LM20", 1)[1].split(
            "#else\\n#error \"Unsupported nRF54LM20 board\"", 1)[0]
        self.assertIn("#define HCI_BOARD_HAS_UART 0", udg)
        self.assertIn("#define HCI_HOST_SELECT HCI_HOST_SELECT_USB", udg)
        self.assertNotIn("#define UART_DEVNO", udg)
        self.assertNotIn("HCI_NRF91_TRACE_BRIDGE", udg)
        self.assertIn("#define HCI_HOST_SELECT HCI_HOST_SELECT_UART", wildthing)
        self.assertIn("#define HCI_NRF91_TRACE_BRIDGE 1", wildthing)
        self.assertIn("#define UART_RATE 1000000", wildthing)
        self.assertIn("#define HCI_NRF91_TRACE_UART_RATE 115200", wildthing)
        self.assertIn("#define UART_HW_FLOWCTRL 0", wildthing)
        self.assertIn("#define UART_DEVNO 1", wildthing)
        self.assertIn("#define HCI_NRF91_TRACE_UART_DEVNO 2", wildthing)
        for name, pin in (("UART_RX_PORT", 1), ("UART_RX_PIN", 14),
                          ("UART_TX_PORT", 3), ("UART_TX_PIN", 4)):
            with self.subTest(name=name):
                self.assertRegex(wildthing, rf"#define {name}\\s+{pin}\\b")
        self.assertIn("{3, 5, 1, IOPINDIR_INPUT", wildthing)
        self.assertIn("{1, 13, 1, IOPINDIR_OUTPUT", wildthing)
        self.assertNotIn("#define UART_CTS_PORT", wildthing)
        self.assertNotIn("#define UART_RTS_PORT", wildthing)
        self.assertIn("BOARD == UDG_NRF54LM20", source)
        self.assertIn("BOARD == WILDTHING51_NRF54LM20", source)

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
