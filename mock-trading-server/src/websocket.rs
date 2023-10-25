use actix::prelude::*;
use actix_web_actors::ws;
use rand::Rng;
use serde_json::{json, Value};
use std::time::{SystemTime, UNIX_EPOCH};
use uuid::Uuid;

use crate::websocket_message_types::*;

pub struct WebSocketActor {
    user_id: Option<String>,
}

impl Actor for WebSocketActor {
    type Context = ws::WebsocketContext<Self>;
}

impl WebSocketActor {
    pub fn new() -> Self {
        Self { user_id: None }
    }
}

impl StreamHandler<Result<ws::Message, ws::ProtocolError>> for WebSocketActor {
    fn handle(&mut self, msg: Result<ws::Message, ws::ProtocolError>, ctx: &mut Self::Context) {
        match msg {
            Ok(ws::Message::Text(text)) => {
                debug!("Received message: {}", text);
                let Ok(payload): Result<Value, _> = serde_json::from_str(&text) else {
                    error!("Payload is invalid JSON: {}", text);
                    return;
                };
                let Some(payload_type) = payload["type"].as_str() else {
                    error!("Payload does not have a 'type' field: {}", payload);
                    return;
                };

                match payload_type {
                    "AUTHENTICATE" => {
                        let auth_request: AuthRequest = serde_json::from_str(&text).unwrap();
                        self.user_id = Some(auth_request.api_token);

                        ctx.text(json!({"type": "AUTHENTICATED"}).to_string());
                    }
                    "SUBSCRIBE" => {
                        let timestamp = SystemTime::now()
                            .duration_since(UNIX_EPOCH)
                            .unwrap()
                            .as_millis();
                        let subscription_request: SubscriptionRequest =
                            serde_json::from_str(&text).unwrap();

                        let output_channels = subscription_request
                            .channels
                            .iter()
                            .map(|channel| {
                                json!({
                                    "account_id": self.user_id.as_ref().unwrap(),
                                    "name": channel.name
                                })
                            })
                            .collect::<Vec<Value>>();

                        ctx.text(
                            json!({
                                "type": "SUBSCRIPTIONS",
                                "channels": output_channels,
                                "time": timestamp,
                            })
                            .to_string(),
                        );
                    }
                    "CREATE_ORDER" => {
                        let timestamp = SystemTime::now()
                            .duration_since(UNIX_EPOCH)
                            .unwrap()
                            .as_millis();
                        let limit_order_request: LimitOrderRequest =
                            serde_json::from_str(&text).unwrap();
                        let mut rng = rand::thread_rng();
                        ctx.text(
                            json!({
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
                            })
                            .to_string(),
                        );
                    }
                    "CANCEL_ORDER" => {
                        let timestamp = SystemTime::now()
                            .duration_since(UNIX_EPOCH)
                            .unwrap()
                            .as_millis();
                        let cancel_order_request: CancelOrderRequest =
                            serde_json::from_str(&text).unwrap();
                        let mut rng = rand::thread_rng();
                        ctx.text(
                            json!({
                                "type": "DONE",
                                "status": "CANCELLED",
                                "order_book_sequence": rng.gen::<i64>(),
                                "uid": self.user_id.as_ref().unwrap(),
                                "instrument_code": cancel_order_request.instrument_code,
                                "client_id": cancel_order_request.client_id,
                                "order_id": Uuid::new_v4().to_string(),
                                "channel_name": "TRADING", // This is fixed for testing
                                "time": timestamp,
                            })
                            .to_string(),
                        );
                    }
                    _ => {
                        error!("Ignoring unknown message type: {}", payload);
                    }
                }
            }
            Ok(ws::Message::Close(reason)) => {
                info!("Closing connection");
                ctx.close(reason);
                ctx.stop();
            }
            _ => {}
        }
    }
}
