.PHONY: configure build flash monitor probe check

PORT ?= /dev/ttyACM0

configure:
	tools/configure.sh

build:
	idf.py build

flash: build
	idf.py -p $(PORT) flash

monitor:
	idf.py -p $(PORT) monitor

probe:
	python3 tools/protocol_probe.py --env .env playlists

check: build
	python3 -m py_compile tools/protocol_probe.py
	git check-ignore .env
