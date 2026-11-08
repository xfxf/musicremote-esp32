.PHONY: configure build flash monitor probe check check-visual

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

check-visual:
	python3 tools/check_visual_evidence.py
	PYTHONPATH=tools python3 -m unittest tools/test_visual_evidence.py

check: build check-visual
	python3 -m py_compile tools/protocol_probe.py
	PYTHONPATH=tools python3 -m unittest tools/test_protocol_probe.py
	c++ -std=c++20 -Wall -Wextra -Werror -Imain tools/test_settings_policy.cpp -o /tmp/musicremote-settings-policy-test
	/tmp/musicremote-settings-policy-test
	c++ -std=c++20 -Wall -Wextra -Werror -Imain tools/test_advanced_controls.cpp -o /tmp/musicremote-advanced-controls-test
	/tmp/musicremote-advanced-controls-test
	c++ -std=c++20 -Wall -Wextra -Werror -Imain tools/test_index_worker_policy.cpp -o /tmp/musicremote-index-worker-policy-test
	/tmp/musicremote-index-worker-policy-test
	c++ -std=c++20 -Wall -Wextra -Werror -Imain tools/test_playlist_cache_policy.cpp -o /tmp/musicremote-playlist-cache-policy-test
	/tmp/musicremote-playlist-cache-policy-test
	git check-ignore .env
