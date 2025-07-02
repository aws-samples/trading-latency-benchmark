CXX = g++
# HFT OPTIMIZED COMPILATION FLAGS
CXXFLAGS = -std=c++17 -Wall -Wextra -O3 -march=native -mtune=native
CXXFLAGS += -flto -fno-omit-frame-pointer -g
CXXFLAGS += -DNDEBUG -ffast-math -funroll-loops
CXXFLAGS += -msse4.2 -mavx2 -mfma  # Enable SIMD instructions for maximum performance
CXXFLAGS += -fno-stack-protector  # Remove stack protection overhead for HFT
CXXFLAGS += -fomit-frame-pointer  # Optimize register usage
INCLUDES = -I. 
LDFLAGS = -L/usr/local/lib -Wl,-rpath,/usr/local/lib
LDFLAGS += -Wl,--no-as-needed -Wl,-O1  # Optimized linking
LDLIBS = -lxdp -lbpf -lelf -lpthread

# Source files  
SOURCES = AFXDPSocket.cpp NetworkInterfaceConfigurator.cpp PacketReplicator.cpp
OBJECTS = $(SOURCES:.cpp=.o)

# Executables and XDP programs
TARGETS = packet_replicator control_client test_client market_data_provider_client
XDP_PROGRAMS = unicast_filter.o

# Default target
all: $(TARGETS) $(XDP_PROGRAMS)

# Object files
%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Packet Replicator (Main application)
packet_replicator: PacketReplicatorMain.o $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# Control Client 
control_client: ControlClient.o
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# Test Client
test_client: TestClient.o
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# Market Data Provider Client (Performance benchmark)
market_data_provider_client: MarketDataProviderClient.o
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
	@echo "  packet_replicator  - Build packet replicator (Zero-copy UDP replicator)"
	@echo "  control_client     - Build control client (Manage replicator destinations)"
	@echo "  test_client        - Build test client (Send UDP test packets)"
	@echo "  clean              - Remove all built files"
	@echo "  install-deps       - Install development and runtime dependencies"
	@echo "  install-runtime    - Install only runtime libraries"
	@echo "  check-deps         - Check library dependencies"
	@echo "  help               - Show this help"
	@echo ""
	@echo "Packet Replicator Usage:"
	@echo "  1. Install dependencies: make install-deps"
	@echo "  2. Build project: make"
	@echo "  3. Start replicator: sudo ./packet_replicator <interface> <listen_ip> <listen_port>"
	@echo "  4. Add destinations: ./control_client <replicator_ip> add <dest_ip> <dest_port>"
	@echo "  5. Send test packets: ./test_client <listen_ip> <listen_port>"
	@echo ""
	@echo "Example workflow:"
	@echo "  Terminal 1: sudo ./packet_replicator eth0 192.168.1.100 8080"
	@echo "  Terminal 2: ./control_client 192.168.1.100 add 10.0.0.5 9000"
	@echo "  Terminal 3: ./test_client 192.168.1.100 8080"

.PHONY: all clean install-deps install-runtime check-deps help
