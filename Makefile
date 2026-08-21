SHELL := /bin/bash

# ESP-IDF installed via Espressif Installation Manager (eim).
# `export.sh` looks for a different Python venv and will fail here.
IDF_ACTIVATE ?= $(HOME)/.espressif/tools/activate_idf_v6.0.2.sh

# Serial port. Empty = auto-detect Espressif USB-Serial/JTAG (VID 0x303A).
# Override: make flash PORT=/dev/cu.usbmodem2101
PORT ?=

.PHONY: all help check check-env ports build flash app-flash monitor run clean menuconfig reset erase-nvs

all: build

help:
	@echo "Targets:"
	@echo "  make check       verify IDF toolchain + look for the USB board"
	@echo "  make check-env   verify IDF toolchain only"
	@echo "  make ports       list USB serial ports (Espressif VID 0x303A first)"
	@echo "  make build       compile firmware"
	@echo "  make flash       build + flash bootloader, table, app (NVS/BLE bonds kept)"
	@echo "  make app-flash   flash app only; keeps NVS (BLE bonding keys)"
	@echo "  make erase-nvs   wipe NVS (forget BLE pairing)"
	@echo "  make monitor     serial monitor"
	@echo "  make run         flash, then monitor"
	@echo "  make menuconfig  idf.py menuconfig"
	@echo "  make clean       idf.py fullclean"
	@echo "  make reset       hard-reset the chip"
	@echo
	@echo "PORT=$(if $(strip $(PORT)),$(PORT),<auto>)"

# Load EIM env into the current recipe shell. PATH from -e is IDF tools only,
# so it is prepended to the existing PATH.
define load_idf
	if [ ! -f "$(IDF_ACTIVATE)" ]; then \
		echo "FAIL  ESP-IDF activate script not found: $(IDF_ACTIVATE)" >&2; \
		echo "      Install ESP-IDF 6.0.x with eim (Espressif Installation Manager)." >&2; \
		exit 1; \
	fi; \
	eval "$$( "$(IDF_ACTIVATE)" -e | awk -F= ' \
		$$1 == "SYSTEM_PATH" { next } \
		$$1 == "PATH" { print "export PATH=\"" substr($$0, 6) ":$$PATH\""; next } \
		NF >= 2 { \
			key = $$1; \
			val = substr($$0, index($$0, "=") + 1); \
			gsub(/"/, "\\\"", val); \
			print "export " key "=\"" val "\""; \
		} \
	' )"
endef

# Candidate USB serial ports. Prints a `PORT=...` line for the preferred device
# (Espressif VID 0x303A first). Exported so recipes can run: python -c "$$FIND_PORT_PY"
define FIND_PORT_PY
import serial.tools.list_ports as lp
vids = {0x303A: "Espressif", 0x1A86: "WCH CH340/CH9102", 0x10C4: "Silicon Labs CP210x", 0x0403: "FTDI"}
needles = ("usbmodem", "usbserial", "wchusbserial", "slab_usbtouart")
cands = []
print("USB serial ports:")
for x in lp.comports():
    name = x.device or ""
    vid = x.vid or 0
    hit = vid in vids or any(s in name.lower() for s in needles)
    if not hit:
        continue
    chip = vids.get(vid, "USB serial")
    mfg = x.manufacturer or ""
    desc = x.description or ""
    print("  %s  vid=%04x pid=%04x  %s  %s %s" % (name, vid, x.pid or 0, chip, mfg, desc))
    cands.append((0 if vid == 0x303A else 1, name))
if not cands:
    print("  (none)")
else:
    cands.sort()
    print("PORT=" + cands[0][1])
endef
export FIND_PORT_PY

define resolve_port
	if [ -n "$(strip $(PORT))" ]; then \
		printf '%s\n' "$(strip $(PORT))"; \
	else \
		python -c 'exec(__import__("os").environ["FIND_PORT_PY"])' | awk -F= '/^PORT=/{print $$2; found=1} END{if(!found) exit 1}'; \
	fi
endef

check: check-env
	@echo
	@$(load_idf); \
	out="$$( python -c 'exec(__import__("os").environ["FIND_PORT_PY"])' )"; \
	echo "$$out" | grep -v '^PORT=' || true; \
	port="$$( echo "$$out" | awk -F= '/^PORT=/{print $$2}' )"; \
	if [ -z "$$port" ]; then \
		echo "FAIL  board not connected over USB"; \
		echo "      Plug Waveshare ESP32-S3-Touch-LCD-3.49 via USB-C."; \
		echo "      Expect /dev/cu.usbmodem*  VID=0x303A (Espressif USB-Serial/JTAG)."; \
		echo "      Then: make ports"; \
		exit 1; \
	fi; \
	echo "OK    board on $$port"

check-env:
	@$(load_idf); \
	fail=0; \
	ok() { printf "OK    %s\n" "$$1"; }; \
	bad() { printf "FAIL  %s\n" "$$1"; fail=1; }; \
	if [ -d "$$IDF_PATH" ]; then ok "ESP-IDF $$IDF_VERSION  $$IDF_PATH"; else bad "IDF_PATH missing: $$IDF_PATH"; fi; \
	if [ -x "$$IDF_PYTHON_ENV_PATH/bin/python" ]; then ok "$$(python --version 2>&1)  $$IDF_PYTHON_ENV_PATH"; else bad "IDF Python venv missing: $$IDF_PYTHON_ENV_PATH"; fi; \
	if command -v ninja >/dev/null; then ok "ninja $$(ninja --version)"; else bad "ninja not on PATH"; fi; \
	if command -v cmake >/dev/null; then ok "cmake $$(cmake --version | head -1 | awk '{print $$NF}')"; else bad "cmake not on PATH"; fi; \
	if command -v xtensa-esp-elf-gcc >/dev/null; then ok "$$(xtensa-esp-elf-gcc --version | head -1)"; else bad "xtensa-esp-elf-gcc not on PATH (needed for esp32s3)"; fi; \
	esptool_ver="$$(python -c 'import esptool; print(getattr(esptool, "__version__", "ok"))' 2>/dev/null)" || true; \
	if [ -n "$$esptool_ver" ]; then ok "esptool $$esptool_ver"; else bad "esptool not importable in IDF venv"; fi; \
	ver="$$( python "$$IDF_PATH/tools/idf.py" --version 2>/dev/null || true )"; \
	if [ -n "$$ver" ]; then ok "$$ver"; else bad "idf.py --version failed"; fi; \
	target="$$( awk -F= '/^CONFIG_IDF_TARGET=/{gsub(/"/,"",$$2); print $$2}' sdkconfig.defaults 2>/dev/null )"; \
	if [ "$$target" = "esp32s3" ]; then ok "target $$target (Waveshare ESP32-S3-Touch-LCD-3.49)"; else bad "unexpected target '$$target' in sdkconfig.defaults"; fi; \
	exit $$fail

ports:
	@$(load_idf); \
	out="$$( python -c 'exec(__import__("os").environ["FIND_PORT_PY"])' )"; \
	echo "$$out" | grep -v '^PORT=' || true; \
	port="$$( echo "$$out" | awk -F= '/^PORT=/{print $$2}' )"; \
	if [ -n "$$port" ]; then \
		echo "selected $$port  (override with PORT=...)"; \
	else \
		echo "No USB serial device for flashing."; \
		echo "Plug the board via USB-C and run make ports again."; \
		exit 1; \
	fi

build:
	@$(load_idf); \
	python "$$IDF_PATH/tools/idf.py" build

flash:
	@$(load_idf); \
	port="$$( $(resolve_port) )" || { \
		echo "FAIL  no USB serial port. Plug the board and run: make ports" >&2; \
		exit 1; \
	}; \
	echo "flashing on $$port"; \
	python "$$IDF_PATH/tools/idf.py" -p "$$port" flash

# App partition only. Bonding keys stay in the nvs partition
# (CONFIG_BT_NIMBLE_NVS_PERSIST). Use after the first full `make flash`.
app-flash:
	@$(load_idf); \
	port="$$( $(resolve_port) )" || { \
		echo "FAIL  no USB serial port. Plug the board and run: make ports" >&2; \
		exit 1; \
	}; \
	echo "app-flash on $$port (NVS kept)"; \
	python "$$IDF_PATH/tools/idf.py" -p "$$port" app-flash

# Forget BLE bonds on the board. macOS still has its own LTK: forget the
# device there too, then re-pair within 60s of boot.
erase-nvs:
	@$(load_idf); \
	port="$$( $(resolve_port) )" || { \
		echo "FAIL  no USB serial port. Plug the board and run: make ports" >&2; \
		exit 1; \
	}; \
	echo "erasing nvs on $$port"; \
	python "$$IDF_PATH/components/partition_table/parttool.py" -p "$$port" erase_partition --partition-name nvs

monitor:
	@$(load_idf); \
	port="$$( $(resolve_port) )" || { \
		echo "FAIL  no USB serial port. Plug the board and run: make ports" >&2; \
		exit 1; \
	}; \
	python "$$IDF_PATH/tools/idf.py" -p "$$port" monitor

# Build, flash, then reset into the app and open the serial monitor.
# Exit the monitor with Ctrl+].
run: flash
	@$(load_idf); \
	port="$$( $(resolve_port) )" || exit 1; \
	python "$$IDF_PATH/tools/idf.py" -p "$$port" monitor

menuconfig:
	@$(load_idf); \
	python "$$IDF_PATH/tools/idf.py" menuconfig

clean:
	@$(load_idf); \
	python "$$IDF_PATH/tools/idf.py" fullclean

reset:
	@$(load_idf); \
	port="$$( $(resolve_port) )" || { \
		echo "FAIL  no USB serial port. Plug the board and run: make ports" >&2; \
		exit 1; \
	}; \
	esptool --chip esp32s3 -p "$$port" run
