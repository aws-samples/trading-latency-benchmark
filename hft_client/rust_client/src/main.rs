//! HFT Client Main Entry Point
//!
//! High-performance, low-latency WebSocket client for benchmarking
//! trading system round-trip times.

use rust_hft_client::{Config, HftClient};

#[tokio::main]
async fn main() {
    // Initialize logging
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info"))
        .format_timestamp_millis()
        .init();

    log::info!("Starting HFT Client");

    // Load configuration
    let config = Config::load_or_default();
    config.log_config();

    // Create and run client
    let mut client = HftClient::new(config);
    
    match client.connect().await {
        Ok(()) => {
            log::info!("Client completed successfully");
        }
        Err(e) => {
            log::error!("Client error: {}", e);
            std::process::exit(1);
        }
    }
}
