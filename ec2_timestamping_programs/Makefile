# Copyright (c) Amazon.com, Inc. or its affiliates. All rights reserved.
# SPDX-License-Identifier: MIT-0

# Timestamp Client/Server Makefile

# Permission is hereby granted, free of charge, to any person obtaining a copy of this
# software and associated documentation files (the "Software"), to deal in the Software
# without restriction, including without limitation the rights to use, copy, modify,
# merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
# permit persons to whom the Software is furnished to do so.

# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
# PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
# HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
# OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
# SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

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
CLIENT_SRC = src/timestamp_client.c
SERVER_SRC = src/timestamp_server.c

# Object files
COMMON_OBJ = $(BUILD_DIR)/timestamp_common.o $(BUILD_DIR)/timestamp_logging.o
CLIENT_OBJ = $(BUILD_DIR)/timestamp_client.o
SERVER_OBJ = $(BUILD_DIR)/timestamp_server.o

# Executables
CLIENT_BIN = timestamp_client
SERVER_BIN = timestamp_server
CSVJOIN_BIN = timestamp_csvjoin

# Default target - builds and cleans up intermediate files
all: $(CLIENT_BIN) $(SERVER_BIN) $(CSVJOIN_BIN)
	@echo "Cleaning up intermediate files"
	@rm -f $(CLIENT_OBJ) $(SERVER_OBJ) $(COMMON_OBJ)
	@echo "Build completed"

# Client binary
$(CLIENT_BIN): $(CLIENT_OBJ) $(COMMON_OBJ)
	@echo "Linking $(CLIENT_BIN)"
	@$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "$(CLIENT_BIN) built successfully"

# Server binary
$(SERVER_BIN): $(SERVER_OBJ) $(COMMON_OBJ)
	@echo "Linking $(SERVER_BIN)"
	@$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "$(SERVER_BIN) built successfully"

# CSV join binary
$(CSVJOIN_BIN): src/timestamp_csvjoin.c $(COMMON_OBJ)
	@echo "Linking $(CSVJOIN_BIN)"
	@$(CC) -O3 -march=native -mtune=native -funroll-loops -ffast-math -flto \
	-fomit-frame-pointer -finline-functions -falign-functions=32 -falign-loops=32 \
	-Wall -Wextra -std=gnu99 -D_GNU_SOURCE -DNDEBUG -Iinclude \
	-o $@ $< $(COMMON_OBJ) $(LDFLAGS)
	@echo "$(CSVJOIN_BIN) built successfully"

# Create build directory
$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

# Object file rules
$(BUILD_DIR)/%.o: src/%.c $(COMMON_HDR) | $(BUILD_DIR)
	@echo "Compiling $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# Debug build (with debugging symbols and less optimization)
debug: CFLAGS = -O1 -g -Wall -Wextra -std=gnu99 -D_GNU_SOURCE -pthread -Iinclude
debug: $(CLIENT_BIN) $(SERVER_BIN) $(CSVJOIN_BIN)
	@echo "Debug build completed"

# Clean build artifacts
clean:
	@echo "Cleaning build artifacts"
	rm -rf $(BUILD_DIR)
	rm -f $(CLIENT_BIN) $(SERVER_BIN) $(CSVJOIN_BIN)
	@echo "Clean completed"

# Install binaries (optional)
install: $(CLIENT_BIN) $(SERVER_BIN) $(CSVJOIN_BIN)
	@echo "Installing binaries to /usr/local/bin"
	install -m 755 $(CLIENT_BIN) /usr/local/bin/
	install -m 755 $(SERVER_BIN) /usr/local/bin/
	install -m 755 $(CSVJOIN_BIN) /usr/local/bin/
	@echo "Installation completed"

# Uninstall binaries
uninstall:
	@echo "Removing installed binaries"
	rm -f /usr/local/bin/$(CLIENT_BIN)
	rm -f /usr/local/bin/$(SERVER_BIN)
	rm -f /usr/local/bin/$(CSVJOIN_BIN)
	@echo "Uninstall completed"

# Check dependencies and system requirements
check-deps:
	@echo "Checking system dependencies"
	@echo "Compiler: $(shell $(CC) --version | head -1)"
	@echo "libc version: $(shell ldd --version | head -1)"
	@echo "Available libraries:"
	@ldconfig -p | grep -E "(librt|libpthread)" || echo "Warning: Required libraries may not be available"
	@echo "Dependency check completed"

# Print build configuration
config:
	@echo "Build Configuration:"
	@echo "  Compiler: $(CC)"
	@echo "  CFLAGS: $(CFLAGS)"
	@echo "  LDFLAGS: $(LDFLAGS)"
	@echo "  Target: $(shell $(CC) -dumpmachine)"

# Help target
help:
	@echo "Hardware Timestamp Tools Build System"
	@echo ""
	@echo "Available targets:"
	@echo "  all          - Build both client and server (default, maximum optimization)"
	@echo "  client       - Build client only (maximum optimization)"
	@echo "  server       - Build server only (maximum optimization)"
	@echo "  debug        - Build with debug symbols"
	@echo "  clean        - Remove build artifacts"
	@echo "  install      - Install binaries to /usr/local/bin"
	@echo "  uninstall    - Remove installed binaries"
	@echo "  check-deps   - Check system dependencies"
	@echo "  config       - Show build configuration"
	@echo "  help         - Show this help message"
	@echo ""
	@echo "Examples:"
	@echo "  make                    # Build both programs (-O3 maximum optimization)"
	@echo "  make client             # Build client only (-O3 maximum optimization)"
	@echo "  make debug              # Debug build (-O1 with debug symbols)"
	@echo "  make clean all          # Clean and rebuild"

# Individual targets
client: $(CLIENT_BIN)
server: $(SERVER_BIN)

# Force rebuild
rebuild: clean all

# Phony targets
.PHONY: all clean install uninstall check-deps config help debug client server rebuild

# Dependencies
$(CLIENT_OBJ): $(CLIENT_SRC) $(COMMON_HDR)
$(SERVER_OBJ): $(SERVER_SRC) $(COMMON_HDR)
$(BUILD_DIR)/timestamp_common.o: src/timestamp_common.c $(COMMON_HDR)
$(BUILD_DIR)/timestamp_logging.o: src/timestamp_logging.c $(COMMON_HDR)
