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
	PYTHONPATH=tools python3 -m unittest tools/test_protocol_probe.py
	c++ -std=c++20 -Wall -Wextra -Werror -Imain tools/test_settings_policy.cpp -o /tmp/musicremote-settings-policy-test
	/tmp/musicremote-settings-policy-test
	git check-ignore .env
