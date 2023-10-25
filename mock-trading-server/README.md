# Mock Trading Server
This is a mock trading server for testing the benchmark tool. It respond with dummy data that changes based on your request. 

# Prerequisite
* Rust (tested on 1.71.0)

# Usage
```
cargo run --release
```

A REST API server and websocket server will start on `0.0.0.0:8888`

# Endpoints
## REST:
- `POST /private/account/user/balances/{user_id}/{currency}/{amount}`: Adds balances for a user. Requires user_id, currency, and amount in path parameters.

## WebSocket
- `GET /`: Handles incoming WebSocket connections. The WebSocket handler supports these message types:
  - `AUTHENTICATE` - Authenticates the connection with an API token
  - `SUBSCRIBE` - Subscribe to channels 
  - `CREATE_ORDER` - Create a new order
  - `CANCEL_ORDER` - Cancel an existing order

See `src/websocket_message_types.rs` for the request payload JSON format.

