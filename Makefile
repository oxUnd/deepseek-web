CC ?= cc
CFLAGS ?= -O2 -fPIC -Wall -Wextra
NBASE_DIR ?= $(HOME)/Work/c-family/nothing
NBASE_LIB ?= $(NBASE_DIR)/build/libnbase.a
CPPFLAGS += -I$(NBASE_DIR)/include
LDLIBS ?= $(NBASE_LIB) -lpthread -lcurl

all: deepseek_web.so

deepseek_web.so: deepseek_web.o cJSON.o $(NBASE_LIB)
	$(CC) -shared -o $@ deepseek_web.o cJSON.o $(LDLIBS)

deepseek_web.o: deepseek_web.c cJSON.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

cJSON.o: cJSON.c cJSON.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(NBASE_LIB):
	@echo "nbase static library not found: $(NBASE_LIB)"
	@echo "Build nbase first, or set NBASE_DIR=/path/to/nothing"
	@exit 1

clean:
	rm -f deepseek_web.o cJSON.o deepseek_web.so

.PHONY: all clean
