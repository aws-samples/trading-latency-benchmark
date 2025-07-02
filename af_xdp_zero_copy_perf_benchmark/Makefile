/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: MIT-0
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this
 * software and associated documentation files (the "Software"), to deal in the Software
 * without restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
 * PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -g
INCLUDES = -I. 
LDFLAGS = -L/usr/local/lib -Wl,-rpath,/usr/local/lib
LDLIBS = -lxdp -lbpf -lelf -lpthread

# Source files  
SOURCES = AFXDPSocket.cpp NetworkInterfaceConfigurator.cpp PacketMultiplexer.cpp
OBJECTS = $(SOURCES:.cpp=.o)

# Executables and XDP programs
TARGETS = packet_multiplexer control_client test_client
XDP_PROGRAMS = unicast_filter.o

# Default target
all: $(TARGETS) $(XDP_PROGRAMS)

# Object files
%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Packet Multiplexer (Main application)
packet_multiplexer: PacketMultiplexerMain.o $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# Control Client 
control_client: ControlClient.o
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# Test Client
test_client: TestClient.o
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# XDP Program (BPF bytecode)
unicast_filter.o: unicast_filter.c
	clang -O2 -g -Wall -target bpf -c $< -o $@

# Clean
clean:
	rm -f *.o $(TARGETS) multicast_converter_demo multi_queue_demo $(XDP_PROGRAMS)

# Install dependencies
install-deps:
	@echo "Installing AF_XDP dependencies..."
	@if command -v yum >/dev/null 2>&1; then \
		echo "Detected Amazon Linux/RHEL - using yum"; \
		sudo yum update -y; \
		sudo yum install -y libbpf-devel libxdp-devel gcc-c++ make; \
		sudo yum install -y libbpf libxdp; \
	elif command -v dnf >/dev/null 2>&1; then \
		echo "Detected Fedora/RHEL - using dnf"; \
		sudo dnf update -y; \
		sudo dnf install -y libbpf-devel libxdp-devel gcc-c++ make; \
		sudo dnf install -y libbpf libxdp; \
	elif command -v apt-get >/dev/null 2>&1; then \
		echo "Detected Ubuntu/Debian - using apt-get"; \
		sudo apt-get update; \
		sudo apt-get install -y libbpf-dev libxdp-dev build-essential; \
	else \
		echo "ERROR: Unknown package manager. Please manually install:"; \
		echo "  - libbpf-dev/libbpf-devel (development headers)"; \
		echo "  - libxdp-dev/libxdp-devel (development headers)"; \
		echo "  - libbpf (runtime library)"; \
		echo "  - libxdp (runtime library)"; \
		echo "  - build-essential/gcc-c++/make"; \
		exit 1; \
	fi
	@echo "Dependencies installed successfully!"

# Install runtime libraries only
install-runtime:
	@echo "Installing AF_XDP runtime libraries..."
	@if command -v yum >/dev/null 2>&1; then \
		echo "Amazon Linux detected - checking for libxdp..."; \
		if sudo yum install -y libxdp 2>/dev/null; then \
			echo "libxdp installed successfully"; \
		else \
			echo "WARNING: libxdp not available in repositories"; \
			echo "You may need to build libxdp from source or use a different Linux distribution"; \
			echo "For now, installing available BPF libraries..."; \
			sudo yum install -y kernel-libbpf kernel-libbpf-devel || true; \
		fi \
	elif command -v dnf >/dev/null 2>&1; then \
		if sudo dnf install -y libbpf libxdp 2>/dev/null; then \
			echo "Libraries installed successfully"; \
		else \
			echo "WARNING: Some XDP libraries may not be available"; \
			sudo dnf install -y libbpf kernel-libbpf || true; \
		fi \
	elif command -v apt-get >/dev/null 2>&1; then \
		sudo apt-get install -y libbpf1 libxdp1; \
	else \
		echo "ERROR: Unknown package manager"; \
		exit 1; \
	fi
	@echo "Available libraries installed!"

# Check library dependencies
check-deps:
	@echo "Checking library dependencies:"
	@if [ -f multicast_client_demo ]; then \
		ldd multicast_client_demo 2>/dev/null || echo "Cannot check dependencies"; \
	else \
		echo "multicast_client_demo not found - run 'make' first"; \
	fi

# Help
help:
	@echo "Available targets:"
	@echo "  all                - Build all targets"
	@echo "  packet_multiplexer - Build packet multiplexer (Zero-copy UDP multiplexer)"
	@echo "  control_client     - Build control client (Manage multiplexer destinations)"
	@echo "  test_client        - Build test client (Send UDP test packets)"
	@echo "  clean              - Remove all built files"
	@echo "  install-deps       - Install development and runtime dependencies"
	@echo "  install-runtime    - Install only runtime libraries"
	@echo "  check-deps         - Check library dependencies"
	@echo "  help               - Show this help"
	@echo ""
	@echo "Packet Multiplexer Usage:"
	@echo "  1. Install dependencies: make install-deps"
	@echo "  2. Build project: make"
	@echo "  3. Start multiplexer: sudo ./packet_multiplexer <interface> <listen_ip> <listen_port>"
	@echo "  4. Add destinations: ./control_client <multiplexer_ip> add <dest_ip> <dest_port>"
	@echo "  5. Send test packets: ./test_client <listen_ip> <listen_port>"
	@echo ""
	@echo "Example workflow:"
	@echo "  Terminal 1: sudo ./packet_multiplexer eth0 192.168.1.100 8080"
	@echo "  Terminal 2: ./control_client 192.168.1.100 add 10.0.0.5 9000"
	@echo "  Terminal 3: ./test_client 192.168.1.100 8080"

.PHONY: all clean install-deps install-runtime check-deps help
