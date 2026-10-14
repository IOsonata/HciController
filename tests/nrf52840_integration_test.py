#!/usr/bin/env python3
"""Source-level nRF52840 IOC and integration contract regression tests.

Checks are independent of the target toolchain. A successful run is not a
substitute for the ARM link, USB enumeration, or on-board UART validation.
"""
from pathlib import Path
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
IOC = ROOT / "nRF52840" / "ioc"


class Nrf52840IntegrationTest(unittest.TestCase):
    def test_sdc_build_configurations_match_iosonata_main(self):
        project = ET.parse(IOC / ".cproject").getroot()
        configs = project.findall(
            ".//cconfiguration/storageModule[@moduleId='org.eclipse.cdt.core.settings']"
        )
        self.assertEqual({c.get("name") for c in configs},
                         {"Debug", "Release", "Release_MBR", "Release_SD"})
        for cfg in configs:
            with self.subTest(config=cfg.get("name")):
                parent = cfg.getparent() if hasattr(cfg, "getparent") else None
                # ElementTree has no parent links: locate containing config.
                data = (IOC / ".cproject").read_text()
                start = data.find("<cconfiguration", 0)
                blocks = data.split("<cconfiguration")[1:]
                matched = [b for b in blocks if
                           f'name="{cfg.get("name")}"' in b.split("</cconfiguration>")[0]]
                self.assertEqual(len(matched), 1)
                text = matched[0].split("</cconfiguration>")[0]
                self.assertEqual(text.count('value="NRFXLIB_SDC"'), 2)
                self.assertEqual(text.count('value="NRF52840_XXAA"'), 2)
                self.assertIn("IOsonata_nRF52840", text)
                self.assertIn("TaktOS_M4", text)
                self.assertIn("softdevice_controller_multirole", text)
                self.assertIn("nrf52/hard-float", text)
                self.assertIn("nRF52840/lib/ioc/", text)

    def test_usb_worker_and_shared_clock_irq(self):
        app = (ROOT / "src" / "hci_app.cpp").read_text()
        target = (ROOT / "src" / "hci_nrf52840.cpp").read_text()
        self.assertIn("bool UsbEvtQue(", app)
        self.assertIn("UsbCheckStatus();", app)
        self.assertIn("nRFPowerClockIrqEnable(", target)
        self.assertIn("void nRFClockIrqHandler(", target)
        self.assertNotIn("void POWER_CLOCK_IRQHandler(", target)
        self.assertNotIn("void UsbdXtalRequest(", target)

    def test_board_specific_uart_policy(self):
        board = (ROOT / "nRF52840" / "src" / "board.h").read_text()
        udg = board.split("#if BOARD == UDG_NRF52840", 1)[1].split(
            "#elif BOARD == IBK_NRF52840", 1)[0]
        thingy = board.split("#elif BOARD == THINGY91_NRF52840", 1)[1].split(
            "#elif BOARD == WILDTHING91", 1)[0]
        wildthing = board.split("#elif BOARD == WILDTHING51", 1)[1].split(
            '\n#else\n#error "No pins defined.', 1)[0]
        self.assertIn("#define HCI_BOARD_HAS_UART 0", udg)
        self.assertNotIn("#define UART_PINS", udg)
        self.assertIn("#define HCI_NRF91_TRACE_BRIDGE 1", thingy)
        self.assertIn("#define HCI_NRF91_TRACE_UART_RATE 115200", thingy)
        self.assertIn("#define UART_HW_FLOWCTRL 0", wildthing)
        self.assertIn("#define HCI_NRF91_TRACE_BRIDGE 1", wildthing)
        self.assertIn("#define HCI_NRF91_TRACE_UART_DEVNO 1", wildthing)
        self.assertIn("#define UART_RATE", wildthing)


if __name__ == "__main__":
    unittest.main()
