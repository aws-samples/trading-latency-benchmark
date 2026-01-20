//! WebSocket Client Module
//!
//! Manages WebSocket connections and orchestrates the test loop.
//! Supports both TLS and non-TLS connections using tokio-tungstenite.

use crate::config::Config;
use crate::protocol::{self, IncomingMessage};
use crate::tracker::LatencyTracker;

use futures_util::{SinkExt, StreamExt};
use thiserror::Error;
use tokio_tungstenite::{
    connect_async,
    tungstenite::{Error as WsError, Message},
};

/// Client error types
#[derive(Debug, Error)]
pub enum ClientError {
    #[error("WebSocket error: {0}")]
    WebSocket(#[from] WsError),

    #[error("JSON error: {0}")]
    Json(#[from] serde_json::Error),

    #[error("TLS error: {0}")]
    Tls(String),

    #[error("Connection closed unexpectedly")]
    ConnectionClosed,

    #[error("Authentication failed")]
    AuthenticationFailed,

    #[error("Protocol error: {0}")]
    Protocol(String),
}

/// Client state machine states
#[derive(Debug, Clone, PartialEq)]
enum ClientState {
    Connecting,
    Authenticating,
    Subscribing,
    Testing,
    Closed,
}

/// High-frequency trading WebSocket client
pub struct HftClient {
    config: Config,
    tracker: LatencyTracker,
    state: ClientState,
    current_pair_index: usize,
    current_client_id: Option<String>,
}

impl HftClient {
    /// Create a new HFT client with the given configuration
    pub fn new(config: Config) -> Self {
        let tracker = LatencyTracker::new(config.test_size, config.host.clone());
        
        Self {
            config,
            tracker,
            state: ClientState::Connecting,
            current_pair_index: 0,
            current_client_id: None,
        }
    }

    /// Connect to the WebSocket server and run the test loop
    pub async fn connect(&mut self) -> Result<(), ClientError> {
        let url = self.config.websocket_url();
        log::info!("Connecting to {}", url);

        if self.config.use_ssl {
            self.connect_tls(&url).await
        } else {
            self.connect_plain(&url).await
        }
    }

    /// Connect without TLS
    async fn connect_plain(&mut self, url: &str) -> Result<(), ClientError> {
        let (ws_stream, _) = connect_async(url).await?;
        log::info!("WebSocket connection established (non-TLS)");
        
        self.run_test_loop(ws_stream).await
    }

    /// Connect with TLS
    async fn connect_tls(&mut self, url: &str) -> Result<(), ClientError> {
        // For TLS connections, we use the native-tls connector
        let connector = native_tls::TlsConnector::builder()
            .build()
            .map_err(|e| ClientError::Tls(e.to_string()))?;
        
        let (ws_stream, _) = tokio_tungstenite::connect_async_tls_with_config(
            url,
            None,
            false,
            Some(tokio_tungstenite::Connector::NativeTls(connector)),
        )
        .await?;
        
        log::info!("WebSocket connection established (TLS)");
        
        self.run_test_loop(ws_stream).await
    }

    
    
    async fn run_test_loop<S>(&mut self, ws_stream: S) -> Result<(), ClientError>
    // Generic over `S` so it works with both TLS and non-TLS WebSocket streams.
    where
        S: StreamExt<Item = Result<Message, WsError>>   // `StreamExt<Item = Result<Message, WsError>>`: Can read messages (enables `read.next().await`)
            + SinkExt<Message, Error = WsError>         // `SinkExt<Message, Error = WsError>`: Can write messages (enables `write.send(...).await`)
            + Unpin,                                    // `Unpin`: Can be safely moved in memory after being pinned (required for `.split()` and `.await`)
    {
        // Split the bidirectional WebSocket into separate read/write halves
        // This allows concurrent read/write operations
        let (mut write, mut read) = ws_stream.split();

        // Transition to Authenticating state and send API token to server
        self.state = ClientState::Authenticating;
        let auth_msg = protocol::auth_message(self.config.api_token);
        log::debug!("Sending: {}", auth_msg);
        write.send(Message::Text(auth_msg)).await?;

        // Main message processing loop - continuously reads incoming WebSocket messages
        // until the connection closes. The flow is:
        // Auth → AUTHENTICATED → Subscribe → SUBSCRIPTIONS →
        //   Send Order → BOOKED → Send Cancel → DONE → (repeat, measuring RTT each cycle)
        while let Some(msg_result) = read.next().await {
            match msg_result {
                // Text messages are parsed and delegated to handle_message()
                // which implements the state machine (AUTHENTICATED, SUBSCRIPTIONS, BOOKED, DONE, ERROR)
                Ok(Message::Text(text)) => {
                    log::debug!("Received: {}", text);
                    self.handle_message(&text, &mut write).await?;
                }
                // Server initiated close - gracefully exit the loop
                Ok(Message::Close(frame)) => {
                    log::info!("Connection closed: {:?}", frame);
                    self.state = ClientState::Closed;
                    break;
                }
                // WebSocket keepalive - respond with Pong
                Ok(Message::Ping(data)) => {
                    write.send(Message::Pong(data)).await?;
                }
                // Ignore other message types (Binary, Pong, Frame)
                Ok(_) => {
                }
                // WebSocket error - log and propagate
                Err(e) => {
                    log::error!("WebSocket error: {}", e);
                    return Err(ClientError::WebSocket(e));
                }
            }
        }

        Ok(())
    }

    /// Handle an incoming message based on current state
    async fn handle_message<S>(
        &mut self,
        text: &str,
        write: &mut futures_util::stream::SplitSink<S, Message>,
    ) -> Result<(), ClientError>
    where
        S: SinkExt<Message, Error = WsError> + Unpin,
    {
        let msg: IncomingMessage = match protocol::parse_message(text) {
            Ok(m) => m,
            Err(e) => {
                log::warn!("Failed to parse message: {} - {}", e, text);
                return Ok(());
            }
        };

        match msg.msg_type.as_str() {
            "AUTHENTICATED" => {
                log::info!("Authentication successful");
                self.state = ClientState::Subscribing;
                
                // Send subscribe message
                let sub_msg = protocol::subscribe_message();
                log::debug!("Sending: {}", sub_msg);
                write.send(Message::Text(sub_msg)).await?;
            }
            "SUBSCRIPTIONS" => {
                log::info!("Subscription successful, starting test loop");
                self.state = ClientState::Testing;
                
                // Start the test loop by sending first order
                self.send_order(write).await?;
            }
            "BOOKED" => {
                if let Some(client_id) = &msg.client_id {
                    if let Some(rtt) = self.tracker.record_order_response(client_id) {
                        log::debug!("Order RTT: {} ns", rtt);
                    }
                    
                    // Send cancel for this order
                    let instrument_code = msg.instrument_code
                        .as_deref()
                        .map(|s| s.to_string())
                        .unwrap_or_else(|| self.current_pair());
                    self.send_cancel(client_id, &instrument_code, write).await?;
                }
            }
            "DONE" => {
                if let Some(client_id) = &msg.client_id {
                    if let Some(rtt) = self.tracker.record_cancel_response(client_id) {
                        log::debug!("Cancel RTT: {} ns", rtt);
                    }
                }
                
                // Check if we should print stats
                if self.tracker.should_print_stats() {
                    self.tracker.print_stats();
                }
                
                // Stop if test_size reached, otherwise continue
                if self.tracker.is_test_complete() {
                    log::info!(
                        "Test completed. Reached TEST_SIZE: {}. Closing connection.",
                        self.config.test_size
                    );
                    write.send(Message::Close(None)).await?;
                    self.state = ClientState::Closed;
                } else {
                    self.send_order(write).await?;
                }
            }
            "ERROR" => {
                log::error!("Received error from server: {}", text);
                if self.state == ClientState::Authenticating {
                    return Err(ClientError::AuthenticationFailed);
                }
            }
            other => {
                log::debug!("Received unknown message type: {}", other);
            }
        }

        Ok(())
    }

    /// Send a CREATE_ORDER message
    async fn send_order<S>(
        &mut self,
        write: &mut futures_util::stream::SplitSink<S, Message>,
    ) -> Result<(), ClientError>
    where
        S: SinkExt<Message, Error = WsError> + Unpin,
    {
        let client_id = protocol::generate_client_id();
        let instrument_code = self.current_pair();
        
        let order_msg = protocol::create_buy_order(&instrument_code, &client_id);
        
        self.tracker.record_order_sent(client_id.clone());
        self.current_client_id = Some(client_id);
        
        log::debug!("Sending: {}", order_msg);
        write.send(Message::Text(order_msg)).await?;
        
        // Rotate to next trading pair
        self.rotate_pair();
        
        Ok(())
    }

    /// Send a CANCEL_ORDER message
    async fn send_cancel<S>(
        &mut self,
        client_id: &str,
        instrument_code: &str,
        write: &mut futures_util::stream::SplitSink<S, Message>,
    ) -> Result<(), ClientError>
    where
        S: SinkExt<Message, Error = WsError> + Unpin,
    {
        let cancel_msg = protocol::cancel_order(instrument_code, client_id);
        
        self.tracker.record_cancel_sent(client_id.to_string());
        
        log::debug!("Sending: {}", cancel_msg);
        write.send(Message::Text(cancel_msg)).await?;
        
        Ok(())
    }

    /// Get the current trading pair
    fn current_pair(&self) -> String {
        self.config.coin_pairs
            .get(self.current_pair_index)
            .cloned()
            .unwrap_or_else(|| "BTC_EUR".to_string())
    }

    /// Rotate to the next trading pair
    fn rotate_pair(&mut self) {
        if !self.config.coin_pairs.is_empty() {
            self.current_pair_index = (self.current_pair_index + 1) % self.config.coin_pairs.len();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_new_client() {
        let config = Config::default();
        let client = HftClient::new(config);
        assert_eq!(client.state, ClientState::Connecting);
        assert_eq!(client.current_pair_index, 0);
    }

    #[test]
    fn test_current_pair() {
        let config = Config::default();
        let client = HftClient::new(config);
        assert_eq!(client.current_pair(), "BTC_USDT");
    }

    #[test]
    fn test_rotate_pair() {
        let config = Config::default();
        let mut client = HftClient::new(config);
        
        assert_eq!(client.current_pair(), "BTC_USDT");
        client.rotate_pair();
        assert_eq!(client.current_pair(), "BTC_CHF");
        client.rotate_pair();
        assert_eq!(client.current_pair(), "BTC_EUR");
        client.rotate_pair();
        assert_eq!(client.current_pair(), "BTC_USDC");
        client.rotate_pair();
        assert_eq!(client.current_pair(), "BTC_USDT");
    }
}
