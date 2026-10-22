# keel build. Objects land in $(BUILD) so the tree stays free of compiler output.

CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes \
            -D_GNU_SOURCE -pthread -Iinclude
LDFLAGS ?= -pthread
BUILD   ?= build

LIB_SRC := src/net.c src/url.c src/http_parse.c src/mime.c src/static.c src/http_write.c \
           src/config.c src/log.c src/handlers.c src/event.c
SRC := src/main.c $(LIB_SRC)
OBJ := $(patsubst src/%.c,$(BUILD)/%.o,$(SRC))
DEP := $(OBJ:.o=.d)
LIB_OBJ := $(patsubst src/%.c,$(BUILD)/%.o,$(LIB_SRC))

TEST_SRC := $(wildcard tests/test_*.c)
TEST_BIN := $(patsubst tests/%.c,$(BUILD)/%,$(TEST_SRC))

.PHONY: all clean test asan clean-objs install
.PRECIOUS: $(BUILD)/%.o

-include $(BUILD)/sanitize.mk

all: keel

keel: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ)

$(BUILD)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/test_%: tests/test_%.c $(LIB_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Itests -o $@ $< $(LIB_OBJ) $(LDFLAGS)

$(BUILD)/integration: tests/integration.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Itests -o $@ $< $(LDFLAGS)

test: keel $(TEST_BIN) $(BUILD)/integration
	@set -e; for t in $(TEST_BIN); do echo "== $$t"; $$t; done
	@echo "== $(BUILD)/integration"; $(BUILD)/integration

asan:
	mkdir -p $(BUILD)
	printf 'CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer\nLDFLAGS += -fsanitize=address,undefined\n' > $(BUILD)/sanitize.mk
	$(MAKE) clean-objs
	$(MAKE) all test

clean-objs:
	rm -f $(OBJ) $(TEST_BIN) $(BUILD)/integration keel

clean:
	rm -rf $(BUILD) keel

PREFIX ?= /usr/local
install: keel docs/keel.1
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 keel $(DESTDIR)$(PREFIX)/bin/keel
	install -d $(DESTDIR)$(PREFIX)/share/man/man1
	install -m 644 docs/keel.1 $(DESTDIR)$(PREFIX)/share/man/man1/keel.1

-include $(DEP)
