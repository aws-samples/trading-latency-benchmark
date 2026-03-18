CXX      ?= g++
CC       ?= clang
CXXFLAGS ?= -O2 -std=c++17 -Wall
BPFFLAGS  = -O2 -target bpf -g

# libbpf (and xsk.h from xdp-tools) — installed by provision.nodes.yaml
# receiver uses libbpf directly for XDP attach (no libxdp dispatcher)
XDP_CFLAGS  = $(shell pkg-config --cflags libbpf 2>/dev/null || \
                echo -I/usr/local/include)
XDP_LDFLAGS = $(shell pkg-config --libs   libbpf 2>/dev/null || \
                echo -L/usr/local/lib -lbpf) \
              -L/usr/local/lib -lxdp -lelf -lz \
              -Wl,-rpath,/usr/local/lib

TARGETS = sender receiver subscriber_filter.o

all: $(TARGETS)

# AF_XDP TX sender — needs libxdp + libbpf (rewritten from AF_PACKET)
sender: sender.cpp
	$(CXX) $(CXXFLAGS) $(XDP_CFLAGS) -o $@ $< $(XDP_LDFLAGS)

# AF_XDP receiver — needs libxdp + libbpf
receiver: receiver.cpp
	$(CXX) $(CXXFLAGS) $(XDP_CFLAGS) -o $@ $< $(XDP_LDFLAGS)

# XDP BPF object — compiled with clang targeting BPF
# Kernel headers are resolved from the running kernel via bpftool or
# the libbpf vmlinux.h path; standard eBPF helper headers from libbpf-devel.
subscriber_filter.o: subscriber_filter.c
	clang $(BPFFLAGS) \
	    -I/usr/include \
	    -I/usr/local/include \
	    $(shell pkg-config --cflags libbpf 2>/dev/null) \
	    -c $< -o $@

clean:
	rm -f $(TARGETS)

.PHONY: all clean
