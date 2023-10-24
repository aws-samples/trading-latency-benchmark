use actix::prelude::*;
use actix_web_actors::ws;
use serde_json::{json, Value};
use serde::{Deserialize, Serialize};
use std::time::{SystemTime, UNIX_EPOCH};
use uuid::Uuid;
use rand::Rng;
// Add websocket support for this
//ExchangeClient is connecting via websocket to localhost:8888
//
//channel is active, starting websocket handshaking...
//
//Websocket client is connected
//
//Websocket client is authenticating for 3002
//
//Auth Request: 
//
//{"type":"AUTHENTICATE","api_token":"3002"}
//
//✓ Auth Acked Response:
//
//{type=AUTHENTICATED}
//
//Subscription Request:
//
//{"type":"SUBSCRIBE","channels":[{"name":"ORDERS"}]}
//
//✓ Subscriptions Acked
//
//{"channels":[{"account_id":3002,"name":"TRADING"}],"type":"SUBSCRIPTIONS","time":1698064922324}
//
//Limit Order Request:
//
//{"type":"CREATE_ORDER","order":{"instrument_code":"BTC_USDC","client_id":"4df7275e-6af9-4f2a-b620-7a9f6527a4f0","side":"BUY","type":"LIMIT","price":"1","amount":"1","time_in_force":"GOOD_TILL_CANCELLED"}}
//
//✓ Limit Order Acked:
//
//{"order_book_sequence":25532628695,"side":"SELL","uid":3002,"amount":"1","price":"1","instrument_code":"BTC_USDC","client_id":"4df7275e-6af9-4f2a-b620-7a9f6527a4f0","order_id":"2e81f80a-d6a0-48c0-bf17-4f63ba01d5a1","channel_name":"TRADING","type":"BOOKED","time":2002205959737}
//
//Cancel Order Request: 
//
//{"type":"CANCEL_ORDER","client_id":"4df7275e-6af9-4f2a-b620-7a9f6527a4f0","instrument_code":"BTC_USDC"}

// Message formats
#[derive(Deserialize)]
struct AuthRequest {
    r#type: String,
    api_token: String,
}

#[derive(Deserialize)]
struct Channel {
    name: String,
}

#[derive(Deserialize)]
struct SubscriptionRequest {
    r#type: String,
    channels: Vec<Channel>,
}

#[derive(Deserialize)]
struct Order {
    instrument_code: String,
    client_id: String,
    side: String,
    r#type: String,
    price: String, // These are String becasue we only need to do ping-pong, no need to parse it to number
    amount: String,
    time_in_force: String,
}

#[derive(Deserialize)]
struct LimitOrderRequest {
    r#type: String,
    order: Order,
}

#[derive(Deserialize)]
struct CancelOrderRequest {
    r#type: String,
    client_id: String,
    instrument_code: String,
}
    

pub struct WebSocketActor {
    user_id: Option<String>
}

impl Actor for WebSocketActor {
    type Context = ws::WebsocketContext<Self>;
}

impl WebSocketActor {
    pub fn new() -> Self {
        Self {
            user_id: None
        }
    }
}

impl StreamHandler<Result<ws::Message, ws::ProtocolError>> for WebSocketActor {
    fn handle(&mut self, msg: Result<ws::Message, ws::ProtocolError>, ctx: &mut Self::Context) {
        match msg {
            Ok(ws::Message::Ping(msg)) => ctx.pong(&msg),
            Ok(ws::Message::Text(text)) => {
                println!("Received message: {}", text);

                let payload: Value = serde_json::from_str(&text).unwrap(); 
                let payload_type = payload["type"].as_str().unwrap();

                match payload_type {
                    "AUTHENTICATE" => {
                        // parse the json into AuthRequest
                        let auth_request: AuthRequest = serde_json::from_str(&text).unwrap();
                        self.user_id = Some(auth_request.api_token);

                        ctx.text(json!({"type": "AUTHENTICATED"}).to_string());
                    },
                    "SUBSCRIBE" => {
                        let timestamp = SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_millis();
                        let subscription_request: SubscriptionRequest = serde_json::from_str(&text).unwrap();

                        let output_channels = subscription_request.channels.iter().map(|channel| {
                            json!({
                                "account_id": self.user_id.as_ref().unwrap(),
                                "name": channel.name
                            })
                        }).collect::<Vec<Value>>();

                        ctx.text(json!({
                            "type": "SUBSCRIPTIONS",
                            "channels": output_channels,
                            "time": timestamp,
                        }).to_string());
                    },
                    "CREATE_ORDER" => {
                        let timestamp = SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_millis();
                        let limit_order_request: LimitOrderRequest = serde_json::from_str(&text).unwrap();
                        let mut rng = rand::thread_rng();
                        ctx.text(json!({
                            "type": "BOOKED",
                            "order_book_sequence": rng.gen::<i64>(),
                            "side": limit_order_request.order.side,
                            "uid": self.user_id.as_ref().unwrap(),
                            "amount": limit_order_request.order.amount,
                            "price": limit_order_request.order.price,
                            "instrument_code": limit_order_request.order.instrument_code,
                            "client_id": limit_order_request.order.client_id,
                            "order_id": Uuid::new_v4().to_string(),
                            "channel_name": "TRADING", // This is fixed for testing
                            "time": timestamp,
                        }).to_string());
                    },
                    "CANCEL_ORDER" => {
                        let timestamp = SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_millis();
                        let cancel_order_request: CancelOrderRequest = serde_json::from_str(&text).unwrap();
                        let mut rng = rand::thread_rng();
                        ctx.text(json!({
                            "type": "DONE",
                            "status": "CANCELLED",
                            "order_book_sequence": rng.gen::<i64>(),
                            "uid": self.user_id.as_ref().unwrap(),
                            "instrument_code": cancel_order_request.instrument_code,
                            "client_id": cancel_order_request.client_id,
                            "order_id": Uuid::new_v4().to_string(),
                            "channel_name": "TRADING", // This is fixed for testing
                            "time": timestamp,
                        }).to_string());
                    }
                    _ => {
                        // TODO: log an error
                    }

                }
            },
            Ok(ws::Message::Close(reason)) => {
                println!("Closing connection");
                ctx.close(reason);
                ctx.stop();
            },
            _ => {}
        }
    }
}