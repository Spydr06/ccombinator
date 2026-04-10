PREFIX ?= /usr/local

VERSION_MAJOR := 0
VERSION_MINOR := 3

VERSION := $(VERSION_MAJOR).$(VERSION_MINOR)

BUILD_DIR ?= build
EXAMPLES_DIR := examples
INCLUDE_DIR := include

SHARED_LIB := libccombinator.so
STATIC_LIB := libccombinator.a

PC_FILE := ccombinator.pc

CC_HEADERS := $(wildcard $(INCLUDE_DIR)/*.h)
CC_SOURCES := $(wildcard *.c)
CC_OBJECTS := $(patsubst %.c, $(BUILD_DIR)/%.o, $(CC_SOURCES))

ALL_HEADERS := $(CC_HEADERS) $(wildcard *.h)

EX_SOURCES := $(wildcard $(EXAMPLES_DIR)/*.c)
EXAMPLES := $(patsubst %.c, $(BUILD_DIR)/%, $(EX_SOURCES))

CFLAGS += -std=c2x -Wall -Wextra -pedantic -fPIC -g -I$(INCLUDE_DIR) -DCC_VERSION_MAJOR=$(VERSION_MAJOR) -DCC_VERSION_MINOR=$(VERSION_MINOR)
LDFLAGS +=

define HELP_TEXT
ccombinator (Version $(VERSION)) - A simple parser combinator library for C.

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
pc					Generate the pkg-config file [$(BUILD_DIR)/$(PC_FILE)]
install             Install the library and headers to the prefix
install-static      Install only the static library
install-shared      Install only the shared library
install-headers     Install only the header files
install-pc			Install the pkg-config file
clean               Clean up build files
endef

export HELP_TEXT

.PHONY: all
all: static shared examples pc

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

$(BUILD_DIR)/$(EXAMPLES_DIR):
	mkdir -p $@

$(BUILD_DIR):
	mkdir -p $@

.PHONY: pc
pc: $(BUILD_DIR)/$(PC_FILE)

$(BUILD_DIR)/$(PC_FILE): $(PC_FILE).in
	sed -e 's|@PREFIX@|$(PREFIX)|g' 			\
	    -e 's|@INCLUDEDIR@|$(PREFIX)/include|g' \
		-e 's|@LIBDIR@|$(PREFIX)/lib|g' 		\
		-e 's|@VERSION@|$(VERSION)|g' 			\
	    $< > $@

.PHONY: install
install: install-static install-shared install-headers install-pc

.PHONY: install-static
install-static: $(BUILD_DIR)/$(STATIC_LIB)
	install -m 644 $< -D -t $(PREFIX)/lib

.PHONY: install-shared
install-shared: $(BUILD_DIR)/$(SHARED_LIB)
	install -m 644 $< -D -t $(PREFIX)/lib

.PHONY: install-headers
install-headers: $(CC_HEADERS)
	install -m 644 $^ -D -t $(PREFIX)/include

.PHONY: install-pc
install-pc: $(BUILD_DIR)/$(PC_FILE)
	install -D -m 644 $^ $(PREFIX)/lib/pkgconfig/$(PC_FILE)

.PHONY: help
help:
	@echo "$$HELP_TEXT"

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)

