# keel build. Objects land in $(BUILD) so the tree stays free of compiler output.

CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes \
            -D_GNU_SOURCE -pthread -Iinclude
LDFLAGS ?= -pthread
BUILD   ?= build

LIB_SRC := src/net.c src/url.c src/http_parse.c src/mime.c src/static.c src/http_write.c \
           src/event.c
SRC := src/main.c $(LIB_SRC)
OBJ := $(patsubst src/%.c,$(BUILD)/%.o,$(SRC))
DEP := $(OBJ:.o=.d)
LIB_OBJ := $(patsubst src/%.c,$(BUILD)/%.o,$(LIB_SRC))

TEST_SRC := $(wildcard tests/test_*.c)
TEST_BIN := $(patsubst tests/%.c,$(BUILD)/%,$(TEST_SRC))

.PHONY: all clean test
.PRECIOUS: $(BUILD)/%.o

all: keel

keel: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ)

$(BUILD)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/test_%: tests/test_%.c $(LIB_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Itests -o $@ $< $(LIB_OBJ) $(LDFLAGS)

test: $(TEST_BIN)
	@set -e; for t in $(TEST_BIN); do echo "== $$t"; $$t; done

clean:
	rm -rf $(BUILD) keel

-include $(DEP)
