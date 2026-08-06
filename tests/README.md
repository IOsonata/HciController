# HciController tests

Two kinds of test live here. The C++ tests under `unit/` run on the build
machine and need nothing but a C++14 compiler. The Python tools under
`hardware/` drive a real UDG-NRF52840x over its USB CDC port.

```
tests/
  Makefile              builds and runs every C++ test
  unit/                 the C++ tests
  stubs/                fake target headers, see below
  hardware/             Python tools that talk to a board
```

## Running the C++ tests

```sh
make -C tests run
```

Everything is compiled natively. The headers the firmware needs on device,
`nrf.h`, MPSL, the SoftDevice Controller, TinyUSB, TaktOS and the IOsonata
device interface, are replaced by the fakes in `stubs/`, so no SDK checkout is
required and no board has to be attached.

One test is the exception. `hci_sdc_dispatch_test` compiles the real command
table in `src/hci_sdc_nrfxlib.cpp` against the real SoftDevice Controller
headers, with only the SDC entry points stubbed out at link time. That catches
a wrong parameter type or a missing include that a hand written fake header
would hide. It is built when the headers can be found and skipped otherwise:

```sh
make -C tests run NRFXLIB_DIR=/path/to/sdk-nrfxlib
```

The default location is `../external/sdk-nrfxlib` relative to the repository
root. `make -C tests print-config` shows what was resolved.

The stubs for the SDC entry points in `stubs/sdclink/sdc_stub.cpp` are
generated from the headers, so they cannot drift from the real prototypes. A
build failure there means the nrfxlib version changed, not that the test is
wrong.

## What each test covers

| Test | Layer | What it pins down |
| --- | --- | --- |
| `hci_h4_test` | `hci_h4.cpp` | H:4 framing, header lengths per indicator, oversize drop and resynchronisation, delivery backpressure |
| `hci_cmd_dispatch_test` | `hci_cmd_dispatch.cpp` | opcode lookup, parameter length checking, Command Complete and Command Status construction |
| `hci_sdc_test` | `hci_sdc.cpp` | routing between the transport and the dispatch table, event pending handling |
| `hci_intrf_transport_test` | `hci_intrf_transport.cpp` | short reads and short writes, indicator prefixing, oversize refusal, closed port |
| `hci_controller_test` | `hci_controller.cpp` | both directions of the bridge, retry of a refused packet, rejection of an indicator the controller must not emit |
| `hci_nrf52840_usb_test` | `hci_nrf52840.cpp` | USB bring up order, that MPSL keeps the clock, that every wait is bounded and reports its own error code |
| `hci_taktos_poll_test` | `hci_taktos.cpp` | the poll interval, so a lost wake cannot stall the USB transport |
| `hci_tinyusb_init_test` | `hci_tinyusb.cpp` | that bring up goes through `tusb_rhport_init` and records the device role, which is what makes the interrupt handler dispatch |
| `hci_sdc_dispatch_test` | `hci_sdc_nrfxlib.cpp` | all 68 opcodes reach the intended SDC call with the right response type and parameter length, and the supported commands bitmap agrees with the table in both directions |

## Hardware tools

`hardware/` holds three Python tools. They need Python 3.8 or later and no
packages beyond the standard library.

`hci_test.py` checks the transport itself. It opens the CDC port, resets the
controller and runs nine checks covering framing, command response pairing and
the buffer size report.

```sh
python3 tests/hardware/hci_test.py /dev/ttyACM0
```

`hci_ble_test.py` drives the radio. It implements enough of a host to
advertise, scan, connect, and serve a small GATT database with Generic Access
and the Nordic UART Service, so a phone can find the board, connect, discover
services and exchange data. It also runs direct test mode.

```sh
python3 tests/hardware/hci_ble_test.py /dev/ttyACM0 advertise
python3 tests/hardware/hci_ble_test.py /dev/ttyACM0 scan
python3 tests/hardware/hci_ble_test.py /dev/ttyACM0 connect
python3 tests/hardware/hci_ble_test.py /dev/ttyACM0 dtm-tx
python3 tests/hardware/hci_ble_test.py /dev/ttyACM0 counters
```

`counters` reads the firmware's own tallies over the vendor specific opcode in
`hci_counters.h`. Nothing else puts those numbers on the wire, so without it a
running board can only be questioned with a debugger.

`connect --flood N` sends N ACL packets while ignoring flow control and reports
which counters moved. It first says how many of the N reached
`sdc_hci_data_put` at all, because a block of refusal counters reading zero
looks the same whether the controller accepted everything or nothing ever
arrived. Once that is established it distinguishes the retry path firing, the
controller refusing with some other error, and the controller taking every
packet.

`sdc_symbols.py` reads a SoftDevice Controller archive and says which of the
optional dispatch table rows its library actually defines, so the macros in
`src/hci_sdc_nrfxlib.cpp` are set from evidence rather than from assumption. It
parses the archive symbol index directly and needs no cross toolchain.

```sh
python3 tests/sdc_symbols.py
python3 tests/sdc_symbols.py /path/to/libsoftdevice_controller_multirole.a
```

`fake_controller.py` is a controller simulator on a pty. It answers the same
opcodes the firmware does and can simulate a connection, so the two tools above
can be exercised and debugged with no board attached.

```sh
python3 tests/hardware/fake_controller.py
```

Host tests do not replace hardware validation. They pin down the parts that can
be reasoned about off target, which is framing, dispatch and start up ordering.
Everything involving the radio, real USB enumeration or MPSL timing has to be
checked on a UDG-NRF52840x.
