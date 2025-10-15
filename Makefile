BUILD_DIR := build
EXAMPLES_DIR := examples
INCLUDE_DIR := include

SHARED_LIB := libccombinator.so
STATIC_LIB := libccombinator.a

CC_HEADERS := $(wildcard $(INCLUDE_DIR)/%.h)
CC_SOURCES := $(wildcard *.c)
CC_OBJECTS := $(patsubst %.c, $(BUILD_DIR)/%.o, $(CC_SOURCES))

EX_SOURCES := $(wildcard $(EXAMPLES_DIR)/*.c)
EXAMPLES := $(patsubst %.c, $(BUILD_DIR)/%, $(EX_SOURCES))

CFLAGS += -std=c2x -Wall -Wextra -pedantic -fPIC -g -I$(INCLUDE_DIR)
LDFLAGS +=

.PHONY: all
all: static shared examples

.PHONY: static
static: $(BUILD_DIR)/$(STATIC_LIB)

.PHONY: shared
shared: $(BUILD_DIR)/$(SHARED_LIB)

.PHONY: examples
examples: $(EXAMPLES)

$(BUILD_DIR)/$(EXAMPLES_DIR)/%: $(EXAMPLES_DIR)/%.c $(CC_HEADERS) $(BUILD_DIR)/$(EXAMPLES_DIR) static shared
	$(CC) $(CFLAGS) $(LDFLAGS) -L$(BUILD_DIR) -o $@ $< -l:libccombinator.a 

$(BUILD_DIR)/$(STATIC_LIB): $(CC_OBJECTS)
	$(AR) rcv $@ $^

$(BUILD_DIR)/$(SHARED_LIB): $(CC_OBJECTS)
	$(CC) $(LDFLAGS) -shared -o $@ $^

$(BUILD_DIR)/%.o: %.c $(CC_HEADERS) $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/$(EXAMPLES_DIR): $(BUILD_DIR)
	mkdir -p $@

$(BUILD_DIR):
	mkdir -p $@

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)

