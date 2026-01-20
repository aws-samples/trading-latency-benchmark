//! Protocol Module Tests
//!
//! Unit tests and property-based tests for the protocol module.
//!
//! Property tests validate:
//! - Property 4: Authentication Message Format
//! - Property 5: Order Message Serialization Completeness
//! - Property 6: Cancel Message Serialization Completeness
//! - Property 7: Message Parsing Extracts Type
//! - Property 8: UUID v4 Generation Validity

use rust_hft_client::protocol;
use proptest::prelude::*;
use uuid::Uuid;

#[test]
fn test_auth_message_structure() {
    let msg = protocol::auth_message(12345);
    let parsed: serde_json::Value = serde_json::from_str(&msg).unwrap();
    
    assert_eq!(parsed["type"], "AUTHENTICATE");
    assert_eq!(parsed["api_token"], "12345");
}

#[test]
fn test_subscribe_message_structure() {
    let msg = protocol::subscribe_message();
    let parsed: serde_json::Value = serde_json::from_str(&msg).unwrap();
    
    assert_eq!(parsed["type"], "SUBSCRIBE");
    assert!(parsed["channels"].is_array());
    assert_eq!(parsed["channels"][0]["name"], "ORDERS");
}

#[test]
fn test_create_order_message_structure() {
    let msg = protocol::create_buy_order("ETH_USD", "client-123");
    let parsed: serde_json::Value = serde_json::from_str(&msg).unwrap();
    
    assert_eq!(parsed["type"], "CREATE_ORDER");
    assert_eq!(parsed["order"]["instrument_code"], "ETH_USD");
    assert_eq!(parsed["order"]["client_id"], "client-123");
    assert_eq!(parsed["order"]["side"], "BUY");
    assert_eq!(parsed["order"]["type"], "LIMIT");
    assert_eq!(parsed["order"]["price"], "1");
    assert_eq!(parsed["order"]["amount"], "1");
    assert_eq!(parsed["order"]["time_in_force"], "GOOD_TILL_CANCELLED");
}

#[test]
fn test_cancel_order_message_structure() {
    let msg = protocol::cancel_order("ETH_USD", "client-123");
    let parsed: serde_json::Value = serde_json::from_str(&msg).unwrap();
    
    assert_eq!(parsed["type"], "CANCEL_ORDER");
    assert_eq!(parsed["instrument_code"], "ETH_USD");
    assert_eq!(parsed["client_id"], "client-123");
}

#[test]
fn test_uuid_generation() {
    let id1 = protocol::generate_client_id();
    let id2 = protocol::generate_client_id();
    
    // UUIDs should be unique
    assert_ne!(id1, id2);
    
    // UUIDs should be valid format (36 chars: 8-4-4-4-12)
    assert_eq!(id1.len(), 36);
    assert_eq!(id2.len(), 36);
}

#[test]
fn test_parse_booked_message() {
    let json = r#"{"type":"BOOKED","client_id":"abc-123","instrument_code":"BTC_EUR"}"#;
    let msg = protocol::parse_message(json).unwrap();
    
    assert_eq!(msg.msg_type, "BOOKED");
    assert_eq!(msg.client_id, Some("abc-123".to_string()));
    assert_eq!(msg.instrument_code, Some("BTC_EUR".to_string()));
}

#[test]
fn test_parse_done_message() {
    let json = r#"{"type":"DONE","client_id":"xyz-789","instrument_code":"ETH_USD"}"#;
    let msg = protocol::parse_message(json).unwrap();
    
    assert_eq!(msg.msg_type, "DONE");
    assert_eq!(msg.client_id, Some("xyz-789".to_string()));
    assert_eq!(msg.instrument_code, Some("ETH_USD".to_string()));
}

// ============================================================================
// Property-Based Tests
// ============================================================================

// Strategy for generating valid instrument codes
fn instrument_code_strategy() -> impl Strategy<Value = String> {
    "[A-Z]{3,4}_[A-Z]{3,4}"
}

// Strategy for generating valid client IDs
fn client_id_strategy() -> impl Strategy<Value = String> {
    "[a-z0-9]{8}-[a-z0-9]{4}-[a-z0-9]{4}-[a-z0-9]{4}-[a-z0-9]{12}"
}

// Strategy for generating message types
fn message_type_strategy() -> impl Strategy<Value = String> {
    prop_oneof![
        Just("AUTHENTICATED".to_string()),
        Just("SUBSCRIPTIONS".to_string()),
        Just("BOOKED".to_string()),
        Just("DONE".to_string()),
        Just("ERROR".to_string()),
    ]
}

proptest! {
    /// Feature: rust-hft-client, Property 4: Authentication Message Format
    /// 
    /// *For any* api_token value, the generated AUTHENTICATE message SHALL be 
    /// valid JSON containing exactly the fields "type" (with value "AUTHENTICATE") 
    /// and "api_token" (with the string representation of the token).
    /// 
    /// **Validates: Requirements 3.1**
    #[test]
    fn prop_auth_message_format(api_token in 1u32..1000000u32) {
        let msg = protocol::auth_message(api_token);
        
        // Property: Message is valid JSON
        let parsed: serde_json::Value = serde_json::from_str(&msg)
            .expect("Auth message should be valid JSON");
        
        // Property: Contains "type" field with value "AUTHENTICATE"
        prop_assert!(parsed["type"] == "AUTHENTICATE", "type should be AUTHENTICATE");
        
        // Property: Contains "api_token" field with string representation
        prop_assert!(parsed["api_token"] == api_token.to_string(), "api_token should match");
        
        // Property: Only contains these two fields
        let obj = parsed.as_object().unwrap();
        prop_assert_eq!(obj.len(), 2);
    }

    /// Feature: rust-hft-client, Property 5: Order Message Serialization Completeness
    /// 
    /// *For any* valid order parameters (instrument_code, client_id, side, price, amount), 
    /// the generated CREATE_ORDER message SHALL be valid JSON containing all required 
    /// fields: type, and nested order object with instrument_code, client_id, side, 
    /// type, price, amount, and time_in_force.
    /// 
    /// **Validates: Requirements 3.3**
    #[test]
    fn prop_order_message_completeness(
        instrument_code in instrument_code_strategy(),
        client_id in client_id_strategy()
    ) {
        let msg = protocol::create_buy_order(&instrument_code, &client_id);
        
        // Property: Message is valid JSON
        let parsed: serde_json::Value = serde_json::from_str(&msg)
            .expect("Order message should be valid JSON");
        
        // Property: Contains "type" field with value "CREATE_ORDER"
        prop_assert!(parsed["type"] == "CREATE_ORDER", "type should be CREATE_ORDER");
        
        // Property: Contains nested "order" object
        prop_assert!(parsed["order"].is_object());
        
        // Property: Order contains all required fields
        let order = &parsed["order"];
        prop_assert!(order["instrument_code"] == instrument_code.as_str(), "instrument_code should match");
        prop_assert!(order["client_id"] == client_id.as_str(), "client_id should match");
        prop_assert!(order["side"].is_string());
        prop_assert!(order["type"].is_string());
        prop_assert!(order["price"].is_string());
        prop_assert!(order["amount"].is_string());
        prop_assert!(order["time_in_force"].is_string());
    }

    /// Feature: rust-hft-client, Property 6: Cancel Message Serialization Completeness
    /// 
    /// *For any* valid cancel parameters (instrument_code, client_id), the generated 
    /// CANCEL_ORDER message SHALL be valid JSON containing all required fields: 
    /// type, client_id, and instrument_code.
    /// 
    /// **Validates: Requirements 3.4**
    #[test]
    fn prop_cancel_message_completeness(
        instrument_code in instrument_code_strategy(),
        client_id in client_id_strategy()
    ) {
        let msg = protocol::cancel_order(&instrument_code, &client_id);
        
        // Property: Message is valid JSON
        let parsed: serde_json::Value = serde_json::from_str(&msg)
            .expect("Cancel message should be valid JSON");
        
        // Property: Contains "type" field with value "CANCEL_ORDER"
        prop_assert!(parsed["type"] == "CANCEL_ORDER", "type should be CANCEL_ORDER");
        
        // Property: Contains required fields
        prop_assert!(parsed["client_id"] == client_id.as_str(), "client_id should match");
        prop_assert!(parsed["instrument_code"] == instrument_code.as_str(), "instrument_code should match");
        
        // Property: Only contains these three fields
        let obj = parsed.as_object().unwrap();
        prop_assert_eq!(obj.len(), 3);
    }

    /// Feature: rust-hft-client, Property 7: Message Parsing Extracts Type
    /// 
    /// *For any* valid JSON message containing a "type" field, parsing SHALL 
    /// correctly extract the message type value.
    /// 
    /// **Validates: Requirements 3.5**
    #[test]
    fn prop_message_parsing_extracts_type(
        msg_type in message_type_strategy(),
        client_id in client_id_strategy(),
        instrument_code in instrument_code_strategy()
    ) {
        // Create a JSON message with the given type
        let json = format!(
            r#"{{"type":"{}","client_id":"{}","instrument_code":"{}"}}"#,
            msg_type, client_id, instrument_code
        );
        
        // Property: Parsing succeeds
        let parsed = protocol::parse_message(&json)
            .expect("Should parse valid JSON message");
        
        // Property: Type is correctly extracted
        prop_assert_eq!(parsed.msg_type, msg_type);
        
        // Property: Optional fields are correctly extracted
        prop_assert_eq!(parsed.client_id, Some(client_id));
        prop_assert_eq!(parsed.instrument_code, Some(instrument_code));
    }

    /// Feature: rust-hft-client, Property 8: UUID v4 Generation Validity
    /// 
    /// *For any* generated client_id, it SHALL be a valid UUID v4 format string 
    /// (8-4-4-4-12 hex digits with version 4 indicator).
    /// 
    /// **Validates: Requirements 3.6**
    #[test]
    fn prop_uuid_generation_validity(_seed in 0u64..1000u64) {
        let client_id = protocol::generate_client_id();
        
        // Property: Length is 36 characters (8-4-4-4-12 with hyphens)
        prop_assert_eq!(client_id.len(), 36);
        
        // Property: Can be parsed as a valid UUID
        let uuid = Uuid::parse_str(&client_id)
            .expect("Generated client_id should be valid UUID");
        
        // Property: Is version 4 (random) UUID
        prop_assert_eq!(uuid.get_version_num(), 4);
    }
}
