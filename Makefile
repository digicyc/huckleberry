# Build, flash, and monitor HuckleBerry with arduino-cli.
# Board: LilyGoLib's "LilyGo T-Deck" definition (ESP32 core 3.3.10, 16 MB flash, 8 MB OPI PSRAM,
# 3 MB app + 9.9 MB FFat), installed by `make deps`.

PORT  ?= $(firstword $(wildcard /dev/serial/by-id/usb-Espressif*) $(wildcard /dev/ttyACM*))
BAUD  ?= 115200

# LilyGoLib and its tested third-party libraries, shared with the T-Watch project.
LILYGO ?= $(abspath ../T-Watch)
BUILD  ?= build

ESP32_INDEX := https://espressif.github.io/arduino-esp32/package_esp32_index.json
TDECK_INDEX := https://raw.githubusercontent.com/Xinyuan-LilyGO/LilyGoLib/master/arduino/temporary_tdeck/package_lilygo_tdeck_index.json

# MeshCore radio and table settings. They are global because the MeshCore and RadioLib headers
# read them; the radio values are MeshCore's "USA/Canada (Recommended)" preset.
MESH_FLAGS := -DRADIOLIB_STATIC_ONLY=1 -DRADIOLIB_GODMODE=1 \
	-DLORA_FREQ=910.525 -DLORA_BW=62.5 -DLORA_SF=7 -DLORA_CR=5 -DLORA_TX_POWER=22 \
	-DSX126X_DIO3_TCXO_VOLTAGE=1.8f -DSX126X_DIO2_AS_RF_SWITCH=true -DSX126X_CURRENT_LIMIT=140 \
	-DSX126X_RX_BOOSTED_GAIN=1 -DMAX_CONTACTS=128 -DMAX_GROUP_CHANNELS=16
# LilyGoLib keeps a copy of the screen for `make screenshot`.
APP_FLAGS := -DLILYGO_SCREEN_CAPTURE_ENABLED=1
EXTRA_FLAGS ?=
FQBN  := lilygo_tdeck_temp:esp32:lilygo_tdeck:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=default,PartitionScheme=app3M_fat9M_16MB,DebugLevel=none,LoopCore=1,EventsCore=1,EraseFlash=none

.PHONY: deps build upload monitor flash release settime screenshot ports clean

# The T-Deck board package and the Arduino libraries MeshCore and the Weather app need beyond
# LilyGoLib-ThirdParty.
deps:
	arduino-cli core update-index --additional-urls $(ESP32_INDEX),$(TDECK_INDEX)
	arduino-cli core install esp32:esp32@3.3.10 --additional-urls $(ESP32_INDEX)
	arduino-cli core install lilygo_tdeck_temp:esp32@0.1.1 --additional-urls $(TDECK_INDEX)
	arduino-cli lib install "ArduinoJson@7.4.3" "Crypto@0.4.0" "base64@1.3.0" "RTClib@2.1.4" "CayenneLPP@1.6.1"

build:
	arduino-cli compile --fqbn '$(FQBN)' --build-path $(BUILD) \
		--build-property "compiler.cpp.extra_flags=$(MESH_FLAGS) $(APP_FLAGS) $(EXTRA_FLAGS)" \
		--library $(LILYGO)/LilyGoLib --libraries $(LILYGO)/LilyGoLib-ThirdParty --library lib/MeshCore .

upload:
	@test -n "$(PORT)" || { echo "No T-Deck serial port found; plug it in or pass PORT=/dev/ttyACMx"; exit 1; }
	arduino-cli upload --fqbn '$(FQBN)' --input-dir $(BUILD) --port $(PORT) .

monitor:
	@test -n "$(PORT)" || { echo "No T-Deck serial port found; plug it in or pass PORT=/dev/ttyACMx"; exit 1; }
	arduino-cli monitor --port $(PORT) --config baudrate=$(BAUD)

flash: build upload monitor

# Firmware files for a GitHub release, in dist/, named with the version in src/version.h:
#   huckleberry-<version>-install.bin  bootloader, partition table and app, flashed at 0x0. For a
#                                      first install: it also clears the settings (NVS).
#   huckleberry-<version>-update.bin   the app alone, flashed at 0x10000. Keeps settings, chats and
#                                      the MeshCore identity.
#   SHA256SUMS
# The install image is Arduino's merged.bin without its padding to the full 16 MB flash.
VERSION := $(shell sed -n 's/.*HUCKLEBERRY_VERSION "\(.*\)".*/\1/p' src/version.h)
ESPTOOL ?= $(firstword $(wildcard $(HOME)/.arduino15/packages/esp32/tools/esptool_py/*/esptool) esptool)
DIST    ?= dist

release: build
	rm -rf $(DIST)
	mkdir -p $(DIST)
	$(ESPTOOL) --chip esp32s3 merge-bin -o $(DIST)/huckleberry-$(VERSION)-install.bin \
		--flash-mode keep --flash-freq keep --flash-size keep \
		0x0 $(BUILD)/HuckleBerry.ino.bootloader.bin 0x8000 $(BUILD)/HuckleBerry.ino.partitions.bin \
		0xe000 $(BUILD)/boot_app0.bin 0x10000 $(BUILD)/HuckleBerry.ino.bin
	cp $(BUILD)/HuckleBerry.ino.bin $(DIST)/huckleberry-$(VERSION)-update.bin
	cd $(DIST) && sha256sum *.bin > SHA256SUMS
	@ls -l $(DIST)

# Sends the host's current Unix time to the running sketch, which sets the clock.
settime:
	@test -n "$(PORT)" || { echo "No T-Deck serial port found; plug it in or pass PORT=/dev/ttyACMx"; exit 1; }
	stty -F $(PORT) raw -echo && printf 'T%s\n' "$$(date +%s)" > $(PORT)

# Saves what the screen shows to screenshot.png (or OUT=file.png).
screenshot:
	@test -n "$(PORT)" || { echo "No T-Deck serial port found; plug it in or pass PORT=/dev/ttyACMx"; exit 1; }
	uv run tools/screenshot.py --port $(PORT) --out $(or $(OUT),screenshot.png)

ports:
	arduino-cli board list

clean:
	rm -rf $(BUILD)
