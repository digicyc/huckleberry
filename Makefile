# Build, flash, and monitor HuckleBerry with arduino-cli.
# Board: LilyGoLib's "LilyGo T-Deck" definition (ESP32 core 3.3.10, 16 MB flash, 8 MB OPI PSRAM,
# 3 MB app + 9.9 MB FFat), installed by `make deps`.

PORT  ?= $(firstword $(wildcard /dev/serial/by-id/usb-Espressif*) $(wildcard /dev/ttyACM*))
BAUD  ?= 115200

# LilyGoLib and its tested third-party libraries, shared with the T-Watch project.
LILYGO ?= $(abspath ../T-Watch)
BUILD  ?= build

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

.PHONY: deps build upload monitor flash settime screenshot ports clean

# The T-Deck board package and the Arduino libraries MeshCore and the Weather app need beyond
# LilyGoLib-ThirdParty.
deps:
	arduino-cli core install esp32:esp32@3.3.10
	arduino-cli core install lilygo_tdeck_temp:esp32 --additional-urls $(TDECK_INDEX)
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
