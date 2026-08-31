PROJECT := sentry
BUILD_DIR := build
SRC_DIR := src
INCLUDE_DIR := include
LINKER_SCRIPT := linker/skr_mini_e3_v2.ld

LIBOPENCM3_DIR := lib/libopencm3
LIBOPENCM3_REV := d60d7802fd20821a766675545b6ef7a2207207a3
LIBOPENCM3_LIBRARY := $(LIBOPENCM3_DIR)/lib/libopencm3_stm32f1.a

PREFIX ?= arm-none-eabi
CC := $(PREFIX)-gcc
OBJCOPY := $(PREFIX)-objcopy
SIZE := $(PREFIX)-size
HOST_CC ?= cc
PYTHON ?= python

SOURCES := $(wildcard $(SRC_DIR)/*.c)
OBJECTS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SOURCES))
DEPENDENCIES := $(OBJECTS:.o=.d)

ARCH_FLAGS := -mthumb -mcpu=cortex-m3 -msoft-float -mfix-cortex-m3-ldrd
CPPFLAGS := -DSTM32F1 -I$(INCLUDE_DIR) -I$(LIBOPENCM3_DIR)/include
CFLAGS := $(ARCH_FLAGS) -std=c11 -Os -g3 -ffunction-sections -fdata-sections \
	-Wall -Wextra -Wshadow -Wconversion -Wundef -Werror
LDFLAGS := $(ARCH_FLAGS) -nostartfiles -static \
	-L$(LIBOPENCM3_DIR)/lib \
	-T$(LINKER_SCRIPT) -Wl,-Map=$(BUILD_DIR)/$(PROJECT).map \
	-Wl,--gc-sections -Wl,--fatal-warnings
LDLIBS := -Wl,--start-group -lopencm3_stm32f1 -lc -lgcc -lnosys -Wl,--end-group

.PHONY: all clean deps check-tools verify-libopencm3 test camera-test

all: check-tools $(BUILD_DIR)/$(PROJECT).bin firmware.bin

check-tools:
	@command -v $(CC) >/dev/null 2>&1 || { echo "error: $(CC) not found in PATH"; exit 1; }
	@command -v $(OBJCOPY) >/dev/null 2>&1 || { echo "error: $(OBJCOPY) not found in PATH"; exit 1; }
	@command -v git >/dev/null 2>&1 || { echo "error: git not found in PATH"; exit 1; }
	@command -v $(PYTHON) >/dev/null 2>&1 || { echo "error: $(PYTHON) not found in PATH"; exit 1; }

deps: verify-libopencm3 $(LIBOPENCM3_LIBRARY)

$(LIBOPENCM3_DIR)/.git:
	@mkdir -p lib
	git clone --branch v0.8.0 --depth 1 https://github.com/libopencm3/libopencm3.git $(LIBOPENCM3_DIR)

verify-libopencm3: $(LIBOPENCM3_DIR)/.git
	@actual=$$(git -C $(LIBOPENCM3_DIR) rev-parse HEAD); \
	if [ "$$actual" != "$(LIBOPENCM3_REV)" ]; then \
		echo "error: libopencm3 is $$actual, expected $(LIBOPENCM3_REV)"; \
		exit 1; \
	fi

$(LIBOPENCM3_LIBRARY): verify-libopencm3
	cd $(LIBOPENCM3_DIR) && $(PYTHON) scripts/irq2nvic_h \
		./include/libopencm3/stm32/f1/irq.json
	$(MAKE) -C $(LIBOPENCM3_DIR) TARGETS=stm32/f1

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

# libopencm3 generates target headers (including STM32F1 NVIC definitions),
# so a pristine build must finish the pinned dependency before compiling us.
$(OBJECTS): | $(LIBOPENCM3_LIBRARY)

$(BUILD_DIR)/$(PROJECT).elf: $(OBJECTS) $(LIBOPENCM3_LIBRARY) $(LINKER_SCRIPT)
	$(CC) $(OBJECTS) $(LDFLAGS) $(LDLIBS) -o $@
	$(SIZE) $@

$(BUILD_DIR)/$(PROJECT).bin: $(BUILD_DIR)/$(PROJECT).elf
	$(OBJCOPY) -O binary $< $@

firmware.bin: $(BUILD_DIR)/$(PROJECT).elf
	$(OBJCOPY) -O binary $< $@

test: tests/protocol_test tests/motion_test tests/board_mapping_test \
		tests/tmc2209_test tests/driver_control_test \
		tests/tmc_uart_diagnostics_test tests/tilt_reference_test
	./tests/protocol_test
	./tests/motion_test
	./tests/board_mapping_test
	./tests/tmc2209_test
	./tests/driver_control_test
	./tests/tmc_uart_diagnostics_test
	./tests/tilt_reference_test
	$(PYTHON) tests/tmc_uart_transport_test.py
	$(PYTHON) tests/m4_hardware_structure_test.py
	$(PYTHON) tests/host_test.py
	$(PYTHON) tests/camera_storage_test.py
	$(PYTHON) tests/camera_service_test.py
	$(PYTHON) tests/camera_http_test.py

camera-test:
	$(PYTHON) tests/camera_storage_test.py
	$(PYTHON) tests/camera_service_test.py
	$(PYTHON) tests/camera_http_test.py

tests/protocol_test: tests/protocol_test.c src/protocol.c src/tmc2209.c \
		src/tmc_uart_diagnostics.c \
		include/protocol.h include/motion_controller.h include/motion.h \
		include/tilt_reference.h include/board.h \
		include/driver_control.h include/tmc2209.h
	$(HOST_CC) -std=c11 -O2 -Iinclude -Wall -Wextra -Wshadow -Wconversion \
		-Wundef -Werror tests/protocol_test.c src/protocol.c src/tmc2209.c \
		src/tmc_uart_diagnostics.c -o $@

tests/motion_test: tests/motion_test.c src/motion.c include/motion.h
	$(HOST_CC) -std=c11 -O2 -Iinclude -Wall -Wextra -Wshadow -Wconversion \
		-Wundef -Werror tests/motion_test.c src/motion.c -o $@

tests/tilt_reference_test: tests/tilt_reference_test.c src/tilt_reference.c \
		src/motion.c include/tilt_reference.h include/motion.h
	$(HOST_CC) -std=c11 -O2 -Iinclude -Wall -Wextra -Wshadow -Wconversion \
		-Wundef -Werror tests/tilt_reference_test.c src/tilt_reference.c \
		src/motion.c -o $@

tests/board_mapping_test: tests/board_mapping_test.c include/board.h \
		include/tmc2209.h include/tmc_uart.h
	$(HOST_CC) -std=c11 -O2 -Iinclude -Wall -Wextra -Wshadow -Wconversion \
		-Wundef -Werror tests/board_mapping_test.c -o $@

tests/tmc2209_test: tests/tmc2209_test.c src/tmc2209.c include/tmc2209.h
	$(HOST_CC) -std=c11 -O2 -Iinclude -Wall -Wextra -Wshadow -Wconversion \
		-Wundef -Werror tests/tmc2209_test.c src/tmc2209.c -o $@

tests/driver_control_test: tests/driver_control_test.c src/driver_control.c \
		src/tmc_uart_diagnostics.c \
		include/driver_control.h include/tmc2209.h include/tmc_uart.h \
		include/board.h
	$(HOST_CC) -std=c11 -O2 -Iinclude -Wall -Wextra -Wshadow -Wconversion \
		-Wundef -Werror tests/driver_control_test.c src/driver_control.c \
		src/tmc_uart_diagnostics.c -o $@

tests/tmc_uart_diagnostics_test: tests/tmc_uart_diagnostics_test.c \
		src/tmc_uart_diagnostics.c include/tmc_uart_diagnostics.h \
		include/tmc2209.h
	$(HOST_CC) -std=c11 -O2 -Iinclude -Wall -Wextra -Wshadow -Wconversion \
		-Wundef -Werror tests/tmc_uart_diagnostics_test.c \
		src/tmc_uart_diagnostics.c -o $@

clean:
	rm -rf $(BUILD_DIR) firmware.bin tests/protocol_test tests/protocol_test.exe \
		tests/motion_test tests/motion_test.exe tests/board_mapping_test \
		tests/board_mapping_test.exe tests/tmc2209_test \
		tests/tmc2209_test.exe tests/driver_control_test \
		tests/driver_control_test.exe tests/tmc_uart_diagnostics_test \
		tests/tmc_uart_diagnostics_test.exe tests/tilt_reference_test \
		tests/tilt_reference_test.exe

-include $(DEPENDENCIES)
