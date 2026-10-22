# keel build. Objects land in $(BUILD) so the tree stays free of compiler output.

CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes \
            -D_GNU_SOURCE -pthread -Iinclude
LDFLAGS ?= -pthread
BUILD   ?= build

SRC := src/main.c
OBJ := $(patsubst src/%.c,$(BUILD)/%.o,$(SRC))
DEP := $(OBJ:.o=.d)

.PHONY: all clean

all: keel

keel: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ)

$(BUILD)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

clean:
	rm -rf $(BUILD) keel

-include $(DEP)
