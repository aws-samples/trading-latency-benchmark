# HFT Trading Latency Benchmark Client

A high-performance C++ client for measuring trading latency using WebSocket connections. This project implements a latency testing framework for trading systems with precise histogram-based measurements.

## Features

- WebSocket-based communication with trading servers
- High-precision latency measurements using HdrHistogram
- Configurable test parameters
- SSL/TLS support for secure connections
- JSON-based protocol implementation
- Detailed latency statistics reporting

## Prerequisites

- CMake (version 3.27 or higher)
- C++17 compatible compiler
- OpenSSL development libraries
- Git

## Dependencies

The following dependencies are automatically fetched and configured by CMake:
- WebSocket++ (v0.8.2)
- nlohmann/json (v3.11.2)
- HdrHistogram_c (v0.11.8)

## Building

```bash
# Clone the repository
git clone https://github.com/yourusername/trading-latency-benchmark.git
cd trading-latency-benchmark/cpp-client

# Create build directory
mkdir build
cd build

# Configure and build
cmake ..
cmake --build .
```

## System Setup

### macOS

Using Homebrew:
```bash
# Install build tools
brew install cmake
brew install openssl
brew install boost

# Set OpenSSL paths (add to your ~/.zshrc or ~/.bash_profile)
export OPENSSL_ROOT_DIR=$(brew --prefix openssl)
export OPENSSL_INCLUDE_DIR=$OPENSSL_ROOT_DIR/include
export OPENSSL_LIBRARIES=$OPENSSL_ROOT_DIR/lib

# Install Xcode Command Line Tools if not already installed
xcode-select --install
```

### Amazon Linux 2/2023
#### Update system
```bash
sudo yum update -y
```
#### Install development tools
```bash
sudo yum groupinstall "Development Tools" -y
```

#### Install CMake (version 3.27 or higher)
#### Method 1: Using Amazon Linux Extras (AL2)
```bash
sudo amazon-linux-extras enable cmake3
sudo yum install cmake3 -y
```
#### Method 2: Install from source (if you need a newer version)
```bash
wget https://github.com/Kitware/CMake/releases/download/v3.27.0/cmake-3.27.0.tar.gz
tar -xzvf cmake-3.27.0.tar.gz
cd cmake-3.27.0
./bootstrap
make -j$(nproc)
sudo make install
```

#### Install dependencies
```bash
sudo yum install openssl-devel boost-devel -y
```
#### Verify installations
```bash
cmake --version  # Should be 3.27 or higher
g++ --version    # Should support C++17
```

#### Verify System Requirements
```bash
# Check C++ compiler version
g++ --version    # Should support C++17

# Check CMake version
cmake --version  # Should be 3.27 or higher

# Check OpenSSL version
openssl version

# Check Boost version
brew list --versions boost  # On macOS
rpm -q boost-devel         # On Amazon Linux
```

## Usage

```bash
./hft_client [config_file]
```

### Configuration
Create a JSON configuration file with the following structure:

```json
{
    "server_uri": "ws://localhost:8080",
    "use_ssl": false,
    "test_duration_seconds": 60,
    "requests_per_second": 100,
    "warmup_seconds": 5,
    "log_level": "info"
}
```

#### Configuration Parameters
| Parameter | Description | Default |
|-----------|-------------|---------|
| server_uri | WebSocket server endpoint | ws://localhost:8080 |
| use_ssl | Enable/disable SSL/TLS | false |
| test_duration_seconds | Duration of the test in seconds | 60 |
| requests_per_second | Target request rate | 100 |
| warmup_seconds | Warm-up period before measurements | 5 |
| log_level | Logging verbosity (debug/info/warn/error) | info |

## Project Structure
```bash
cpp-client/
├── CMakeLists.txt
├── main.cpp
├── Config.cpp
├── Config.h
├── ExchangeClient.cpp
├── ExchangeClient.h
├── ExchangeClientLatencyTestHandler.cpp
├── ExchangeClientLatencyTestHandler.h
├── ExchangeProtocol.cpp
├── ExchangeProtocol.h
└── Logger.h
```

## Performance Metrics

The client measures and reports the following metrics:

- Round-trip latency (microseconds)
- Percentile distributions (50th, 90th, 99th, 99.9th)
- Message rates (messages/second)
- Error rates and types

## Contributing

1. Fork the repository
2. Create your feature branch (`git checkout -b feature/amazing-feature`)
3. Commit your changes (`git commit -m 'Add some amazing feature'`)
4. Push to the branch (`git push origin feature/amazing-feature`)
5. Open a Pull Request

### Development Guidelines

- Follow C++17 standards
- Use smart pointers for memory management
- Include unit tests for new features
- Document public APIs
- Follow existing code style

## Testing
### Build and run tests (if implemented)
```bash
cd build
cmake .. -DBUILD_TESTING=ON
cmake --build .
ctest
```

## Acknowledgments

- WebSocket++ team for the WebSocket implementation
- nlohmann/json for the JSON library
- HdrHistogram for the latency measurement tools