CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O3 -march=native -mtune=native
CXXFLAGS += -flto -fno-omit-frame-pointer -g
CXXFLAGS += -DNDEBUG -funroll-loops -MMD -MP
CXXFLAGS += -msse4.2 -mavx2 -mfma
INCLUDES = -I./src -I/usr/local/include
LDFLAGS = -L/usr/local/lib -Wl,-rpath,/usr/local/lib -Wl,--no-as-needed -Wl,-O1
LDLIBS = -lxdp -lbpf -lelf -lpthread

# ── Targets ──────────────────────────────────────────────────────────────────
CORE_TARGETS = replicator rtt replicator_ctl udp_send
MCAST_TARGETS = mcast_send mcast_receive
XDP_PROGRAMS = src/xdp/ucast.o
MCAST_XDP = src/xdp/mcast.o

# ── Replicator object files ──────────────────────────────────────────────────
REPLICATOR_SRCS = src/XdpSocket.cpp src/NicConfig.cpp src/Replicator.cpp src/KernelEcho.cpp
REPLICATOR_OBJS = $(REPLICATOR_SRCS:.cpp=.o)

# ── Build modes ──────────────────────────────────────────────────────────────
# make all         → full AF_XDP build (requires libxdp, libbpf, root to run)
# make kernel-mode → no libxdp/libbpf dep (containers, CI, any Linux)
# make full        → all + multicast targets
all: $(CORE_TARGETS) $(XDP_PROGRAMS)
full: all mcast
mcast: $(MCAST_TARGETS) $(MCAST_XDP)

# ── Kernel-mode build (recursive make with overridden variables) ──────────────
# Builds replicator (kernel-mode only) + tools without any libxdp/libbpf dependency.
kernel-mode:
	$(MAKE) clean
	$(MAKE) replicator rtt replicator_ctl udp_send \
		CXXFLAGS="$(CXXFLAGS) -DKERNEL_MODE_ONLY" \
		LDLIBS="-lpthread" \
		REPLICATOR_OBJS="src/NicConfig.o src/KernelEcho.o"

# ── Pattern rules ────────────────────────────────────────────────────────────
src/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

tools/%.o: tools/%.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

src/xdp/%.o: src/xdp/%.c
	clang -O2 -g -Wall -target bpf -c $< -o $@

# ── Core: replicator ─────────────────────────────────────────────────────────
replicator: src/ReplicatorMain.o $(REPLICATOR_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# ── Tools: measurement instruments ───────────────────────────────────────────
rtt: tools/rtt.o
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

mcast_send: tools/mcast_send.o
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

mcast_receive: tools/mcast_receive.o
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# ── CLI: admin utilities ──────────────────────────────────────────────────────
replicator_ctl: tools/replicator_ctl.o
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

udp_send: tools/udp_send.o
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# ── Housekeeping ─────────────────────────────────────────────────────────────
clean:
	find . -name '*.o' -o -name '*.d' | xargs rm -f
	rm -f $(CORE_TARGETS) $(MCAST_TARGETS)

.PHONY: all full mcast kernel-mode clean
