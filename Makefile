CC ?= cc
CFLAGS ?= -O2 -fPIC -Wall -Wextra
LDLIBS ?= -lcurl

all: deepseek_web.so

deepseek_web.so: deepseek_web.o cJSON.o
	$(CC) -shared -o $@ $^ $(LDLIBS)

deepseek_web.o: deepseek_web.c cJSON.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

cJSON.o: cJSON.c cJSON.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean:
	rm -f deepseek_web.o cJSON.o deepseek_web.so

.PHONY: all clean
