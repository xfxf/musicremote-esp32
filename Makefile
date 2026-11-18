.PHONY: configure build flash monitor probe check check-visual load-data

PORT ?= /dev/ttyACM0
CJK_FONT := build/data/fonts/cjk.ttf

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

# Fetch the pinned run-time CJK font (#45) and upload it to the microSD card
# over the USB console without removing the card (#49). The upload is skipped
# when the card already holds an identical file. Close idf.py monitor first.
load-data:
	@test -x tools/fetch_cjk_font.sh || { echo "tools/fetch_cjk_font.sh is missing; it ships with the CJK font change (#45)." >&2; exit 1; }
	@test -f $(CJK_FONT) || tools/fetch_cjk_font.sh $(CJK_FONT)
	/usr/bin/python3 tools/load_sd_data.py --port $(PORT) --skip-identical $(CJK_FONT) fonts/cjk.ttf

check-visual:
	python3 tools/check_visual_evidence.py
	PYTHONPATH=tools python3 -m unittest tools/test_visual_evidence.py

check: build check-visual
	python3 -m py_compile tools/protocol_probe.py
	PYTHONPATH=tools python3 -m unittest tools/test_protocol_probe.py
	python3 -m py_compile tools/load_sd_data.py
	PYTHONPATH=tools python3 -m unittest tools/test_load_sd_data.py
	c++ -std=c++20 -Wall -Wextra -Werror -Imain tools/test_sd_loader_policy.cpp -o /tmp/musicremote-sd-loader-policy-test
	/tmp/musicremote-sd-loader-policy-test
	c++ -std=c++20 -Wall -Wextra -Werror -Imain tools/test_settings_policy.cpp -o /tmp/musicremote-settings-policy-test
	/tmp/musicremote-settings-policy-test
	c++ -std=c++20 -Wall -Wextra -Werror -Imain tools/test_advanced_controls.cpp -o /tmp/musicremote-advanced-controls-test
	/tmp/musicremote-advanced-controls-test
	c++ -std=c++20 -Wall -Wextra -Werror -Imain tools/test_index_worker_policy.cpp -o /tmp/musicremote-index-worker-policy-test
	/tmp/musicremote-index-worker-policy-test
	c++ -std=c++20 -Wall -Wextra -Werror -Imain tools/test_playlist_cache_policy.cpp -o /tmp/musicremote-playlist-cache-policy-test
	/tmp/musicremote-playlist-cache-policy-test
	c++ -std=c++20 -Wall -Wextra -Werror -Imain tools/test_connection_policy.cpp -o /tmp/musicremote-connection-policy-test
	/tmp/musicremote-connection-policy-test
	git check-ignore .env
