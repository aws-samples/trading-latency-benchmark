//! Configuration Module
//!
//! Handles configuration loading from Java-style .properties files (KEY=VALUE format).
//! This allows the Rust, Java, and C++ clients to share a single config file.

use std::collections::HashMap;
use std::path::Path;
use thiserror::Error;

/// Configuration error types
#[derive(Debug, Error)]
pub enum ConfigError {
    #[error("Failed to read config file: {0}")]
    IoError(#[from] std::io::Error),

    #[error("Failed to parse property '{key}': {reason}")]
    ParseError { key: String, reason: String },
}

/// HFT Client configuration
///
/// All fields map to the shared config.properties format used by the Java and C++ clients.
/// Property names: COINPAIRS, HOST, HTTP_PORT, WEBSOCKET_PORT, API_TOKEN, TEST_SIZE,
/// EXCHANGE_CLIENT_COUNT, WARMUP_COUNT, USE_SSL, KEY_STORE_PATH, KEY_STORE_PASSWORD,
/// CIPHERS, PING_INTERVAL, HISTOGRAM_SIGNIFICANT_FIGURES, PAYLOAD_SIZES
#[derive(Debug, Clone, PartialEq)]
pub struct Config {
    pub coin_pairs: Vec<String>,
    pub host: String,
    pub websocket_port: u16,
    pub api_token: u32,
    pub test_size: usize,
    pub exchange_client_count: usize,
    pub warmup_count: usize,
    pub use_ssl: bool,
    pub key_store_path: Option<String>,
    pub key_store_password: Option<String>,
    pub ciphers: Vec<String>,
    pub ping_interval: u64,
    pub histogram_significant_figures: u8,
    pub payload_sizes: Vec<usize>,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            coin_pairs: vec!["BTC_USDT".into(), "BTC_CHF".into(), "BTC_EUR".into(), "BTC_USDC".into()],
            host: "localhost".into(),
            websocket_port: 8888,
            api_token: 3001,
            test_size: 10,
            exchange_client_count: 1,
            warmup_count: 1,
            use_ssl: false,
            key_store_path: None,
            key_store_password: None,
            ciphers: vec![
                "ECDHE-RSA-AES128-GCM-SHA256".into(),
                "ECDHE-RSA-AES256-GCM-SHA384".into(),
                "TLS_AES_128_GCM_SHA256".into(),
                "TLS_AES_256_GCM_SHA384".into(),
            ],
            ping_interval: 5000,
            histogram_significant_figures: 5,
            payload_sizes: vec![0, 64, 256, 512, 1024, 1400, 2048],
        }
    }
}

impl Config {
    /// Parse a Java-style .properties file into a HashMap.
    /// Skips blank lines and lines starting with '#'.
    fn parse_properties(content: &str) -> HashMap<String, String> {
        let mut map = HashMap::new();
        for line in content.lines() {
            let trimmed = line.trim();
            if trimmed.is_empty() || trimmed.starts_with('#') {
                continue;
            }
            if let Some((key, value)) = trimmed.split_once('=') {
                map.insert(key.trim().to_string(), value.trim().to_string());
            }
        }
        map
    }

    /// Load configuration from a .properties file (Java KEY=VALUE format).
    pub fn load<P: AsRef<Path>>(path: P) -> Result<Self, ConfigError> {
        let content = std::fs::read_to_string(path)?;
        Self::from_properties_str(&content)
    }

    /// Parse config from a properties-format string.
    pub fn from_properties_str(content: &str) -> Result<Self, ConfigError> {
        let props = Self::parse_properties(content);
        let defaults = Config::default();

        Ok(Config {
            coin_pairs: props.get("COINPAIRS")
                .map(|v| v.split(',').map(|s| s.trim().to_string()).collect())
                .unwrap_or(defaults.coin_pairs),
            host: props.get("HOST")
                .cloned()
                .unwrap_or(defaults.host),
            websocket_port: props.get("WEBSOCKET_PORT")
                .map(|v| v.parse().map_err(|_| ConfigError::ParseError {
                    key: "WEBSOCKET_PORT".into(), reason: format!("invalid u16: {}", v),
                }))
                .transpose()?
                .unwrap_or(defaults.websocket_port),
            api_token: props.get("API_TOKEN")
                .map(|v| v.parse().map_err(|_| ConfigError::ParseError {
                    key: "API_TOKEN".into(), reason: format!("invalid u32: {}", v),
                }))
                .transpose()?
                .unwrap_or(defaults.api_token),
            test_size: props.get("TEST_SIZE")
                .map(|v| v.parse().map_err(|_| ConfigError::ParseError {
                    key: "TEST_SIZE".into(), reason: format!("invalid usize: {}", v),
                }))
                .transpose()?
                .unwrap_or(defaults.test_size),
            exchange_client_count: props.get("EXCHANGE_CLIENT_COUNT")
                .map(|v| v.parse().map_err(|_| ConfigError::ParseError {
                    key: "EXCHANGE_CLIENT_COUNT".into(), reason: format!("invalid usize: {}", v),
                }))
                .transpose()?
                .unwrap_or(defaults.exchange_client_count),
            warmup_count: props.get("WARMUP_COUNT")
                .map(|v| v.parse().map_err(|_| ConfigError::ParseError {
                    key: "WARMUP_COUNT".into(), reason: format!("invalid usize: {}", v),
                }))
                .transpose()?
                .unwrap_or(defaults.warmup_count),
            use_ssl: props.get("USE_SSL")
                .map(|v| v.eq_ignore_ascii_case("true"))
                .unwrap_or(defaults.use_ssl),
            key_store_path: props.get("KEY_STORE_PATH").cloned().or(defaults.key_store_path),
            key_store_password: props.get("KEY_STORE_PASSWORD").cloned().or(defaults.key_store_password),
            ciphers: props.get("CIPHERS")
                .map(|v| v.split(',').map(|s| s.trim().to_string()).collect())
                .unwrap_or(defaults.ciphers),
            ping_interval: props.get("PING_INTERVAL")
                .map(|v| v.parse().map_err(|_| ConfigError::ParseError {
                    key: "PING_INTERVAL".into(), reason: format!("invalid u64: {}", v),
                }))
                .transpose()?
                .unwrap_or(defaults.ping_interval),
            histogram_significant_figures: props.get("HISTOGRAM_SIGNIFICANT_FIGURES")
                .map(|v| v.parse().map_err(|_| ConfigError::ParseError {
                    key: "HISTOGRAM_SIGNIFICANT_FIGURES".into(), reason: format!("invalid u8: {}", v),
                }))
                .transpose()?
                .unwrap_or(defaults.histogram_significant_figures),
            payload_sizes: props.get("PAYLOAD_SIZES")
                .map(|v| v.split(',')
                    .map(|s| s.trim().parse::<usize>().map_err(|_| ConfigError::ParseError {
                        key: "PAYLOAD_SIZES".into(), reason: format!("invalid usize in list: {}", s.trim()),
                    }))
                    .collect::<Result<Vec<_>, _>>())
                .transpose()?
                .unwrap_or(defaults.payload_sizes),
        })
    }

    /// Load configuration, searching for config.properties in standard locations.
    ///
    /// Search order:
    /// 1. ./config.properties (current directory)
    /// 2. ../java_client/src/main/resources/config.properties (sibling Java project)
    pub fn load_or_default() -> Self {
        let search_paths = [
            "config.properties",
            "../java_client/src/main/resources/config.properties",
        ];

        for path in &search_paths {
            if Path::new(path).exists() {
                match Self::load(path) {
                    Ok(config) => {
                        log::info!("Loaded configuration from {}", path);
                        return config;
                    }
                    Err(e) => {
                        log::warn!("Failed to parse {}: {}", path, e);
                    }
                }
            }
        }

        log::warn!("No config.properties found, using defaults");
        Self::default()
    }

    /// Generate the WebSocket URL based on configuration
    pub fn websocket_url(&self) -> String {
        let protocol = if self.use_ssl { "wss" } else { "ws" };
        format!("{}://{}:{}", protocol, self.host, self.websocket_port)
    }

    /// Log all configuration parameters for debugging
    pub fn log_config(&self) {
        log::debug!("Configuration:");
        log::debug!("  coin_pairs: {:?}", self.coin_pairs);
        log::debug!("  host: {}", self.host);
        log::debug!("  websocket_port: {}", self.websocket_port);
        log::debug!("  api_token: {}", self.api_token);
        log::debug!("  test_size: {}", self.test_size);
        log::debug!("  exchange_client_count: {}", self.exchange_client_count);
        log::debug!("  warmup_count: {}", self.warmup_count);
        log::debug!("  use_ssl: {}", self.use_ssl);
        log::debug!("  key_store_path: {:?}", self.key_store_path);
        log::debug!("  ciphers: {:?}", self.ciphers);
        log::debug!("  ping_interval: {} ms", self.ping_interval);
        log::debug!("  histogram_significant_figures: {}", self.histogram_significant_figures);
        log::debug!("  payload_sizes: {:?}", self.payload_sizes);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_default_config() {
        let config = Config::default();
        assert_eq!(config.host, "localhost");
        assert_eq!(config.websocket_port, 8888);
        assert_eq!(config.api_token, 3001);
        assert_eq!(config.test_size, 10);
        assert!(!config.use_ssl);
    }

    #[test]
    fn test_parse_java_properties() {
        let content = r#"
COINPAIRS=BTC_USDT,BTC_CHF,BTC_EUR,BTC_USDC
HOST=localhost
WEBSOCKET_PORT=8888
API_TOKEN=3001
TEST_SIZE=10
EXCHANGE_CLIENT_COUNT=1
WARMUP_COUNT=1
USE_SSL=false
# This is a comment
CIPHERS=ECDHE-RSA-AES128-GCM-SHA256,ECDHE-RSA-AES256-GCM-SHA384
PING_INTERVAL=5000
HISTOGRAM_SIGNIFICANT_FIGURES=5
PAYLOAD_SIZES=0,64,256,512,1024,1400,2048
"#;
        let config = Config::from_properties_str(content).unwrap();
        assert_eq!(config.coin_pairs, vec!["BTC_USDT", "BTC_CHF", "BTC_EUR", "BTC_USDC"]);
        assert_eq!(config.host, "localhost");
        assert_eq!(config.websocket_port, 8888);
        assert_eq!(config.api_token, 3001);
        assert_eq!(config.test_size, 10);
        assert_eq!(config.exchange_client_count, 1);
        assert!(!config.use_ssl);
        assert_eq!(config.ciphers, vec!["ECDHE-RSA-AES128-GCM-SHA256", "ECDHE-RSA-AES256-GCM-SHA384"]);
        assert_eq!(config.ping_interval, 5000);
        assert_eq!(config.histogram_significant_figures, 5);
        assert_eq!(config.payload_sizes, vec![0, 64, 256, 512, 1024, 1400, 2048]);
    }

    #[test]
    fn test_parse_partial_properties_uses_defaults() {
        let content = "HOST=example.com\nWEBSOCKET_PORT=9999\n";
        let config = Config::from_properties_str(content).unwrap();
        assert_eq!(config.host, "example.com");
        assert_eq!(config.websocket_port, 9999);
        // Defaults for missing fields
        assert_eq!(config.api_token, 3001);
        assert_eq!(config.test_size, 10);
    }

    #[test]
    fn test_parse_empty_properties_uses_defaults() {
        let config = Config::from_properties_str("").unwrap();
        assert_eq!(config, Config::default());
    }

    #[test]
    fn test_parse_comments_and_blank_lines() {
        let content = "# comment\n\n  # another comment\nHOST=myhost\n";
        let config = Config::from_properties_str(content).unwrap();
        assert_eq!(config.host, "myhost");
    }

    #[test]
    fn test_use_ssl_true() {
        let content = "USE_SSL=true\n";
        let config = Config::from_properties_str(content).unwrap();
        assert!(config.use_ssl);
    }

    #[test]
    fn test_use_ssl_case_insensitive() {
        let content = "USE_SSL=True\n";
        let config = Config::from_properties_str(content).unwrap();
        assert!(config.use_ssl);
    }

    #[test]
    fn test_websocket_url_no_ssl() {
        let config = Config {
            use_ssl: false,
            host: "example.com".to_string(),
            websocket_port: 9999,
            ..Default::default()
        };
        assert_eq!(config.websocket_url(), "ws://example.com:9999");
    }

    #[test]
    fn test_websocket_url_with_ssl() {
        let config = Config {
            use_ssl: true,
            host: "example.com".to_string(),
            websocket_port: 9999,
            ..Default::default()
        };
        assert_eq!(config.websocket_url(), "wss://example.com:9999");
    }

    #[test]
    fn test_key_store_path_parsed() {
        let content = "KEY_STORE_PATH=/home/ec2-user/keystore.p12\nKEY_STORE_PASSWORD=123456\n";
        let config = Config::from_properties_str(content).unwrap();
        assert_eq!(config.key_store_path, Some("/home/ec2-user/keystore.p12".into()));
        assert_eq!(config.key_store_password, Some("123456".into()));
    }
}
