# HciController tests

Two kinds of test live here. The C++ tests under `unit/` run on the build
machine and need nothing but a C++14 compiler. The Python tools under
`hardware/` drive a real UDG-NRF52840x over its USB CDC port.

```
tests/
  Makefile              builds and runs every C++ test
  unit/                 the C++ tests
  stubs/                fake target headers, see below
  project_files.py      the Eclipse project against the tree
  command_coverage.py   the Python tooling against the dispatch table
  hardware/             Python tools that talk to a board
```

The fakes under `stubs/` now cover the IOsonata UART, the USB CDC interface,
the circular FIFO and a stand in `board.h`, so `src/hci_app.cpp` compiles off
target. They carry the member names from the real headers and none of the
behaviour.

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
| `hci_sdc_dispatch_test` | `hci_sdc_nrfxlib.cpp` | all 126 opcodes reach the intended SDC call with the right response type and parameter length, and the supported commands bitmap agrees with the table in both directions |
| `hci_sdc_resources_test` | `hci_sdc_resources.h` | every term of the SoftDevice Controller memory pool against the real nrfxlib macros, and every `sdc_cfg_t` member the firmware writes against the real union |

The last two need `NRFXLIB_DIR` and are skipped without it. They are the only
tests that see the vendor headers; everything else builds against the fakes
under `stubs/`.

That matters most for `hci_sdc_resources_test`. `stubs/sdc/sdc.h` holds
hand written copies of the `SDC_MEM_*` macros, and the pool is computed from
them in every other test. Both that test and `hci_nrf52840_usb_test` measure
their headers against `unit/hci_sdc_expected_resources.h`, so a copy that
drifts from the vendor header fails the stub build while the real one still
passes, and which of the two failed says which header moved.

## What is compiled but not run

Two things are built by `make -C tests run` and never executed, because a
compile is all that is needed to catch them.

`src/hci_app.cpp` is compiled. It is the largest source here and no test links
it, so a structure member that moved under it used to be found only by an
`arm-none-eabi` build. Linking it would need MPSL, TinyUSB and a controller
all answering, which is a target's job; compiling it costs nothing and catches
the member names, the driver callback signatures and the configuration fields
it fills in.

Every source that does not need the vendor headers is also compiled a second
time with `HCI_TRACE=1`. With tracing off the macro discards its arguments, so
an argument naming a field that no longer exists is invisible until an ARM
build with tracing on. `HciTrace` has the printf format attribute, so the
second build checks every trace call against its format string.

That second build is why `hci_trace.h` has a non-ARM path. Semihosting is an
ARM debug call; off target the line goes to stderr, which is not useful on a
workstation and is not meant to be. It is there so the file compiles.

## The Eclipse project

`make -C tests run` starts by running `project_files.py`, which compares the
files in `src/` and `include/` with the linked resources in
`nRF52840/ioc/.project`.

That project names every file individually, so a source added to the tree and
not added there is simply not compiled. Nothing in a host test can see it: the
Makefile finds the file by wildcard and builds it happily, while the target
build links without it and fails on an undefined reference that names a symbol
rather than a file. `src/hci_sdc_resources.cpp` went out that way once.

It needs Python 3 and is skipped with a message when there is none.

```sh
python3 tests/project_files.py
```

## The Python tooling against the dispatch table

`command_coverage.py` runs next. It compares the opcodes in
`src/hci_sdc_nrfxlib.cpp` with the entries in `hardware/hci_commands.py` and
fails on either direction: a command the firmware dispatches that nothing will
ever send at a radio, or an entry for a command that was removed.

That gap was real and large. The firmware grew from 60 opcodes to 126 over
this branch while the Python tooling drove 29, so most of what the host tests
pinned down had only ever met a compiled stub. Nothing said so, because the
two halves had no reason to look at each other.

The opcode values come from the nrfxlib headers, since the dispatch table
names them symbolically, so this needs a checkout and is skipped without one.

```sh
python3 tests/command_coverage.py /path/to/sdk-nrfxlib
```

## Hardware tools

`hardware/` holds four Python tools and the command table they share. They
need Python 3.8 or later and no packages beyond the standard library, except
`hci_test.py` and `hci_ble_test.py`, which talk to a serial port and need
pyserial.

`hci_commands.py` is the table: one entry per opcode the firmware dispatches,
with a parameter block that can be sent, what answer to expect, and what has
to exist first. It holds no return lengths, because the C++ dispatch test
already checks those against the vendor headers and a second copy in Python
would be a second copy to keep right.

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
python3 tests/hardware/hci_ble_test.py /dev/ttyACM0 probe
```

`probe` is the broad one, and it is what makes the 126 opcodes mean something
on a radio rather than in a compiled stub. It sends every command in
`hci_commands.py`, prints what came back, and puts the controller back where
it found it.

Three answers matter and they are not the same:

| Answer | Meaning |
| --- | --- |
| accepted | the command reached the link layer and it agreed |
| refused | a status other than success. Often correct: a controller answers Command Disallowed to something out of order and Unsupported Feature to something the library does not do on this part |
| Unknown HCI Command | the dispatch table does not carry the opcode, which contradicts what the host test said. This is the one that is always a defect |

Where a refusal is the right answer to a well formed parameter block, the row
says which statuses it expects and the run counts those as a pass. A refusal
the table did not predict means one of the two is wrong, and the table is the
better first suspect. Two runs against a dongle produced eighteen refusals
and then ten, and all twenty eight were mistakes in the table. The firmware
answered correctly every time.

What they were is worth knowing, because each is a way a host can be wrong
about a controller that is behaving:

- two parameter blocks a field short of what the vendor header declares
- a feature bit off by six, asking for Isochronous Channels (Host Support)
  while the comment claimed Connection Subrating
- an own address type naming a public address the board does not have, which
  the parameter commands accept and the enables refuse, so the failure lands
  one command late
- rows naming an advertising set before it was created or after it was gone
- two commands sent in the one order that cannot succeed: crediting packets
  with flow control off, and disabling a channel survey never enabled
- **legacy and extended advertising commands in the same session**

The last one accounted for ten by itself. A controller is in legacy
advertising mode or extended advertising mode, not both, and whichever set of
commands the host uses first, the other is Command Disallowed until reset.
Vol 4 Part E 3.1.1. So `probe` runs in three parts: the commands that work in
either mode, then the legacy ones, then a reset, then the extended ones. A
run that skips the reset gets Command Disallowed for everything extended,
which reads exactly like a controller missing half its commands.

Commands that need a link are skipped unless a handle is given, and commands
that change the identity of the board, leave the radio transmitting or end the
connection are skipped unless asked for:

```sh
python3 tests/hardware/hci_ble_test.py probe --handle 0x0040
python3 tests/hardware/hci_ble_test.py probe --consent
python3 tests/hardware/hci_ble_test.py probe --verbose   # say why each skip
```

The handle is checked once with a harmless read before anything uses it. A
handle with nothing behind it would otherwise turn two dozen rows into two
dozen Unknown Connection Identifier lines, which says nothing about any of
them. A row can need more than one thing at a time: Disconnect and the
encryption commands need consent **and** a live connection, and asking for
only one of the two sends them at a handle that is not there.

Some rows put the controller somewhere the next command cannot work from: a
direct test mode test that is still running, an initiator still scanning for
a peer. Those undo themselves immediately rather than at the end of the
phase, because a running test is Command Disallowed for the next test, for
advertising, and for scanning.

### Getting the commands that need a link

Twenty eight commands need a connection, and a dongle cannot connect to
itself. `--wait-connect` closes that with no second board: after the rest of
the run it advertises as `HCI-PROBE`, waits for a phone to tap connect, and
sends the connection scoped rows against the handle it gets. Disconnect runs
last whatever order the table is in, and the link is closed afterwards
either way.

```sh
python3 tests/hardware/hci_ble_test.py probe --consent --wait-connect 30
```

nRF Connect or LightBlue is enough on the phone side. Nothing has to be
paired and no service has to be discovered; the connection itself is what the
commands need.

Five commands are still out of reach from one board. They need a periodic
advertising sync, which needs a second radio transmitting a periodic train,
and `probe` says so rather than pretending to cover them.

### When the port disappears

`probe` reports this rather than raising, because on a dongle it is a
finding. MPSL and the SoftDevice Controller reset the chip from their assert
handlers by design, see `HciNrf52840MpslAssert` in `src/hci_nrf52840.cpp`, so
a controller fault takes the USB device with it. The board re-enumerates and
the next run starts clean, which is what makes it easy to blame on the cable.
The run names the command that was in flight and exits 3.

`--settle-ms` is how that gets narrowed down. Commands that put the radio to
work undo themselves immediately, and this is the pause before that undo. The
default is 100 ms. Zero ends a direct test mode test in the same millisecond
it was started, which is how the first controller reset was found:

```sh
python3 tests/hardware/hci_ble_test.py probe --consent --settle-ms 0
python3 tests/hardware/hci_ble_test.py probe --consent --settle-ms 500
```

A board that survives one and not the other says the fault is in the timing
of the teardown, not in either command.

`counters` reads the firmware's own tallies over the vendor specific opcode in
`hci_counters.h`. Nothing else puts those numbers on the wire, so without it a
running board can only be questioned with a debugger.

It also reports the two SoftDevice Controller memory figures, which are not
counters: what `sdc_cfg_set` asked for at startup and what the build reserved.
The pool is computed from the configuration in `include/hci_sdc_resources.h`,
but `sdc.h` says the memory macros may move between minor releases, so the
build time number can be right and the run time one larger. This is the only way to
see the headroom on a sealed dongle, and the way to watch it shrink across an
nrfxlib upgrade before a controller refuses to enable:

```
   SDC pool required                 38860
   SDC pool reserved                 39372
                                       512  headroom
```

A controller that reports zero for both has a platform layer that never filled
them in, which is not the same as one that wanted no memory.

`connect --flood N` sends N ACL packets while ignoring flow control and reports
which counters moved. It first says how many of the N reached
`sdc_hci_data_put` at all, because a block of refusal counters reading zero
looks the same whether the controller accepted everything or nothing ever
arrived. Once that is established it distinguishes the retry path firing, the
controller refusing with some other error, and the controller taking every
packet.

`sdc_symbols.py` reads a SoftDevice Controller archive and checks that every
SDC command `src/hci_sdc_nrfxlib.cpp` calls is defined in it. The firmware
links `libsoftdevice_controller_multirole` and only that, so no dispatch table
row is conditional; the question worth asking is whether an nrfxlib upgrade
dropped or renamed something the table needs. Without this that is a link error
naming one symbol, with no indication of how many others went with it. It
parses the archive symbol index directly and needs no cross toolchain.

Pointed at the peripheral-only library it reports 41 missing commands, which is
what a wrong archive looks like.

```sh
python3 tests/sdc_symbols.py
python3 tests/sdc_symbols.py /path/to/libsoftdevice_controller_multirole.a
```

`fake_controller.py` is a controller simulator on a pty. It answers every
opcode in `hci_commands.py` with the reply shape that table declares, Command
Complete or Command Status or nothing at all, and can simulate a connection,
so the tools above can be exercised and debugged with no board attached. It
does not reproduce return lengths, for the same reason the table does not
carry them.

```sh
python3 tests/hardware/fake_controller.py
python3 tests/hardware/fake_controller.py --script hci_ble_test.py --args=probe
```

Host tests do not replace hardware validation. They pin down the parts that can
be reasoned about off target, which is framing, dispatch and start up ordering.
Everything involving the radio, real USB enumeration or MPSL timing has to be
checked on a UDG-NRF52840x.
