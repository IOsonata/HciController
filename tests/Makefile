# Host tests for HciController.
#
# Everything here is compiled with the native C/C++ compiler using the same
# GNU C17 and GNU C++23 language standards as the nRF52840 target project. The
# target headers that the firmware needs on device, nrf.h, MPSL, TinyUSB,
# TaktOS and the SoftDevice Controller, are replaced by the fakes under stubs,
# so no SDK checkout is needed for the default target.
#
# CFifo is mirrored under tests/iosonata so the transport tests stay
# self-contained. SysLog is an IOsonata library service and the focused trace
# integration test builds the sibling IOsonata sources that the target links.
#
#   make -C tests run                  build and run everything available
#   make -C tests run NRFXLIB_DIR=...  add the SDC dispatch test
#   make -C tests clean
#
# Works wherever this file sits, as long as it stays inside the repository.
# Override with: make ROOT=/path/to/HciController

THIS_DIR := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

# Walk up from this file until include/hci_h4.h shows up.
ROOT_CANDIDATES := $(THIS_DIR)/.. $(THIS_DIR) $(THIS_DIR)/../.. $(THIS_DIR)/../../..
ROOT ?= $(firstword $(foreach d,$(ROOT_CANDIDATES),\
          $(if $(wildcard $(d)/include/hci_h4.h),$(abspath $(d)))))

SRC := $(ROOT)/src
UNIT := $(THIS_DIR)/unit
STUBS := $(THIS_DIR)/stubs

# The mirrored CFifo is the same implementation used by the target and has no
# hardware dependency.
IOSONATA := $(THIS_DIR)/iosonata
IOSONATA_ROOT ?= $(ROOT)/../IOsonata

CC ?= cc
CXX ?= c++
BUILD_DIR ?= $(THIS_DIR)/build

# Built as C, with the C compiler, and linked in as an object. Compiling a .c
# through the C++ driver is deprecated and would fail the -Werror build on
# some toolchains.
CFIFO_OBJ := $(BUILD_DIR)/iosonata/cfifo.o

CXXFLAGS ?= -std=gnu++23 -O0 -g3 -Wall -Wextra -Werror -Wshadow -Wundef
CFLAGS ?= -std=gnu17 -O0 -g3 -Wall -Wextra -Werror -Wshadow -Wundef

# stubs/sdc holds the fake sdc.h. It comes first so it wins over a real
# nrfxlib checkout that may also be on the path, and it is left out of the
# dispatch test, which wants the real headers.
MOCK_INCLUDES := -I$(STUBS)/sdc -I$(STUBS) -I$(IOSONATA) -I$(ROOT)/include \
                 -I$(IOSONATA_ROOT)/include
# CFG_TUSB_MCU is what the firmware build passes and what tusb_config.h refuses
# to be included without. The stub tusb.h includes the real config so a test
# sees the real function count rather than a copy of it that can drift.
MOCK_DEFINES := -DHCI_NRF52840_HFCLK_WAIT_LOOPS=1000 \
                -DHCI_NRF52840_USBREG_WAIT_LOOPS=1000 \
                -DCFG_TUSB_MCU=OPT_MCU_NRF5X

# name : test source : firmware sources under test
TESTS := hci_h4 \
         hci_cmd_dispatch \
         hci_sdc \
         hci_sdc_critical \
         hci_intrf_transport \
         hci_intrf_transport_integrity \
         hci_controller \
         hci_nrf52840_usb \
         hci_taktos_poll \
         hci_tinyusb_init

hci_h4_SRC             := $(SRC)/hci_h4.cpp
hci_cmd_dispatch_SRC   := $(SRC)/hci_cmd_dispatch.cpp
hci_sdc_SRC            := $(SRC)/hci_sdc.cpp $(SRC)/hci_cmd_dispatch.cpp
hci_sdc_critical_SRC   := $(SRC)/hci_sdc.cpp $(SRC)/hci_cmd_dispatch.cpp
hci_intrf_transport_SRC := $(SRC)/hci_intrf_transport.cpp $(SRC)/hci_h4.cpp
hci_intrf_transport_integrity_SRC := $(SRC)/hci_intrf_transport.cpp $(SRC)/hci_h4.cpp
hci_controller_SRC     := $(SRC)/hci_controller.cpp \
                          $(SRC)/hci_intrf_transport.cpp $(SRC)/hci_h4.cpp
hci_nrf52840_usb_SRC   := $(SRC)/hci_nrf52840.cpp \
                          $(SRC)/hci_sdc_resources.cpp
hci_taktos_poll_SRC    := $(SRC)/hci_taktos.cpp
hci_tinyusb_init_SRC   := $(SRC)/hci_tinyusb.cpp $(CFIFO_OBJ)

# Tests unrelated to logging only need trace call sites to link. The focused
# HciTrace target below links the real standard IOsonata SysLog implementation.
COMMON_SRC             := $(STUBS)/hci_trace_stub.cpp

TARGETS := $(addprefix $(BUILD_DIR)/,$(addsuffix _test,$(TESTS)))

HCI_TRACE_TARGET := $(BUILD_DIR)/hci_trace_test
HCI_TRACE_SOURCES := $(UNIT)/hci_trace_test.cpp \
                     $(SRC)/hci_trace.cpp \
                     $(IOSONATA_ROOT)/src/syslog.cpp \
                     $(IOSONATA_ROOT)/src/device_intrf.cpp \
                     $(CFIFO_OBJ)
TARGETS += $(HCI_TRACE_TARGET)

# Compiled but not linked, and not run.
#
# Two gaps this closes. hci_app.cpp is the largest source here and no test
# linked it, so a structure member that moved under it was only found by an
# arm-none-eabi build. And every test builds with HCI_TRACE=0, where a trace
# implementation may be optimized differently, so compile every source once
# with semihosting enabled as well.
#
# Linking hci_app would need MPSL, TinyUSB and a controller all answering,
# which is a target's job; compiling it catches structure and API drift.
COMPILE_SRC := $(filter-out $(SRC)/hci_sdc_nrfxlib.cpp,$(wildcard $(SRC)/hci_*.cpp))

OBJECTS := $(patsubst $(SRC)/%.cpp,$(BUILD_DIR)/trace/%.o,$(COMPILE_SRC)) \
           $(BUILD_DIR)/notrace/hci_app.o \
           $(BUILD_DIR)/logport/hci_app.o

# The dispatch test compiles the real command table against the real
# SoftDevice Controller headers, with only the SDC entry points stubbed. It is
# built when those headers can be found and skipped otherwise.
NRFXLIB_DIR ?= $(ROOT)/../external/sdk-nrfxlib
SDC_INCLUDE := $(wildcard $(NRFXLIB_DIR)/softdevice_controller/include)
SDC_MULTIROLE_LIB := $(NRFXLIB_DIR)/softdevice_controller/lib/nrf52/hard-float/libsoftdevice_controller_multirole.a

ifneq ($(strip $(SDC_INCLUDE)),)
SDC_TARGET := $(BUILD_DIR)/hci_sdc_dispatch_test
SDC_SOURCES := $(UNIT)/hci_sdc_dispatch_test.cpp \
               $(SRC)/hci_sdc_nrfxlib.cpp \
               $(SRC)/hci_sdc.cpp \
               $(SRC)/hci_counters.cpp \
               $(SRC)/hci_cmd_dispatch.cpp \
               $(STUBS)/sdclink/sdc_stub.cpp \
               $(STUBS)/sdclink/sdc_stub_page2.cpp
TARGETS += $(SDC_TARGET)

# Focused real-header regressions for command shapes the table-wide dispatch
# walk cannot exercise: flexible CIG returns and Read Buffer Size V2 side
# effects on the generic ACL flow-control layer.
SDC_CRITICAL_TARGET := $(BUILD_DIR)/hci_sdc_critical_dispatch_test
SDC_CRITICAL_SOURCES := $(UNIT)/hci_sdc_critical_dispatch_test.cpp \
                        $(SRC)/hci_sdc_nrfxlib.cpp \
                        $(SRC)/hci_sdc.cpp \
                        $(SRC)/hci_counters.cpp \
                        $(SRC)/hci_cmd_dispatch.cpp \
                        $(STUBS)/sdclink/sdc_stub.cpp \
                        $(STUBS)/sdclink/sdc_stub_page2.cpp
TARGETS += $(SDC_CRITICAL_TARGET)

# The resource configuration against the real headers. Everything else that
# includes hci_nrf52840.h is built against the hand written fake sdc.h, so the
# memory macros and the sdc_cfg_t members it copies have never been compared
# with the vendor ones. This is the only thing that does that, and it needs no
# firmware sources: the header and the real sdc.h are the whole subject.
RESOURCES_TARGET := $(BUILD_DIR)/hci_sdc_resources_test
TARGETS += $(RESOURCES_TARGET)
endif

.PHONY: all run clean check-root check-project check-boards \
        check-usb-desc check-usb-runtime check-commands check-sdc-symbols \
        check-smp check-events check-cis-cleanup print-config

PYTHON ?= python3

all: $(TARGETS) $(OBJECTS)

check-root:
	@test -n "$(strip $(ROOT))" || { \
	  echo "HciController root not found from $(THIS_DIR)."; \
	  echo "Run: make ROOT=/path/to/HciController"; \
	  exit 1; }
	@test -f "$(SRC)/hci_h4.cpp" || { \
	  echo "Missing $(SRC)/hci_h4.cpp"; exit 1; }
	@test -f "$(IOSONATA_ROOT)/src/syslog.cpp" || { \
	  echo "Missing IOsonata SysLog at $(IOSONATA_ROOT). Set IOSONATA_ROOT."; exit 1; }

# Every header, as a prerequisite of everything.
#
# The rules below name only .cpp files, so a change to a header rebuilt
# nothing and whatever ran was built from the header as it stood before the
# edit. That is worse than a stale build, because it reports pass.
HEADERS := $(wildcard $(ROOT)/include/*.h) $(wildcard $(STUBS)/*.h) \
           $(wildcard $(STUBS)/sdc/*.h) $(wildcard $(STUBS)/sdclink/*.h) \
           $(wildcard $(STUBS)/nrfficr/*.h) $(wildcard $(IOSONATA)/*.h) \
           $(IOSONATA_ROOT)/include/syslog.h $(IOSONATA_ROOT)/include/cfifo.h

.SECONDEXPANSION:
$(BUILD_DIR)/%_test: $(UNIT)/%_test.cpp $$($$*_SRC) $(COMMON_SRC) $(HEADERS) \
                     | check-root
	@mkdir -p $(dir $@)
	$(CXX) $(MOCK_INCLUDES) $(MOCK_DEFINES) -DHCI_TRACE=0 $(CXXFLAGS) \
	  $(filter %.cpp %.o,$^) -o $@

$(HCI_TRACE_TARGET): $(HCI_TRACE_SOURCES) $(HEADERS) | check-root
	@mkdir -p $(dir $@)
	$(CXX) -I$(IOSONATA_ROOT)/include -I$(STUBS) -I$(IOSONATA) -I$(ROOT)/include \
	  $(MOCK_DEFINES) -DHCI_TRACE=0 $(CXXFLAGS) \
	  $(filter %.cpp %.o,$^) -o $@

$(BUILD_DIR)/iosonata/%.o: $(IOSONATA)/%.c $(wildcard $(IOSONATA)/*.h) \
                           | check-root
	@mkdir -p $(dir $@)
	$(CC) -I$(IOSONATA) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/trace/%.o: $(SRC)/%.cpp $(HEADERS) | check-root
	@mkdir -p $(dir $@)
	$(CXX) $(MOCK_INCLUDES) $(MOCK_DEFINES) -DHCI_TRACE=1 $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/notrace/%.o: $(SRC)/%.cpp $(HEADERS) | check-root
	@mkdir -p $(dir $@)
	$(CXX) $(MOCK_INCLUDES) $(MOCK_DEFINES) -DHCI_TRACE=0 $(CXXFLAGS) -c $< -o $@

# The same file again with the board saying the USB socket is its own, which
# is the only way the log port path is compiled by anything. The stub board.h
# does not say so, and the boards that do are built by arm-none-eabi and by
# nothing here, so without this the branch the Thingy:91 runs is checked by no
# host build at all.
$(BUILD_DIR)/logport/%.o: $(SRC)/%.cpp $(HEADERS) | check-root
	@mkdir -p $(dir $@)
	$(CXX) $(MOCK_INCLUDES) $(MOCK_DEFINES) -DHCI_TRACE=1 -DHCI_USB_SOCKET=1 \
	  $(CXXFLAGS) -c $< -o $@

ifneq ($(strip $(SDC_INCLUDE)),)
$(SDC_TARGET): $(SDC_SOURCES) | check-root
	@mkdir -p $(dir $@)
	$(CXX) -I$(STUBS)/sdclink -I$(STUBS) -I$(SDC_INCLUDE) -I$(ROOT)/include \
	  -DNRF52840_XXAA -DHCI_TRACE=0 $(CXXFLAGS) $(SDC_SOURCES) -o $@

$(SDC_CRITICAL_TARGET): $(SDC_CRITICAL_SOURCES) | check-root
	@mkdir -p $(dir $@)
	$(CXX) -I$(STUBS)/sdclink -I$(STUBS) -I$(SDC_INCLUDE) -I$(ROOT)/include \
	  -DNRF52840_XXAA -DHCI_TRACE=0 $(CXXFLAGS) $(SDC_CRITICAL_SOURCES) -o $@

# No stubs/sdc on the path, so the real sdc.h is the one that answers.
$(RESOURCES_TARGET): $(UNIT)/hci_sdc_resources_test.cpp \
                     $(ROOT)/include/hci_sdc_resources.h | check-root
	@mkdir -p $(dir $@)
	$(CXX) -I$(STUBS) -I$(SDC_INCLUDE) -I$(ROOT)/include \
	  -DNRF52840_XXAA -DHCI_TRACE=0 $(CXXFLAGS) \
	  $(UNIT)/hci_sdc_resources_test.cpp -o $@
endif

# The USB descriptors against the real TinyUSB headers. src/usb_descriptors.c
# is compiled by the target build and by nothing else, and the thing most
# likely to be wrong in it, the declared configuration length against the
# bytes that follow, is not an error until a host refuses to enumerate. The
# file has a _Static_assert for exactly that; this is what runs it.
#
# Built when a TinyUSB checkout can be found and skipped otherwise, the same
# way the SDC tests are.
TINYUSB_DIR ?= $(ROOT)/../external/tinyusb
TINYUSB_SRC := $(wildcard $(TINYUSB_DIR)/src/tusb.h)

check-usb-desc:
ifneq ($(strip $(TINYUSB_SRC)),)
	@echo "[TEST] usb_descriptors.c against TinyUSB"
	@$(CC) -fsyntax-only -I$(STUBS)/nrfficr -I$(ROOT)/include \
	  -I$(TINYUSB_DIR)/src -I$(STUBS) -DCFG_TUSB_MCU=OPT_MCU_NRF5X \
	  -std=gnu17 -Wall -Wextra -Werror $(SRC)/usb_descriptors.c
	@echo "[ok] descriptor length matches what it declares"
else
	@echo "[TEST] usb_descriptors.c skipped, set TINYUSB_DIR to a checkout"
endif

# hci_app.cpp is compile checked but not linked by the host suite. This pins
# the cross-layer order that defines one USB storm-counting window. In 70088f5
# the runtime path omitted UsbPassMark, so ordinary traffic accumulated forever
# against the startup mark and disconnected once it crossed the storm limit.
check-usb-runtime:
	@command -v $(PYTHON) >/dev/null 2>&1 || { \
	  echo "[TEST] usb_runtime_pass.py skipped, no $(PYTHON)"; exit 0; }
	@echo "[TEST] usb_runtime_pass.py"
	@$(PYTHON) $(THIS_DIR)/usb_runtime_pass.py $(ROOT)

# The Eclipse project names every file individually, so one added to src/ and
# not added there is not compiled, and the target build fails at link time
# naming a symbol rather than a file. Nothing a C++ test can see.
check-project:
	@command -v $(PYTHON) >/dev/null 2>&1 || { \
	  echo "[TEST] project_files.py skipped, no $(PYTHON)"; exit 0; }
	@echo "[TEST] project_files.py"
	@$(PYTHON) $(THIS_DIR)/project_files.py $(ROOT)

# The Python tooling against the dispatch table. For most of this branch the
# firmware answered 126 opcodes and the tooling drove 29, and nothing said so.
check-commands:
	@command -v $(PYTHON) >/dev/null 2>&1 || { \
	  echo "[TEST] command_coverage.py skipped, no $(PYTHON)"; exit 0; }
	@echo "[TEST] command_coverage.py"
	@NRFXLIB_DIR=$(NRFXLIB_DIR) $(PYTHON) $(THIS_DIR)/command_coverage.py

# The dispatch table is compiled against the real headers above. This checks
# the other half: every sdc_hci_cmd_* function it calls must still exist in the
# nRF52 multirole archive from the sdk-nrfxlib HEAD this build uses.
check-sdc-symbols:
	@command -v $(PYTHON) >/dev/null 2>&1 || { \
	  echo "[TEST] sdc_symbols.py skipped, no $(PYTHON)"; exit 0; }
	@if test -f "$(SDC_MULTIROLE_LIB)"; then \
	  echo "[TEST] sdc_symbols.py"; \
	  $(PYTHON) $(THIS_DIR)/sdc_symbols.py "$(SDC_MULTIROLE_LIB)"; \
	else \
	  echo "[TEST] sdc_symbols.py skipped, no nRF52 multirole archive at $(SDC_MULTIROLE_LIB)"; \
	fi

# The pairing crypto against the specification's worked examples. Every
# field in c1 and s1 is on the wire least significant octet first and the
# specification writes them the other way round, so an implementation with
# the order reversed produces values of the right length that are useless,
# and the only symptom is a pairing that does not complete on a bench. s1
# was written the wrong way round and this is what caught it.
check-smp:
	@command -v $(PYTHON) >/dev/null 2>&1 || { \
	  echo "[TEST] smp_vectors.py skipped, no $(PYTHON)"; exit 0; }
	@echo "[TEST] smp_vectors.py"
	@$(PYTHON) $(THIS_DIR)/smp_vectors.py

# The real board.h is compiled by the target build and by nothing else, so
# every pin number in it has only ever been checked by an ARM build for one
# board. That is how the dongle came to hold another board's pin map.
check-boards:
	@command -v $(PYTHON) >/dev/null 2>&1 || { \
	  echo "[TEST] board_pins.py skipped, no $(PYTHON)"; exit 0; }
	@echo "[TEST] board_pins.py"
	@$(PYTHON) $(THIS_DIR)/board_pins.py $(ROOT)

# The connection complete parser is shared by the radio probe and the
# two-controller CIS harness. Replay captured event bodies without a board.
check-events:
	@command -v $(PYTHON) >/dev/null 2>&1 || { \
	  echo "[TEST] hci_connection_event_test.py skipped, no $(PYTHON)"; exit 0; }
	@echo "[TEST] hci_connection_event_test.py"
	@PYTHONPATH=$(THIS_DIR)/harness/lib $(PYTHON) \
	  $(THIS_DIR)/harness/hcicontroller/hci_connection_event_test.py

# A two controller run ended with LE Set Host Feature refused on both dongles
# and the test printing PASS regardless, because teardown swallowed the status.
# Drive the teardown against a recorded controller so ordering stays host-testable.
check-cis-cleanup:
	@command -v $(PYTHON) >/dev/null 2>&1 || { \
	  echo "[TEST] hci_cis_cleanup_test.py skipped, no $(PYTHON)"; exit 0; }
	@echo "[TEST] hci_cis_cleanup_test.py"
	@PYTHONPATH=$(THIS_DIR)/harness/lib $(PYTHON) \
	  $(THIS_DIR)/harness/hcicontroller/hci_cis_cleanup_test.py

run: all check-project check-boards check-usb-desc check-usb-runtime \
     check-commands check-sdc-symbols check-smp check-events check-cis-cleanup
	@set -e; for t in $(TARGETS); do \
	  echo "[TEST] $$(basename $$t)"; $$t; done
	@echo
	@echo "All HciController host tests passed."
	@test -n "$(strip $(SDC_INCLUDE))" || \
	  echo "Skipped hci_sdc_dispatch_test, hci_sdc_critical_dispatch_test and \
hci_sdc_resources_test, set NRFXLIB_DIR to a sdk-nrfxlib checkout."

print-config:
	@echo "ROOT=$(ROOT)"
	@echo "IOSONATA_ROOT=$(IOSONATA_ROOT)"
	@echo "NRFXLIB_DIR=$(NRFXLIB_DIR)"
	@echo "SDC_INCLUDE=$(SDC_INCLUDE)"
	@echo "TARGETS=$(TARGETS)"

clean:
	rm -rf $(BUILD_DIR)
