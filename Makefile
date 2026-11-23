CC ?= cc
CFLAGS ?= -O2 -std=c11 -fPIC -fvisibility=hidden -Wall -Wextra
NBASE_REVISION := fe8cd34e7bf0b401f553eb908794dbb5f2c6c7a3
NBASE_SHA256 := 22fec016afe435a8920f1c11cf7c7128ee883a0ee3978bc95acc8ef6dd280d26
NBASE_URL ?= https://codeberg.org/oxund/nothing/archive/$(NBASE_REVISION).tar.gz
NBASE_DEPS ?= .deps
NBASE_ARCHIVE ?= $(NBASE_DEPS)/nothing-$(NBASE_REVISION).tar.gz
NBASE_DIR ?= $(NBASE_DEPS)/nothing-$(NBASE_REVISION)
NBASE_BUILD ?= $(NBASE_DIR)/build
NBASE_LIB ?= $(NBASE_BUILD)/libnbase.a
MORPH_SDK ?= sdk/include
EXT_INSTALL_DIR ?= $(HOME)/.morph/exts/deepseek_web
# Prefer the pinned dependency's nb headers over the SDK's type-only snapshot.
CPPFLAGS += -I. -I$(NBASE_DIR)/include -I$(MORPH_SDK)
CURL_LIBS ?= $(shell curl-config --libs 2>/dev/null || echo -lcurl)
LDLIBS ?= $(NBASE_LIB) -lpthread $(CURL_LIBS)

# Keep bundled nbase/cJSON private so host symbols cannot replace this version.
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
PLUGIN_LDFLAGS = -shared -Wl,-exported_symbol,_ext_run_v2
TEST_LDFLAGS = $(PLUGIN_LDFLAGS) -Wl,-exported_symbol,_harness_run
else
PLUGIN_LDFLAGS = -shared -Wl,-z,defs -Wl,--exclude-libs,ALL
TEST_LDFLAGS = $(PLUGIN_LDFLAGS)
endif

all: deepseek_web.so

deepseek_web.so: deepseek_web.o cJSON.o $(NBASE_LIB)
	$(CC) $(LDFLAGS) $(PLUGIN_LDFLAGS) -o $@ deepseek_web.o cJSON.o $(LDLIBS)

deepseek_web.o: deepseek_web.c cJSON.h $(MORPH_SDK)/morph/ext.h $(MORPH_SDK)/morph/ext_types.h $(NBASE_LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

cJSON.o: cJSON.c cJSON.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

test_harness.o: test_harness.c $(MORPH_SDK)/morph/ext.h $(MORPH_SDK)/morph/ext_types.h $(NBASE_LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

deepseek_test.so: deepseek_web.o test_harness.o cJSON.o $(NBASE_LIB)
	$(CC) $(LDFLAGS) $(TEST_LDFLAGS) -o $@ deepseek_web.o test_harness.o cJSON.o $(LDLIBS)

test: deepseek_web.so deepseek_test.so
	python3 test_deepseek_web.py --lib ./deepseek_test.so

$(NBASE_ARCHIVE):
	@mkdir -p $(dir $@)
	curl -fL --retry 3 --connect-timeout 15 -o "$@.tmp" "$(NBASE_URL)"
	mv "$@.tmp" "$@"

$(NBASE_DIR)/CMakeLists.txt: $(NBASE_ARCHIVE)
	@nbase_hash_line=$$(cmake -E sha256sum "$(NBASE_ARCHIVE)"); \
		test "$${nbase_hash_line%% *}" = "$(NBASE_SHA256)" || \
		{ echo 'nbase archive SHA-256 mismatch' >&2; exit 1; }
	@mkdir -p "$(NBASE_DIR)"
	tar -xzf "$(NBASE_ARCHIVE)" -C "$(NBASE_DIR)" --strip-components=1
	@touch $@

$(NBASE_LIB): $(NBASE_DIR)/CMakeLists.txt
	cmake -S "$(NBASE_DIR)" -B "$(NBASE_BUILD)" \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_POSITION_INDEPENDENT_CODE=ON \
		-DNB_WITH_HTTP=ON \
		-DNB_BUILD_SHARED=OFF \
		-DNB_BUILD_TESTS=OFF \
		-DNB_BUILD_EXAMPLES=OFF \
		-DNB_BUILD_BENCHMARKS=OFF \
		-DNB_BUILD_FUZZERS=OFF
	cmake --build "$(NBASE_BUILD)" --target nbase --parallel

install: deepseek_web.so
	mkdir -p "$(EXT_INSTALL_DIR)"
	cp manifest.toml deepseek_web.so "$(EXT_INSTALL_DIR)/"

clean:
	rm -f deepseek_web.o test_harness.o cJSON.o deepseek_web.so deepseek_test.so test_host.so

distclean: clean
	rm -rf "$(NBASE_DEPS)"

.PHONY: all install clean distclean test
