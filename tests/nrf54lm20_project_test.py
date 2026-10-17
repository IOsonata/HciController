#!/usr/bin/env python3
"""Static integrity checks for the nRF54LM20 IOcomposer port.

These checks validate project references and MCU/linker selection. They do
not substitute for an ARM compile/link or on-hardware verification.
"""
from pathlib import Path
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
        for token in ("NRF54LM20B_XXAA", "NRF_APPLICATION", "NRFXLIB_SDC", "cortex-m33",
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

    def test_application_processor_selected_in_all_compilers(self):
        root = ET.parse(CPROJECT).getroot()
        configs = root.findall(".//cconfiguration")
        self.assertEqual(len(configs), 2)
        for config in configs:
            defines = config.findall(
                ".//option[@valueType='definedSymbols']/listOptionValue"
            )
            selected = [item.get("value") for item in defines]
            with self.subTest(config=config.get("id")):
                self.assertEqual(selected.count("NRF_APPLICATION"), 2)
                self.assertEqual(selected.count("NRF54LM20B_XXAA"), 2)
                self.assertEqual(selected.count("NRFXLIB_SDC"), 2)

    def test_debug_release_linker_script_selection(self):
        root = ET.parse(CPROJECT).getroot()
        configs = root.findall(".//cconfiguration")
        self.assertEqual(len(configs), 2)
        for config in configs:
            scripts = []
            for option in config.findall(".//option[@valueType='stringList']"):
                if option.get("name") != "Script files (-T)":
                    continue
                for item in option.findall("listOptionValue"):
                    scripts.append(item.get("value", "").strip('"'))
            with self.subTest(config=config.get("id")):
                self.assertEqual(len(scripts), 2)
                self.assertTrue(all(
                    script.endswith("/nrf54lm20a_xxaa_application.ld")
                    or script == "nrf54lm20a_xxaa_application.ld"
                    for script in scripts
                ))

    def test_board_policy_usb_dongle_vs_dual_uart(self):
        board = (PORT / "src" / "board.h").read_text()
        source = (ROOT / "src" / "main.cpp").read_text()
        udg = board.split("#if BOARD == UDG_NRF54LM20", 1)[1].split(
            "#elif BOARD == NORDIC_DK_NRF54LM20", 1)[0]
        wildthing = board.split("#elif BOARD == WILDTHING51_NRF54LM20", 1)[1].split(
            "#else\n#error \"Unsupported nRF54LM20 board\"", 1)[0]
        self.assertIn("#define HCI_BOARD_HAS_UART 0", udg)
        self.assertIn("#define HCI_HOST_SELECT HCI_HOST_SELECT_USB", udg)
        self.assertNotIn("#define UART_DEVNO", udg)
        self.assertNotIn("HCI_UART_BRIDGE", udg)
        self.assertIn("#define HCI_HOST_SELECT HCI_HOST_SELECT_UART", wildthing)
        self.assertIn("#define HCI_UART_BRIDGE 1", wildthing)
        self.assertIn("#define UART_RATE 1000000", wildthing)
        self.assertIn("#define HCI_BRIDGE_UART_RATE 115200", wildthing)
        self.assertIn("#define UART_HW_FLOWCTRL 0", wildthing)
        self.assertIn("#define UART_DEVNO 1", wildthing)
        self.assertIn("#define HCI_BRIDGE_UART_DEVNO 2", wildthing)
        for name, pin in (("UART_RX_PORT", 1), ("UART_RX_PIN", 14),
                          ("UART_TX_PORT", 3), ("UART_TX_PIN", 4)):
            with self.subTest(name=name):
                self.assertRegex(wildthing, rf"#define {name}\s+{pin}\b")
        self.assertIn("{3, 5, 1, IOPINDIR_INPUT", wildthing)
        self.assertIn("{1, 13, 1, IOPINDIR_OUTPUT", wildthing)
        self.assertNotIn("#define UART_CTS_PORT", wildthing)
        self.assertNotIn("#define UART_RTS_PORT", wildthing)
        self.assertIn("BOARD == UDG_NRF54LM20", source)
        self.assertIn("BOARD == WILDTHING51_NRF54LM20", source)

    def test_startup_failure_rollback_and_uart_bridge(self):
        app = (ROOT / "src" / "hci_app.cpp").read_text()
        self.assertIn("static void HciAppUartRelease(", app)
        self.assertIn("hostReady && pApp->UsbRunning &&", app)
        init = app.split("bool HciAppInitMode(", 1)[1].split(
            "bool HciAppInit(", 1)[0]
        self.assertGreaterEqual(init.count("HciAppUartRelease(pApp);"), 4)
        stop = app.split("void HciAppStop(", 1)[1]
        self.assertIn("HciAppUartRelease(pApp);", stop)

    def test_nrf54_sdc_partial_startup_cleanup(self):
        target = (ROOT / "src" / "hci_nrf54lm20.cpp").read_text()
        stop = target.split("static void HciStop(", 1)[1].split(
            "static bool HciStart(", 1)[0]
        self.assertIn("if (pTarget->SdcEnabled)", stop)
        self.assertIn("(void)sdc_disable();", stop)
        self.assertNotIn("if (pTarget->SdcInitialized)", stop)

    def test_uart_bridge_simple_forwarding(self):
        app = (ROOT / "src" / "hci_app.cpp").read_text()
        header = (ROOT / "include" / "hci_app.h").read_text()
        bridge = app.split("static void HciUartBridgePump(", 1)[1].split(
            "\n#endif", 1)[0]
        self.assertIn("s_BridgeUart.Rx(", bridge)
        self.assertIn("s_HciUart.Init(cfg)", app)
        self.assertIn("static UART s_HciUart;", app)
        self.assertIn("static UART s_BridgeUart;", app)
        self.assertNotIn("UARTDev_t Uart;", header)
        self.assertNotIn("UARTDev_t BridgeUart;", header)
        self.assertRegex(bridge, r"s_BridgeCdc\.Tx\(\s*0U\s*,")
        self.assertNotIn("DeviceIntrfTx(", app)
        self.assertNotIn("DeviceIntrfDisable(", app)
        self.assertIn("app->BridgePendingOffset += (uint16_t)sent", bridge)
        self.assertIn("s_BridgeCdc.IsPortOpen()", bridge)
        self.assertIn("BridgePendingLen", header)
        self.assertNotIn("TraceDroppedBytes", header)
        self.assertIn("s_BridgeCdc.Rx(", bridge)
        self.assertIn("s_BridgeUart.Tx(", bridge)
        self.assertIn("app->BridgeInputPending + app->BridgeInputOffset", bridge)
        self.assertIn("app->BridgeInputOffset += (uint16_t)sent", bridge)
        self.assertIn("app->BridgeInputOffset == app->BridgeInputLen", bridge)
        self.assertIn("BridgeInputPending[64]", header)
        self.assertIn("BridgeInputLen", header)
        self.assertIn("BridgeInputOffset", header)

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
