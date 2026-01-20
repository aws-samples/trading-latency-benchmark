//! Protocol Module
//!
//! Handles JSON message construction and parsing for the trading protocol.
//! Supports authentication, subscription, order creation, and order cancellation.

use serde::{Deserialize, Serialize};
use uuid::Uuid;

// ============================================================================
// Outgoing Message Types
// ============================================================================

/// Authentication message sent to the server
#[derive(Debug, Serialize)]
pub struct AuthenticateMessage {
    #[serde(rename = "type")]
    pub msg_type: &'static str,
    pub api_token: String,
}

/// Channel subscription specification
#[derive(Debug, Serialize)]
pub struct Channel {
    pub name: String,
}

/// Subscribe message for channel subscriptions
#[derive(Debug, Serialize)]
pub struct SubscribeMessage {
    #[serde(rename = "type")]
    pub msg_type: &'static str,
    pub channels: Vec<Channel>,
}

/// Order details for CREATE_ORDER message
#[derive(Debug, Serialize)]
pub struct Order {
    pub instrument_code: String,
    pub client_id: String,
    pub side: String,
    #[serde(rename = "type")]
    pub order_type: String,
    pub price: String,
    pub amount: String,
    pub time_in_force: String,
}

/// Create order message
#[derive(Debug, Serialize)]
pub struct CreateOrderMessage {
    #[serde(rename = "type")]
    pub msg_type: &'static str,
    pub order: Order,
}

/// Cancel order message
#[derive(Debug, Serialize)]
pub struct CancelOrderMessage {
    #[serde(rename = "type")]
    pub msg_type: &'static str,
    pub client_id: String,
    pub instrument_code: String,
}

// ============================================================================
// Incoming Message Types
// ============================================================================

/// Generic incoming message for parsing server responses
#[derive(Debug, Deserialize)]
pub struct IncomingMessage {
    #[serde(rename = "type")]
    pub msg_type: String,
    pub client_id: Option<String>,
    pub instrument_code: Option<String>,
}

// ============================================================================
// Protocol Builder Functions
// ============================================================================

/// Generate an AUTHENTICATE message
pub fn auth_message(api_token: u32) -> String {
    let msg = AuthenticateMessage {
        msg_type: "AUTHENTICATE",
        api_token: api_token.to_string(),
    };
    serde_json::to_string(&msg).expect("Failed to serialize auth message")
}

/// Generate a SUBSCRIBE message for the ORDERS channel
pub fn subscribe_message() -> String {
    let msg = SubscribeMessage {
        msg_type: "SUBSCRIBE",
        channels: vec![Channel {
            name: "ORDERS".to_string(),
        }],
    };
    serde_json::to_string(&msg).expect("Failed to serialize subscribe message")
}

/// Generate a CREATE_ORDER message for a BUY order
pub fn create_buy_order(instrument_code: &str, client_id: &str) -> String {
    let msg = CreateOrderMessage {
        msg_type: "CREATE_ORDER",
        order: Order {
            instrument_code: instrument_code.to_string(),
            client_id: client_id.to_string(),
            side: "BUY".to_string(),
            order_type: "LIMIT".to_string(),
            price: "1".to_string(),
            amount: "1".to_string(),
            time_in_force: "GOOD_TILL_CANCELLED".to_string(),
        },
    };
    serde_json::to_string(&msg).expect("Failed to serialize create order message")
}

/// Generate a CREATE_ORDER message for a SELL order
pub fn create_sell_order(instrument_code: &str, client_id: &str) -> String {
    let msg = CreateOrderMessage {
        msg_type: "CREATE_ORDER",
        order: Order {
            instrument_code: instrument_code.to_string(),
            client_id: client_id.to_string(),
            side: "SELL".to_string(),
            order_type: "LIMIT".to_string(),
            price: "1".to_string(),
            amount: "1".to_string(),
            time_in_force: "GOOD_TILL_CANCELLED".to_string(),
        },
    };
    serde_json::to_string(&msg).expect("Failed to serialize create order message")
}

/// Generate a CANCEL_ORDER message
pub fn cancel_order(instrument_code: &str, client_id: &str) -> String {
    let msg = CancelOrderMessage {
        msg_type: "CANCEL_ORDER",
        client_id: client_id.to_string(),
        instrument_code: instrument_code.to_string(),
    };
    serde_json::to_string(&msg).expect("Failed to serialize cancel order message")
}

/// Generate a new UUID v4 client ID
pub fn generate_client_id() -> String {
    Uuid::new_v4().to_string()
}

/// Parse an incoming JSON message
pub fn parse_message(json: &str) -> Result<IncomingMessage, serde_json::Error> {
    serde_json::from_str(json)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_auth_message_format() {
        let msg = auth_message(3001);
        let parsed: serde_json::Value = serde_json::from_str(&msg).unwrap();
        assert_eq!(parsed["type"], "AUTHENTICATE");
        assert_eq!(parsed["api_token"], "3001");
    }

    #[test]
    fn test_subscribe_message_format() {
        let msg = subscribe_message();
        let parsed: serde_json::Value = serde_json::from_str(&msg).unwrap();
        assert_eq!(parsed["type"], "SUBSCRIBE");
        assert_eq!(parsed["channels"][0]["name"], "ORDERS");
    }

    #[test]
    fn test_create_buy_order_format() {
        let msg = create_buy_order("BTC_EUR", "test-client-id");
        let parsed: serde_json::Value = serde_json::from_str(&msg).unwrap();
        assert_eq!(parsed["type"], "CREATE_ORDER");
        assert_eq!(parsed["order"]["instrument_code"], "BTC_EUR");
        assert_eq!(parsed["order"]["client_id"], "test-client-id");
        assert_eq!(parsed["order"]["side"], "BUY");
    }

    #[test]
    fn test_cancel_order_format() {
        let msg = cancel_order("BTC_EUR", "test-client-id");
        let parsed: serde_json::Value = serde_json::from_str(&msg).unwrap();
        assert_eq!(parsed["type"], "CANCEL_ORDER");
        assert_eq!(parsed["instrument_code"], "BTC_EUR");
        assert_eq!(parsed["client_id"], "test-client-id");
    }

    #[test]
    fn test_generate_client_id_is_valid_uuid() {
        let client_id = generate_client_id();
        // UUID v4 format: 8-4-4-4-12 hex digits
        assert_eq!(client_id.len(), 36);
        assert!(Uuid::parse_str(&client_id).is_ok());
    }

    #[test]
    fn test_parse_message() {
        let json = r#"{"type":"BOOKED","client_id":"test-id","instrument_code":"BTC_EUR"}"#;
        let msg = parse_message(json).unwrap();
        assert_eq!(msg.msg_type, "BOOKED");
        assert_eq!(msg.client_id, Some("test-id".to_string()));
        assert_eq!(msg.instrument_code, Some("BTC_EUR".to_string()));
    }
}
