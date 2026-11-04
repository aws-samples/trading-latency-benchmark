# Test image for the native sender/receiver: toolchain + tc + procps + time.
FROM debian:bookworm-slim
RUN apt-get update -qq && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
        g++ make iproute2 procps time ca-certificates >/dev/null && \
    rm -rf /var/lib/apt/lists/*
