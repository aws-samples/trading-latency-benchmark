use serde::Deserialize;

#[derive(Deserialize)]
pub struct AuthRequest {
    //r#type: String, // Not used
    pub api_token: String,
}

#[derive(Deserialize)]
pub struct Channel {
    pub name: String,
}

#[derive(Deserialize)]
pub struct SubscriptionRequest {
    // r#type: String,
    pub channels: Vec<Channel>,
}

#[derive(Deserialize)]
pub struct Order {
    pub instrument_code: String,
    pub client_id: String,
    pub side: String,
    // r#type: String,
    pub price: String, // These are String becasue we only need to do ping-pong, no need to parse it to number
    pub amount: String,
    // time_in_force: String,
}

#[derive(Deserialize)]
pub struct LimitOrderRequest {
    // r#type: String,
    pub order: Order,
}

#[derive(Deserialize)]
pub struct CancelOrderRequest {
    // r#type: String,
    pub client_id: String,
    pub instrument_code: String,
}
