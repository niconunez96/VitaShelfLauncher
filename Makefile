CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -pedantic -Iinclude
CORE = src/core/vitashelflauncher.c src/core/vita_titles.c src/core/integrations.c src/core/library_preferences.c
TEST = build/test_core

.PHONY: test test-preferences test-integrations test-local-library test-ui test-vita-lifecycle test-assets test-renderer test-release-automation clean

test: $(TEST) test-preferences test-integrations test-local-library test-ui test-vita-lifecycle test-release-automation
	$(TEST)

test-preferences: build/test_library_preferences
	build/test_library_preferences

build/test_library_preferences: src/core/library_preferences.c src/core/vitashelflauncher.c include/library_preferences.h include/vitashelflauncher.h tests/test_library_preferences.c
	@mkdir -p build
	$(CC) $(CFLAGS) src/core/library_preferences.c src/core/vitashelflauncher.c tests/test_library_preferences.c -o $@

test-integrations: build/test_integrations
	build/test_integrations

build/test_integrations: src/core/integrations.c tests/test_integrations.c include/integrations.h
	@mkdir -p build
	$(CC) $(CFLAGS) src/core/integrations.c tests/test_integrations.c -o $@

test-local-library: build/test_local_library
	build/test_local_library

build/test_local_library: src/core/vita_titles.c src/core/vitashelflauncher.c src/vita/local_library.c tests/test_local_library.c include/vita_titles.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc/vita src/core/vitashelflauncher.c src/core/vita_titles.c src/vita/local_library.c tests/test_local_library.c -o $@

test-release-automation:
	python3 tests/test_release_automation.py

test-ui: build/test_presentation build/test_catalog_cache
	build/test_presentation
	build/test_catalog_cache
	python3 tests/test_ui_assets.py

test-vita-lifecycle:
	python3 tests/test_vita_render_lifecycle.py
	python3 tests/test_vita_t2_concurrency_probe.py
	python3 tests/test_vita_audio.py
	python3 tests/test_vita_download_tuning.py
	python3 tests/test_download_lifecycle.py
	python3 tests/test_vita_thread_priority.py
	python3 tests/test_romm_worker.py
	python3 tests/test_romm_pairing.py
	python3 tests/test_artwork_worker.py
	python3 tests/test_sync_progress.py
	python3 tests/test_settings_navigation.py
	python3 tests/test_vita_touch_navigation.py
	python3 tests/test_library_independent.py
	python3 tests/test_cache_compatibility_independent.py

build/test_presentation: src/vita/ui_presentation.c src/vita/ui_presentation.h tests/test_presentation.c
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc/vita src/vita/ui_presentation.c tests/test_presentation.c -lm -o $@

build/test_catalog_cache: src/vita/catalog_cache.c src/vita/catalog_cache.h include/vitashelflauncher.h tests/test_catalog_cache.c
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc/vita src/vita/catalog_cache.c tests/test_catalog_cache.c -o $@

test-assets:
	GO111MODULE=off go test ./scripts/assetprep

# Optional host layout smoke test; requires Cairo and pkg-config, not VitaSDK.
test-renderer: build/test_renderer
	build/test_renderer

build/test_renderer: src/vita/ui_renderer.c src/vita/ui_renderer.h src/vita/ui_presentation.c src/vita/ui_presentation.h src/vita/ui_text.c src/vita/ui_text.h src/vita/ui_backdrop.c src/vita/ui_backdrop.h src/core/vitashelflauncher.c tests/preview_renderer.c tests/ui_stubs/vita2d.h tests/ui_stubs/psp2/io/fcntl.h tests/ui_stubs/psp2/kernel/processmgr.h include/vitashelflauncher.h
	@mkdir -p build
	$(CC) $(CFLAGS) -DVP_UI_TESTING -Isrc/vita -Itests/ui_stubs $$(pkg-config --cflags cairo freetype2 libpng) src/core/vitashelflauncher.c src/vita/ui_presentation.c src/vita/ui_text.c src/vita/ui_backdrop.c src/vita/ui_renderer.c tests/preview_renderer.c $$(pkg-config --libs cairo freetype2 libpng) -lm -pthread -o $@

$(TEST): $(CORE) tests/test_core.c include/vitashelflauncher.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(CORE) tests/test_core.c -o $(TEST)

clean:
	rm -rf build
