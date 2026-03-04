
# OS Tuning Benchmark Results
The following two tables contain the raw data for the OS tuning benchmark results used in the 
`Tick to Trade Latency` Blog Part 2.

## Before OS Tuning (Default Configuration)

| Instance | Min | p50 | p90 | p99 | p99.9 |
|---|---|---|---|---|---|
| m8a.zn.metal | 15.1µs | 17.9µs | 18.8µs | 19.4µs | 20.1µs |
| m8a.zn.12xlarge | 16.9µs | 19.5µs | 20.3µs | 22.8µs | 25.2µs |
| m5zn.metal | 27.0µs | 30.3µs | 31.6µs | 33.0µs | 37.5µs |
| m5zn.12xlarge | 31.8µs | 34.9µs | 36.0µs | 38.9µs | 45.1µs |
| c7i.metal | 17.1µs | 28.0µs | 28.9µs | 30.1µs | 33.5µs |
| c7i.24xlarge | 22.1µs | 29.9µs | 31.3µs | 36.7µs | 41.8µs |
| c6in.metal | 20.2µs | 22.3µs | 22.8µs | 23.2µs | 26.4µs |
| c7a.48xlarge | 25.4µs | 27.2µs | 28.0µs | 30.8µs | 43.6µs |
| c8g.metal | 18.8µs | 20.5µs | 21.4µs | 22.1µs | 23.4µs |



## After OS Tuning

| Instance | Min | p50 | p90 | p99 | p99.9 |
|---|---|---|---|---|---|
| m8a.zn.metal | 15.3µs | 17.7µs | 18.4µs | 18.9µs | 19.4µs |
| m8a.zn.12xlarge | 16.6µs | 18.9µs | 19.6µs | 20.3µs | 22.7µs |
| m5zn.metal | 18.4µs | 20.3µs | 20.9µs | 21.2µs | 22.2µs |
| m5zn.12xlarge | 21.3µs | 23.6µs | 24.2µs | 25.9µs | 31.0µs |
| c7i.metal | 18.3µs | 20.3µs | 21.0µs | 21.4µs | 22.0µs |
| c7i.24xlarge | 19.8µs | 21.7µs | 22.3µs | 26.2µs | 30.9µs |
| c6in.metal | 18.8µs | 20.9µs | 21.5µs | 21.8µs | 22.7µs |
| c7a.48xlarge | 24.8µs | 28.2µs | 28.8µs | 32.3µs | 38.9µs |
| c8g.metal | 19.8µs | 21.4µs | 22.0µs | 22.7µs | 23.4µs |