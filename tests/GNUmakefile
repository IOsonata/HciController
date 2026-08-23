# Native USB additions layered over the existing host-test Makefile.
# GNU make loads GNUmakefile before Makefile, so `make -C tests run` keeps the
# existing suite and adds the native HCI USB targets without duplicating it.
#
# GCC/Clang versions released before the final C++23 option spelling use
# `gnu++2b` for the same language generation. Require C++23, but accept either
# compiler spelling so the host release gate does not require a compiler update
# only for the renamed -std option.
CXX ?= c++
HCI_CXX23_STD := $(shell \
	if printf '' | $(CXX) -x c++ -std=gnu++23 -fsyntax-only - >/dev/null 2>&1; then \
		printf '%s' 'gnu++23'; \
	elif printf '' | $(CXX) -x c++ -std=gnu++2b -fsyntax-only - >/dev/null 2>&1; then \
		printf '%s' 'gnu++2b'; \
	fi)

ifeq ($(strip $(HCI_CXX23_STD)),)
$(error $(CXX) does not support GNU C++23 (-std=gnu++23 or -std=gnu++2b))
endif

CXXFLAGS ?= -std=$(HCI_CXX23_STD) -O0 -g3 -Wall -Wextra -Werror -Wshadow -Wundef

include Makefile

USB_DESC_OBJ := $(BUILD_DIR)/usb_descriptors_test.o

TESTS += hci_usb hci_usb_modeswitch hci_controller_invalid_host \
         hci_controller_local_commands
hci_usb_SRC := $(SRC)/hci_usb.cpp \
               $(SRC)/hci_usb_rx.cpp \
               $(SRC)/hci_usb_tinyusb.cpp \
               $(USB_DESC_OBJ) \
               $(CFIFO_OBJ)
hci_usb_modeswitch_SRC := $(hci_usb_SRC)
hci_controller_invalid_host_SRC := $(SRC)/hci_controller.cpp \
                                   $(SRC)/hci_intrf_transport.cpp \
                                   $(SRC)/hci_h4.cpp
hci_controller_local_commands_SRC := $(SRC)/hci_controller.cpp \
                                     $(SRC)/hci_intrf_transport.cpp \
                                     $(SRC)/hci_h4.cpp
TARGETS += $(BUILD_DIR)/hci_usb_test \
           $(BUILD_DIR)/hci_usb_modeswitch_test \
           $(BUILD_DIR)/hci_controller_invalid_host_test \
           $(BUILD_DIR)/hci_controller_local_commands_test

$(USB_DESC_OBJ): $(SRC)/usb_descriptors.c $(ROOT)/include/hci_usb.h \
                 $(STUBS)/tusb.h $(STUBS)/nrfficr/nrf.h | check-root
	@mkdir -p $(dir $@)
	$(CC) -I$(STUBS)/nrfficr $(MOCK_INCLUDES) $(MOCK_DEFINES) $(CFLAGS) \
	  -c $< -o $@

.PHONY: check-hci-transport check-native-usb-review check-counter-schema \
        check-harness-layout check-mode-policy

check-hci-transport:
	@command -v $(PYTHON) >/dev/null 2>&1 || { \
	  echo "[TEST] hci_transport_test.py skipped, no $(PYTHON)"; exit 0; }
	@echo "[TEST] hci_transport_test.py"
	@PYTHONPATH=$(THIS_DIR)/harness/lib $(PYTHON) \
	  $(THIS_DIR)/harness/hcicontroller/hci_transport_test.py

check-native-usb-review:
	@command -v $(PYTHON) >/dev/null 2>&1 || { \
	  echo "[TEST] native_usb_review_regressions.py skipped, no $(PYTHON)"; exit 0; }
	@echo "[TEST] native_usb_review_regressions.py"
	@$(PYTHON) $(THIS_DIR)/native_usb_review_regressions.py $(ROOT)

check-counter-schema:
	@command -v $(PYTHON) >/dev/null 2>&1 || { \
	  echo "[TEST] counter_schema.py skipped, no $(PYTHON)"; exit 0; }
	@echo "[TEST] counter_schema.py"
	@$(PYTHON) $(THIS_DIR)/counter_schema.py $(ROOT)

check-harness-layout:
	@command -v $(PYTHON) >/dev/null 2>&1 || { \
	  echo "[TEST] harness_layout.py skipped, no $(PYTHON)"; exit 0; }
	@echo "[TEST] harness_layout.py"
	@$(PYTHON) $(THIS_DIR)/harness_layout.py $(ROOT)

check-mode-policy:
	@command -v $(PYTHON) >/dev/null 2>&1 || { \
	  echo "[TEST] mode_policy.py skipped, no $(PYTHON)"; exit 0; }
	@echo "[TEST] mode_policy.py"
	@$(PYTHON) $(THIS_DIR)/mode_policy.py $(ROOT)

all: $(BUILD_DIR)/hci_usb_test $(BUILD_DIR)/hci_usb_modeswitch_test \
     $(BUILD_DIR)/hci_controller_invalid_host_test \
     $(BUILD_DIR)/hci_controller_local_commands_test
run: check-hci-transport check-native-usb-review check-counter-schema \
     check-harness-layout check-mode-policy
