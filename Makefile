CC ?= cc
CFLAGS ?= -O2 -fPIC -Wall -Wextra
NBASE_VERSION ?= v0.1.0
NBASE_URL ?= https://codeberg.org/oxund/nothing/archive/$(NBASE_VERSION).tar.gz
NBASE_DEPS ?= .deps
NBASE_ARCHIVE ?= $(NBASE_DEPS)/nothing-$(NBASE_VERSION).tar.gz
NBASE_DIR ?= $(NBASE_DEPS)/nothing-$(NBASE_VERSION)
NBASE_BUILD ?= $(NBASE_DIR)/build
NBASE_LIB ?= $(NBASE_BUILD)/libnbase.a
MORPH_SDK ?= sdk/include
EXT_INSTALL_DIR ?= $(HOME)/.morph/exts/deepseek_web
CPPFLAGS += -I. -I$(MORPH_SDK) -I$(NBASE_DIR)/include
LDLIBS ?= $(NBASE_LIB) -lpthread -lcurl

all: $(NBASE_LIB) deepseek_web.so

deepseek_web.so: deepseek_web.o cJSON.o $(NBASE_LIB)
	$(CC) -shared -o $@ deepseek_web.o cJSON.o $(LDLIBS)

deepseek_web.o: deepseek_web.c cJSON.h $(MORPH_SDK)/morph/ext.h $(MORPH_SDK)/morph/ext_types.h $(NBASE_LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

cJSON.o: cJSON.c cJSON.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

test_harness.o: test_harness.c $(MORPH_SDK)/morph/ext.h $(MORPH_SDK)/morph/ext_types.h $(NBASE_LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

deepseek_test.so: deepseek_web.o test_harness.o cJSON.o $(NBASE_LIB)
	$(CC) -shared -o $@ deepseek_web.o test_harness.o cJSON.o $(LDLIBS)

test: deepseek_test.so
	python3 test_deepseek_web.py --lib ./deepseek_test.so

$(NBASE_ARCHIVE):
	@mkdir -p $(dir $@)
	curl -fL --retry 3 --connect-timeout 15 -o $@ $(NBASE_URL)

$(NBASE_DIR)/CMakeLists.txt: $(NBASE_ARCHIVE)
	@mkdir -p $(NBASE_DIR)
	tar -xzf $(NBASE_ARCHIVE) -C $(NBASE_DIR) --strip-components=1
	@touch $@

$(NBASE_LIB): $(NBASE_DIR)/CMakeLists.txt
	cmake -S $(NBASE_DIR) -B $(NBASE_BUILD) \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_POSITION_INDEPENDENT_CODE=ON \
		-DNB_BUILD_SHARED=OFF \
		-DNB_BUILD_TESTS=OFF \
		-DNB_BUILD_EXAMPLES=OFF \
		-DNB_BUILD_BENCHMARKS=OFF \
		-DNB_BUILD_FUZZERS=OFF
	cmake --build $(NBASE_BUILD) --target nbase --parallel

install: deepseek_web.so
	mkdir -p "$(EXT_INSTALL_DIR)"
	cp manifest.toml deepseek_web.so "$(EXT_INSTALL_DIR)/"

clean:
	rm -f deepseek_web.o test_harness.o cJSON.o deepseek_web.so deepseek_test.so

distclean: clean
	rm -rf $(NBASE_DEPS)

.PHONY: all install clean distclean test
