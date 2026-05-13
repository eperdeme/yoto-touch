# yoto-touch — build helpers wrapping PlatformIO
# Usage:
#   make              # build only
#   make flash        # build + upload
#   make monitor      # serial monitor
#   make fm           # build + upload + monitor (the common workflow)
#   make clean        # clean build artifacts

PIO      := ~/.platformio/penv/bin/pio
DIR      := firmware/arduino
PORT     := /dev/cu.wchusbserial110
BAUD     := 115200

.PHONY: build flash monitor fm clean

build:
	cd $(DIR) && $(PIO) run

flash:
	cd $(DIR) && $(PIO) run -t upload

monitor:
	cd $(DIR) && $(PIO) device monitor -p $(PORT) -b $(BAUD)

fm: flash monitor

clean:
	cd $(DIR) && $(PIO) run -t clean
