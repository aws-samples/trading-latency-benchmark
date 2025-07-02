# AF_XDP Zero-Copy Performance Benchmark

High-performance, low-latency UDP packet replicator using AF_XDP kernel bypass technology for market data distribution.

## Quick Start

### Build
```bash
# Install dependencies (Amazon Linux 2023)
sudo yum install clang libbpf-devel elfutils-libelf-devel -y
sudo yum groupinstall "Development Tools" -y

# Clone and build xdp-tools
git clone https://github.com/xdp-project/xdp-tools.git
cd xdp-tools && ./configure && make && sudo make install

# Build the project
make all
```

### Basic Usage

The packet replicator listens for UDP packets and forwards them to multiple subscribers with zero-copy performance.

#### Current Test Setup

**Server (10.0.0.71)** - Configure network interface and start the packet replicator:
```bash
# Configure network interface
sudo ip link set dev enp39s0 mtu 3498
sudo ethtool -L enp39s0 combined 4

# Start the packet replicator
sudo ./packet_replicator enp39s0 10.0.0.71 9000
```

**Client (10.0.0.34)** - Run market data benchmark:
```bash
# Run trade feed round-trip benchmark
./market_data_provider_client 10.0.0.71 9000 10.0.0.34 9001 10000 10000
```

This runs a market data provider trade feed benchmark that:
- Sends packets to multiplexer at `10.0.0.71:9000`
- Listens for responses on local endpoint `10.0.0.34:9001`
- Sends 10,000 messages at 10,000 msg/sec rate
- Measures round-trip latency performance

#### Verification

**Server (10.0.0.71)** - Verify AF_XDP zero-copy is working:
```bash
# Check AF_XDP statistics for queue 1
ethtool -S enp39s0 | grep queue_1

# Key metrics to look for:
# queue_1_tx_xsk_cnt: 10000          # AF_XDP transmitted packets
# queue_1_tx_xsk_bytes: 1940000      # AF_XDP transmitted bytes  
# queue_1_rx_xdp_redirect: 10000     # XDP redirected packets
# queue_1_rx_cnt: 10017              # Total received packets
```

These statistics confirm that:
- AF_XDP zero-copy TX is active (`tx_xsk_cnt` > 0)
- XDP program is redirecting packets (`rx_xdp_redirect` > 0)  
- Packet processing is working end-to-end

### Commands

- `packet_replicator <interface> <listen_ip> <listen_port>` - Main replicator
- `control_client <replicator_ip> add <dest_ip> <dest_port>` - Add subscriber  
- `control_client <replicator_ip> remove <dest_ip> <dest_port>` - Remove subscriber
- `control_client <replicator_ip> list` - List subscribers
- `test_client <target_ip> <target_port> [count] [message]` - Send test packets

## Components

- **AF_XDP Socket** - Zero-copy kernel bypass networking
- **Packet Replicator** - Multi-threaded UDP packet multiplexer
- **Control Client** - Dynamic subscriber management
- **Test Client** - Traffic generation and testing

## Performance Features

- **Sub-microsecond latency** packet forwarding
- **Zero-copy** direct NIC memory access  
- **Multi-queue** processing with dedicated threads
- **Lock-free** operations in hot path
- **CPU affinity** binding for consistent performance
- **Automatic fallback** to standard sockets if needed

## HFT Optimizations

- Lock-free atomic operations
- Branch prediction hints
- Memory prefetching
- Cache-aligned data structures
- Busy polling with CPU pause instead of sleep

## Troubleshooting

**Permission errors**: Run with `sudo`

**Device busy**: Clean up existing XDP programs:
```bash
sudo ./cleanup.sh
```

**Zero-copy fails**: Automatically falls back to copy mode. Check driver compatibility:
```bash
ethtool -i enp39s0  # Should show i40e, ixgbe, mlx5_core, etc.
```

**No packets received**: Verify ARP resolution:
```bash
cat /proc/net/arp | grep <destination_ip>
```

## Requirements

- Linux kernel 5.4+ (5.10+ recommended)
- AF_XDP compatible network driver
- Root privileges for AF_XDP operations

## License

MIT-0 License
