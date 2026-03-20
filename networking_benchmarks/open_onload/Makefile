# Copyright (c) Amazon.com, Inc. or its affiliates. All rights reserved.
# SPDX-License-Identifier: MIT-0

# Feed Relay Receiver - OpenOnload Makefile

# Compiler and flags
CC = gcc
CFLAGS = -O3 -march=native -mtune=native -funroll-loops -ffast-math \
         -flto -fomit-frame-pointer -finline-functions \
         -falign-functions=32 -falign-loops=32 \
         -Wall -Wextra -std=gnu99 -DNDEBUG -D_GNU_SOURCE -pthread \
         -Iinclude
LDFLAGS = -lrt -lpthread

# Directories
BUILD_DIR = build

# Source files
COMMON_SRC = src/timestamp_common.c src/timestamp_logging.c
COMMON_HDR = include/timestamp_common.h include/timestamp_logging.h
FEED_RELAY_SRC = src/feed_relay_receiver.c

# Object files
COMMON_OBJ = $(BUILD_DIR)/timestamp_common.o $(BUILD_DIR)/timestamp_logging.o
FEED_RELAY_OBJ = $(BUILD_DIR)/feed_relay_receiver.o

# Executable
FEED_RELAY_BIN = feed_relay_receiver

# Default target
all: $(FEED_RELAY_BIN)
	@echo "Cleaning up intermediate files"
	@rm -f $(FEED_RELAY_OBJ) $(COMMON_OBJ)
	@echo "Build completed"

# Feed relay receiver binary
$(FEED_RELAY_BIN): $(FEED_RELAY_OBJ) $(COMMON_OBJ)
	@echo "Linking $(FEED_RELAY_BIN)"
	@$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "$(FEED_RELAY_BIN) built successfully"

# Create build directory
$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

# Object file rules
$(BUILD_DIR)/%.o: src/%.c $(COMMON_HDR) | $(BUILD_DIR)
	@echo "Compiling $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# Debug build (with debugging symbols, less optimization, DEBUG logging enabled)
debug: CFLAGS = -O1 -g -Wall -Wextra -std=gnu99 -D_GNU_SOURCE -DHW_LOG_COMPILE_LEVEL=1 -pthread -Iinclude
debug: $(FEED_RELAY_BIN)
	@echo "Debug build completed"

# Run all property tests
test: all
	@echo "Compiling and running property tests..."
	@for f in tests/test_*.c; do \
		name=$$(basename $$f .c); \
		echo "  Compiling $$name"; \
		$(CC) $(CFLAGS) -lm -o tests/$$name $$f 2>/dev/null || \
		$(CC) -O2 -std=gnu99 -D_GNU_SOURCE -Iinclude -lm -o tests/$$name $$f; \
		echo "  Running $$name"; \
		./tests/$$name || exit 1; \
		echo ""; \
	done
	@echo "All property tests passed"

# Clean build artifacts
clean:
	@echo "Cleaning build artifacts"
	rm -rf $(BUILD_DIR)
	rm -f $(FEED_RELAY_BIN)
	rm -f tests/test_property*[!.c]
	@echo "Clean completed"

# Check dependencies
check-deps:
	@echo "Checking system dependencies"
	@echo "Compiler: $$($(CC) --version | head -1)"
	@echo "libc version: $$(ldd --version | head -1)"
	@echo "Available libraries:"
	@ldconfig -p | grep -E "(librt|libpthread)" || echo "Warning: Required libraries may not be available"
	@echo "Dependency check completed"

# Print build configuration
config:
	@echo "Build Configuration:"
	@echo "  Compiler: $(CC)"
	@echo "  CFLAGS: $(CFLAGS)"
	@echo "  LDFLAGS: $(LDFLAGS)"
	@echo "  Target: $$($(CC) -dumpmachine)"

# Help target
help:
	@echo "Feed Relay Receiver Build System"
	@echo ""
	@echo "Available targets:"
	@echo "  all          - Build feed_relay_receiver (default, maximum optimization)"
	@echo "  debug        - Build with debug symbols and DEBUG logging"
	@echo "  test         - Compile and run all property tests"
	@echo "  clean        - Remove build artifacts"
	@echo "  check-deps   - Check system dependencies"
	@echo "  config       - Show build configuration"
	@echo "  help         - Show this help message"
	@echo ""
	@echo "Examples:"
	@echo "  make                    # Build (-O3 maximum optimization)"
	@echo "  make debug              # Debug build (-O1 with debug symbols)"
	@echo "  make test               # Run property tests"
	@echo "  make clean all          # Clean and rebuild"

# Force rebuild
rebuild: clean all

# Phony targets
.PHONY: all clean debug test check-deps config help rebuild

# Dependencies
$(FEED_RELAY_OBJ): $(FEED_RELAY_SRC) $(COMMON_HDR)
$(BUILD_DIR)/timestamp_common.o: src/timestamp_common.c $(COMMON_HDR)
$(BUILD_DIR)/timestamp_logging.o: src/timestamp_logging.c $(COMMON_HDR)
