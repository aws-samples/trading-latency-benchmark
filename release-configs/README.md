# Configuration Guide

This directory contains sample configuration files for the Trading Latency Benchmark components.

## Quick Start

### Java Client

1. Place `java-client-config.properties` in the same directory as `ExchangeFlow-*.jar`
2. Rename it to `config.properties`
3. Update the `HOST` setting to point to your server
4. Run: `java -jar ExchangeFlow-v1.0.0.jar`

### Rust Mock Trading Server

1. Place `rust-server-configuration.toml` in the same directory as `mock-trading-server`
2. Rename it to `configuration.toml`
3. Update settings as needed
4. Run: `./mock-trading-server-v1.0.0-<platform>`

## Configuration Options

### Java Client (`config.properties`)

#### Required Settings
- **HOST** - Server hostname or IP address (e.g., `localhost`, `192.168.1.100`)
- **WEBSOCKET_PORT** - WebSocket port to connect to (default: `8888`)

#### Performance Settings
- **TEST_SIZE** - Number of round-trip tests to run (default: `50000`)
- **EXCHANGE_CLIENT_COUNT** - Number of concurrent clients (default: `1`)
- **WARMUP_COUNT** - Number of warmup iterations (default: `2`)
- **USE_IOURING** - Use io_uring on Linux for better performance (default: `false`)

#### Trading Settings
- **COINPAIRS** - Comma-separated list of trading pairs to test
- **API_TOKEN** - Authentication token (must match server)

#### SSL/TLS Settings
- **USE_SSL** - Enable SSL/TLS (default: `false`)
- **KEY_STORE_PATH** - Path to Java keystore file (if USE_SSL=true)
- **KEY_STORE_PASSWORD** - Keystore password
- **CIPHERS** - Comma-separated list of cipher suites

#### Monitoring
- **PING_INTERVAL** - Milliseconds between ping checks (default: `60000`)

### Rust Server (`configuration.toml`)

#### Required Settings
- **port** - Port to listen on (default: `8888`)
- **host** - Interface to bind to (`"0.0.0.0"` for all interfaces)

#### SSL/TLS Settings
- **use_ssl** - Enable TLS with rustls (default: `false`)
- **private_key** - Path to PEM-formatted private key file
- **cert_chain** - Path to PEM-formatted certificate chain file
- **cipher_list** - ⚠️ Ignored with rustls (uses secure defaults)

## Common Configurations

### Local Testing (No SSL)

**Java Client:**
```properties
HOST=localhost
WEBSOCKET_PORT=8888
USE_SSL=false
TEST_SIZE=10000
EXCHANGE_CLIENT_COUNT=1
```

**Rust Server:**
```toml
port=8888
host="127.0.0.1"
use_ssl=false
```

### Production (With SSL)

**Java Client:**
```properties
HOST=your-server.example.com
WEBSOCKET_PORT=443
USE_SSL=true
KEY_STORE_PATH=keystore.p12
KEY_STORE_PASSWORD=your-secure-password
```

**Rust Server:**
```toml
port=443
host="0.0.0.0"
use_ssl=true
private_key="/path/to/privkey.pem"
cert_chain="/path/to/fullchain.pem"
```

## Troubleshooting

### Java Client Won't Connect
- Verify `HOST` and `WEBSOCKET_PORT` match the server
- Check if server is running: `telnet <HOST> <PORT>`
- Verify `API_TOKEN` matches between client and server
- Check firewall rules

### Rust Server Won't Start
- Check if port is already in use: `lsof -i :<port>` or `netstat -an | grep <port>`
- Verify configuration.toml exists in the same directory
- If using SSL, verify certificate/key files exist and are readable
- Check server logs for detailed error messages

### SSL/TLS Issues
- Ensure certificate and key files are in PEM format
- Verify file paths are correct and readable
- For Java client: Ensure keystore.p12 is in PKCS12 format
- For Rust server: rustls requires PEM format certificates

## File Locations

Both applications look for config files in the **current working directory**:
- Java: Looks for `config.properties` in working directory, then classpath
- Rust: Looks for `configuration.toml` in working directory

**Example setup:**
```
my-benchmark/
├── ExchangeFlow-v1.0.0.jar
├── config.properties
├── mock-trading-server-v1.0.0-linux-x86_64
└── configuration.toml
```

Then run from the `my-benchmark/` directory:
```bash
# Terminal 1 - Start server
./mock-trading-server-v1.0.0-linux-x86_64

# Terminal 2 - Start client
java -jar ExchangeFlow-v1.0.0.jar
```

## Advanced Configuration

### High-Performance Setup
```properties
# Java Client
USE_IOURING=true              # Linux only, requires kernel 5.10+
EXCHANGE_CLIENT_COUNT=8       # Multiple concurrent clients
TEST_SIZE=100000              # Longer test runs
WARMUP_COUNT=5                # More warmup iterations
```

### Multi-Client Load Testing
```properties
# Run multiple instances with different API_TOKEN values
# Instance 1: API_TOKEN=3001
# Instance 2: API_TOKEN=3002
# Instance 3: API_TOKEN=3003
```

## See Also

- [Main README](../README.md) - Complete project documentation
- [RUSTLS_MIGRATION.md](../RUSTLS_MIGRATION.md) - SSL/TLS implementation details
- [CROSS_PLATFORM_COMPATIBILITY.md](../CROSS_PLATFORM_COMPATIBILITY.md) - Platform support
