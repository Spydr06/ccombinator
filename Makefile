PREFIX ?= /usr/local

BUILD_DIR ?= build
EXAMPLES_DIR := examples
INCLUDE_DIR := include

SHARED_LIB := libccombinator.so
STATIC_LIB := libccombinator.a

CC_HEADERS := $(wildcard $(INCLUDE_DIR)/*.h)
CC_SOURCES := $(wildcard *.c)
CC_OBJECTS := $(patsubst %.c, $(BUILD_DIR)/%.o, $(CC_SOURCES))

ALL_HEADERS := $(CC_HEADERS) $(wildcard *.h)

EX_SOURCES := $(wildcard $(EXAMPLES_DIR)/*.c)
EXAMPLES := $(patsubst %.c, $(BUILD_DIR)/%, $(EX_SOURCES))

CFLAGS += -std=c2x -Wall -Wextra -pedantic -fPIC -g -I$(INCLUDE_DIR)
LDFLAGS +=

define HELP_TEXT
ccombinator - A simple parser combinator library for C.

ccombinator is licensed under the MIT License (MIT).
Copyright (c) 2025-2026 Spydr06

See https://github.com/Spydr06/ccombinator for the source code and copying conditions.

Environment variables:

PREFIX      Install directory prefix [$(PREFIX)]
BUILD_DIR   Directory build files are placed in [$(BUILD_DIR)]
CFLAGS      Additional C compiler flags [$(CFLAGS)]
LDFLAGS     Additional linker flags [$(LDFLAGS)]
CC          The used C compiler [$(CC)]
LD          The used linker [$(LD)]

Make targets:
* Default target

all (*)             Build the library and examples
static              Link to a static library
shared              Link to a shared library
examples            Build example programs in $(EXAMPLES_DIR)
install             Install the library and headers to the prefix
install-static      Install only the static library
install-shared      Install only the shared library
install-headers     Install only the header files
clean               Clean up build files
endef

export HELP_TEXT

.PHONY: all
all: static shared examples

.PHONY: static
static: $(BUILD_DIR)/$(STATIC_LIB)

.PHONY: shared
shared: $(BUILD_DIR)/$(SHARED_LIB)

.PHONY: examples
examples: $(EXAMPLES)

$(BUILD_DIR)/$(EXAMPLES_DIR)/%: $(EXAMPLES_DIR)/%.c $(CC_HEADERS) $(BUILD_DIR)/$(STATIC_LIB) | $(BUILD_DIR)/$(EXAMPLES_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -L$(BUILD_DIR) -o $@ $< -l:libccombinator.a 

$(BUILD_DIR)/$(STATIC_LIB): $(CC_OBJECTS)
	$(AR) rcv $@ $^

$(BUILD_DIR)/$(SHARED_LIB): $(CC_OBJECTS)
	$(CC) $(LDFLAGS) -shared -o $@ $^

$(BUILD_DIR)/%.o: %.c $(ALL_HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/$(EXAMPLES_DIR): $(BUILD_DIR)
	mkdir -p $@

$(BUILD_DIR):
	mkdir -p $@

.PHONY: install
install: install-static install-shared install-headers

.PHONY: install-static
install-static: $(BUILD_DIR)/$(STATIC_LIB) | $(PREFIX)/lib
	install -m 644 $< -t $(PREFIX)/lib

.PHONY: install-shared
install-shared: $(BUILD_DIR)/$(SHARED_LIB) | $(PREFIX)/lib
	install -m 644 $< -t $(PREFIX)/lib

.PHONY: install-headers
install-headers: $(CC_HEADERS) | $(PREFIX)/include
	install -m 644 $^ -t $(PREFIX)/include

.PHONY: help
help:
	@echo "$$HELP_TEXT"

$(PREFIX)/%:
	mkdir -p $@

## Clean the repo
.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)

