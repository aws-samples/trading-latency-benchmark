//! Rust HFT Client Library
//!
//! A high-performance, low-latency WebSocket client designed for benchmarking
//! trading system round-trip times. This library implements a continuous
//! order-cancel test loop, measuring the time between sending orders/cancellations
//! and receiving confirmations.
//!
//! # Modules
//!
//! - `config`: Configuration loading and validation from TOML files
//! - `protocol`: Trading protocol message construction and parsing
//! - `tracker`: Latency measurement and histogram-based statistics reporting
//! - `client`: WebSocket connection management and test loop orchestration

pub mod config;
pub mod protocol;
pub mod tracker;
pub mod client;

// Re-export main types for convenience
pub use config::Config;
pub use client::HftClient;
pub use tracker::LatencyTracker;
