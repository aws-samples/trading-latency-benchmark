//! Configuration Module Tests
//!
//! Unit tests and property-based tests for the config module.
//! 
//! Property tests validate:
//! - Property 1: Configuration Round-Trip Consistency
//! - Property 2: Configuration Default Values
//! - Property 3: WebSocket URL Construction

use rust_hft_client::Config;
use proptest::prelude::*;

#[test]
fn test_default_config_values() {
    let config = Config::default();
    
    assert_eq!(config.coin_pairs, vec!["BTC_USDT", "BTC_CHF", "BTC_EUR", "BTC_USDC"]);
    assert_eq!(config.host, "localhost");
    assert_eq!(config.websocket_port, 8888);
    assert_eq!(config.api_token, 3001);
    assert_eq!(config.test_size, 10);
    assert_eq!(config.exchange_client_count, 1);
    assert_eq!(config.warmup_count, 1);
    assert!(!config.use_ssl);
    assert!(config.key_store_path.is_none());
    assert!(config.key_store_password.is_none());
    assert_eq!(config.ciphers, vec![
        "ECDHE-RSA-AES128-GCM-SHA256",
        "ECDHE-RSA-AES256-GCM-SHA384",
        "TLS_AES_128_GCM_SHA256",
        "TLS_AES_256_GCM_SHA384",
    ]);
    assert_eq!(config.ping_interval, 5000);
    assert_eq!(config.histogram_significant_figures, 5);
    assert_eq!(config.payload_sizes, vec![0, 64, 256, 512, 1024, 1400, 2048]);
}

#[test]
fn test_websocket_url_construction() {
    let mut config = Config::default();
    config.host = "example.com".to_string();
    config.websocket_port = 9999;
    config.use_ssl = false;
    assert_eq!(config.websocket_url(), "ws://example.com:9999");
    
    config.use_ssl = true;
    assert_eq!(config.websocket_url(), "wss://example.com:9999");
}

// ============================================================================
// Property-Based Tests
// ============================================================================

fn hostname_strategy() -> impl Strategy<Value = String> {
    "[a-z][a-z0-9]{0,10}(\\.[a-z][a-z0-9]{0,5}){0,2}"
}

fn port_strategy() -> impl Strategy<Value = u16> {
    1..=65535u16
}

proptest! {
    /// Feature: rust-hft-client, Property 3: WebSocket URL Construction
    ///
    /// **Validates: Requirements 2.1, 2.2**
    #[test]
    fn prop_websocket_url_construction(
        host in hostname_strategy(),
        port in port_strategy(),
        use_ssl in any::<bool>()
    ) {
        let mut config = Config::default();
        config.host = host.clone();
        config.websocket_port = port;
        config.use_ssl = use_ssl;
        
        let url = config.websocket_url();
        
        if use_ssl {
            prop_assert!(url.starts_with("wss://"), "SSL URL should start with wss://");
        } else {
            prop_assert!(url.starts_with("ws://"), "Non-SSL URL should start with ws://");
        }
        
        prop_assert!(url.contains(&host), "URL should contain host");
        prop_assert!(url.contains(&port.to_string()), "URL should contain port");
    }

    /// Feature: rust-hft-client, Property 2: Configuration Default Values
    ///
    /// *For any* partial .properties config, parsing SHALL produce a Config where
    /// missing fields have their defined default values and present fields retain
    /// their specified values.
    ///
    /// **Validates: Requirements 1.3**
    #[test]
    fn prop_config_defaults_preserved(
        api_token in 1000u32..10000u32,
        test_size in 100usize..100000usize
    ) {
        let content = format!(
            "API_TOKEN={}\nTEST_SIZE={}\n",
            api_token, test_size
        );
        
        let config = Config::from_properties_str(&content).unwrap();
        
        prop_assert_eq!(config.api_token, api_token);
        prop_assert_eq!(config.test_size, test_size);
        
        // Missing fields have default values
        prop_assert_eq!(config.host, "localhost");
        prop_assert_eq!(config.websocket_port, 8888);
        prop_assert!(!config.use_ssl);
        prop_assert_eq!(config.warmup_count, 1);
    }

    /// Feature: rust-hft-client, Property 1: Configuration Round-Trip Consistency
    ///
    /// *For any* valid Config written as .properties, parsing SHALL preserve
    /// all specified field values.
    ///
    /// **Validates: Requirements 1.1, 1.2**
    #[test]
    fn prop_config_round_trip(
        host in hostname_strategy(),
        port in port_strategy(),
        api_token in 1000u32..10000u32,
        test_size in 100usize..100000usize,
        use_ssl in any::<bool>()
    ) {
        let content = format!(
            "COINPAIRS=BTC_EUR\nHOST={}\nWEBSOCKET_PORT={}\nAPI_TOKEN={}\nTEST_SIZE={}\nWARMUP_COUNT=1\nUSE_SSL={}\nCIPHERS=AES256-GCM-SHA384\n",
            host, port, api_token, test_size, use_ssl
        );
        
        let config = Config::from_properties_str(&content).unwrap();
        
        prop_assert_eq!(config.host, host);
        prop_assert_eq!(config.websocket_port, port);
        prop_assert_eq!(config.api_token, api_token);
        prop_assert_eq!(config.test_size, test_size);
        prop_assert_eq!(config.use_ssl, use_ssl);
    }
}
