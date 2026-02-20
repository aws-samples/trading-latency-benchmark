# HFT Trading Latency Benchmark Client (Rust)

A high-performance Rust client for measuring trading latency using WebSocket connections. This project implements a latency testing framework for trading systems with precise histogram-based measurements.

## Features

- Async WebSocket communication using tokio-tungstenite
- High-precision latency measurements using HdrHistogram
- Configurable test parameters via TOML
- TLS/SSL support for secure connections
- JSON-based protocol implementation
- Detailed latency statistics reporting (p50, p90, p95, p99, p99.9, p99.99, max)

## Prerequisites

- Rust 1.70+ (2021 edition)
- OpenSSL development libraries (for TLS support)

## Dependencies

Managed via Cargo:
- tokio (async runtime)
- tokio-tungstenite (WebSocket client)
- hdrhistogram (latency measurements)
- serde/serde_json (serialization)
- uuid (client ID generation)

## Building

```bash
cd rust_client

# Debug build
cargo build

# Release build (optimized)
cargo build --release

# Run tests
cargo test
```

## System Setup

### macOS

```bash
# Install Rust
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh

# Install OpenSSL (for TLS support)
brew install openssl
```

### Amazon Linux 2/2023

```bash
# Install Rust
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
source $HOME/.cargo/env

# Install OpenSSL development libraries
sudo yum install openssl-devel -y
```

### Ubuntu/Debian

```bash
# Install Rust
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh

# Install OpenSSL development libraries
sudo apt-get install libssl-dev pkg-config -y
```

## Usage

```bash
# Run with default config (config.toml)
cargo run --release

# Or run the binary directly
./target/release/hft_client
```

### Configuration

Create a `config.toml` file in the project root:

```toml
# Trading pairs to test
coin_pairs = ["BTC_EUR", "BTC_USDT"]

# WebSocket server connection
host = "localhost"
websocket_port = 8888

# Authentication
api_token = 3001

# Test configuration
test_size = 50000      # Print stats every N responses
warmup_count = 1       # Warmup iterations

# TLS settings
use_ssl = false
# key_store_path = "/path/to/keystore.p12"
# key_store_password = "password"
ciphers = "ECDHE-RSA-AES128-GCM-SHA256"
```

#### Configuration Parameters

| Parameter | Description | Default |
|-----------|-------------|---------|
| coin_pairs | Trading pairs to cycle through | ["BTC_EUR", "BTC_USDT"] |
| host | WebSocket server hostname | localhost |
| websocket_port | WebSocket server port | 8888 |
| api_token | Authentication token | 3001 |
| test_size | Responses before printing stats | 50000 |
| warmup_count | Warmup iterations | 1 |
| use_ssl | Enable TLS/SSL | false |
| key_store_path | Path to TLS keystore (optional) | None |
| key_store_password | Keystore password (optional) | None |
| ciphers | TLS cipher suites | ECDHE-RSA-AES128-GCM-SHA256 |

### Environment Variables

```bash
# Set log level
RUST_LOG=debug cargo run    # debug, info, warn, error
RUST_LOG=rust_hft_client=debug,info cargo run  # per-module
```

## Project Structure

```
rust_client/
├── Cargo.toml           # Dependencies and project config
├── config.toml          # Runtime configuration
├── src/
│   ├── main.rs          # Entry point
│   ├── lib.rs           # Library exports
│   ├── client.rs        # WebSocket client and state machine
│   ├── config.rs        # Configuration loading
│   ├── protocol.rs      # Message serialization/parsing
│   └── tracker.rs       # Latency tracking with HdrHistogram
└── tests/
    ├── config_tests.rs
    ├── protocol_tests.rs
    └── tracker_tests.rs
```

## Performance Metrics

The client measures and reports:

- Round-trip latency (microseconds)
- Percentile distributions: p50, p90, p95, p99, p99.9, p99.99
- Maximum latency
- Response count per interval

Example output:
```json
{
  "latency_us": {
    "p50": 245,
    "p90": 312,
    "p95": 356,
    "p99": 489,
    "p99.9": 892,
    "p99.99": 1245,
    "max": 1567
  },
  "count": 50000
}
```

## Testing

```bash
# Run all tests
cargo test

# Run with output
cargo test -- --nocapture

# Run specific test module
cargo test config_tests
cargo test protocol_tests
cargo test tracker_tests
```

## Acknowledgments

- tokio team for the async runtime
- tungstenite for WebSocket implementation
- HdrHistogram for latency measurement tools
