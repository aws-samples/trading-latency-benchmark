# Local build + test harness for the AF_XDP benchmark.
#
# Mirrors the AMI bake (scripts/bake-ami.sh): installs the toolchain, builds
# xdp-tools from source, then `make full` — validating that the code COMPILES
# exactly as the bake will, before spending ~12 min on an EC2 bake.
#
# It then runs the pytest suite in echo mode (replicator --echo-mode): no
# root, no XDP/BPF at runtime, so it works in an unprivileged container. The
# real AF_XDP datapath still requires EC2 (run_ucast).
#
# The Makefile targets x86_64 (-msse4.2/-mavx2/-mfma, rdtsc), so build for
# linux/amd64 (emulated on Apple Silicon):
#
#   cd networking_benchmarks/af_xdp
#   docker build --platform linux/amd64 -f dev/Dockerfile -t afxdp-test .
#   docker run --rm --platform linux/amd64 afxdp-test          # build already ran; runs pytest
#
FROM --platform=linux/amd64 amazonlinux:2023

# ── Toolchain (same set as bake-ami.sh) ───────────────────────────────────────
RUN dnf install -y \
      git clang llvm libbpf-devel elfutils-libelf-devel \
      kernel-headers make gcc gcc-c++ pkgconfig m4 libpcap-devel \
      python3 python3-pip iproute procps-ng \
 && dnf clean all

# ── xdp-tools (provides libxdp for `make full`) ───────────────────────────────
RUN git clone --depth 1 https://github.com/xdp-project/xdp-tools.git /opt/xdp-tools \
 && cd /opt/xdp-tools \
 && grep -rlZ --include='*.c' --include='*.h' \
        -e '\bbool\b' -e '\btrue\b' -e '\bfalse\b' lib headers 2>/dev/null \
      | while IFS= read -r -d '' f; do \
          grep -q 'stdbool.h' "$f" || sed -i '1i #include <stdbool.h>' "$f"; \
        done \
 && ./configure \
 && echo 'CFLAGS += -Wno-error' >> config.mk \
 && make -j"$(nproc)" \
 && make install > /dev/null \
 && ldconfig \
 && mkdir -p /usr/lib64/bpf \
 && cp -f lib/libxdp/xdp-dispatcher.o /usr/lib64/bpf/ 2>/dev/null || true

RUN pip3 install --no-cache-dir pytest

ENV LIBXDP_OBJECT_PATH=/usr/lib64/bpf

# ── Source (only what the build/tests need — keeps context small) ─────────────
# The suite lives at the repo's top-level tests/, so it resolves AF_XDP_DIR (two
# parents up) to /src both here and locally.
WORKDIR /src
COPY Makefile ./
COPY src/   ./src/
COPY tools/ ./tools/
COPY tests/ ./tests/

# Build validation: full AF_XDP build must compile (mirrors the AMI bake).
RUN make full

CMD ["pytest", "tests/", "-v", "--tb=short"]
