# Host tests for HciController.
#
# Everything here is compiled with the native C++ compiler. The target headers
# that the firmware needs on device, nrf.h, MPSL, TinyUSB, TaktOS, IOsonata and
# the SoftDevice Controller, are replaced by the fakes under stubs, so no SDK
# checkout is needed for the default target.
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

CXX ?= c++
BUILD_DIR ?= $(THIS_DIR)/build

CXXFLAGS ?= -std=c++14 -O0 -g3 -Wall -Wextra -Werror -Wshadow -Wundef

# stubs/sdc holds the fake sdc.h. It comes first so it wins over a real
# nrfxlib checkout that may also be on the path, and it is left out of the
# dispatch test, which wants the real headers.
MOCK_INCLUDES := -I$(STUBS)/sdc -I$(STUBS) -I$(ROOT)/include
MOCK_DEFINES := -DHCI_TRACE=0 \
                -DHCI_NRF52840_HFCLK_WAIT_LOOPS=1000 \
                -DHCI_NRF52840_USBREG_WAIT_LOOPS=1000

# name : test source : firmware sources under test
TESTS := hci_h4 \
         hci_cmd_dispatch \
         hci_sdc \
         hci_intrf_transport \
         hci_controller \
         hci_nrf52840_usb \
         hci_taktos_poll \
         hci_tinyusb_init

hci_h4_SRC             := $(SRC)/hci_h4.cpp
hci_cmd_dispatch_SRC   := $(SRC)/hci_cmd_dispatch.cpp
hci_sdc_SRC            := $(SRC)/hci_sdc.cpp $(SRC)/hci_cmd_dispatch.cpp
hci_intrf_transport_SRC := $(SRC)/hci_intrf_transport.cpp $(SRC)/hci_h4.cpp
hci_controller_SRC     := $(SRC)/hci_controller.cpp \
                          $(SRC)/hci_intrf_transport.cpp $(SRC)/hci_h4.cpp
hci_nrf52840_usb_SRC   := $(SRC)/hci_nrf52840.cpp
hci_taktos_poll_SRC    := $(SRC)/hci_taktos.cpp
hci_tinyusb_init_SRC   := $(SRC)/hci_tinyusb.cpp

TARGETS := $(addprefix $(BUILD_DIR)/,$(addsuffix _test,$(TESTS)))

# The dispatch test compiles the real command table against the real
# SoftDevice Controller headers, with only the SDC entry points stubbed. It is
# built when those headers can be found and skipped otherwise.
NRFXLIB_DIR ?= $(ROOT)/../external/sdk-nrfxlib
SDC_INCLUDE := $(wildcard $(NRFXLIB_DIR)/softdevice_controller/include)

ifneq ($(strip $(SDC_INCLUDE)),)
SDC_TARGET := $(BUILD_DIR)/hci_sdc_dispatch_test
SDC_SOURCES := $(UNIT)/hci_sdc_dispatch_test.cpp \
               $(SRC)/hci_sdc_nrfxlib.cpp \
               $(SRC)/hci_sdc.cpp \
               $(SRC)/hci_cmd_dispatch.cpp \
               $(STUBS)/sdclink/sdc_stub.cpp
TARGETS += $(SDC_TARGET)
endif

.PHONY: all run clean check-root print-config

all: $(TARGETS)

check-root:
	@test -n "$(strip $(ROOT))" || { \
	  echo "HciController root not found from $(THIS_DIR)."; \
	  echo "Run: make ROOT=/path/to/HciController"; \
	  exit 1; }
	@test -f "$(SRC)/hci_h4.cpp" || { \
	  echo "Missing $(SRC)/hci_h4.cpp"; exit 1; }

.SECONDEXPANSION:
$(BUILD_DIR)/%_test: $(UNIT)/%_test.cpp $$($$*_SRC) | check-root
	@mkdir -p $(dir $@)
	$(CXX) $(MOCK_INCLUDES) $(MOCK_DEFINES) $(CXXFLAGS) $^ -o $@

ifneq ($(strip $(SDC_INCLUDE)),)
$(SDC_TARGET): $(SDC_SOURCES) | check-root
	@mkdir -p $(dir $@)
	$(CXX) -I$(STUBS)/sdclink -I$(STUBS) -I$(SDC_INCLUDE) -I$(ROOT)/include \
	  -DNRF52840_XXAA -DHCI_TRACE=0 $(CXXFLAGS) $(SDC_SOURCES) -o $@
endif

run: all
	@set -e; for t in $(TARGETS); do \
	  echo "[TEST] $$(basename $$t)"; $$t; done
	@echo
	@echo "All HciController host tests passed."
	@test -n "$(strip $(SDC_INCLUDE))" || \
	  echo "Skipped hci_sdc_dispatch_test, set NRFXLIB_DIR to a sdk-nrfxlib checkout."

print-config:
	@echo "ROOT=$(ROOT)"
	@echo "NRFXLIB_DIR=$(NRFXLIB_DIR)"
	@echo "SDC_INCLUDE=$(SDC_INCLUDE)"
	@echo "TARGETS=$(TARGETS)"

clean:
	rm -rf $(BUILD_DIR)
