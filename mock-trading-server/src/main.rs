use actix_web::middleware::Logger;
use actix_web::{get, web, App, Error, HttpRequest, HttpResponse, HttpServer, Responder};
use actix_web_actors::ws;
use openssl::ssl::{SslAcceptor, SslMethod, SslFiletype};
use config::Config;
use serde::{Deserialize, Serialize};
use std::env;

#[macro_use]
extern crate log;
extern crate env_logger;

mod websocket;
mod websocket_message_types;
use self::websocket::WebSocketActor;

/// Server configuration settings loaded from configuration.toml
#[derive(Deserialize, Serialize, Debug)]
struct Settings {
    /// Whether to use SSL/TLS for secure connections
    use_ssl: bool,
    /// Comma-separated list of cipher suites to use for SSL/TLS
    cipher_list: String,
    /// Path to the private key file for SSL/TLS
    private_key: String,
    /// Path to the certificate chain file for SSL/TLS
    cert_chain: String,
    /// Port to listen on (defaults to 8888 if not specified)
    #[serde(default = "default_port")]
    port: u16,
    /// Host address to bind to (defaults to 0.0.0.0 if not specified)
    #[serde(default = "default_host")]
    host: String,
}

/// Default port to use if not specified in configuration
fn default_port() -> u16 {
    8888
}

/// Default host to use if not specified in configuration
fn default_host() -> String {
    "0.0.0.0".to_string()
}

/// Endpoint to add balances for a user
/// 
/// # Arguments
/// 
/// * `path` - Path parameters containing user_id, currency, and amount
#[get("/private/account/user/balances/{user_id}/{currency}/{amount}")]
async fn add_balances(path: web::Path<(i32, String, i32)>) -> impl Responder {
    let (user_id, currency, amount) = path.into_inner();
    info!(
        "Adding balances for user {}, currency {}, amount {}",
        user_id, currency, amount
    );
    HttpResponse::Ok().body(format!(
        "User Created and balances sent for user: {}",
        user_id
    ))
}

/// WebSocket endpoint for real-time trading communication
/// 
/// # Arguments
/// 
/// * `req` - HTTP request
/// * `stream` - Payload stream
#[get("/")]
async fn ws_index(req: HttpRequest, stream: web::Payload) -> Result<HttpResponse, Error> {
    // Fix the error by using a match statement instead of unwrap_or_else
    let peer_addr = match req.peer_addr() {
        Some(addr) => addr.to_string(),
        None => "unknown".to_string(),
    };
    
    info!("Websocket connection received from: {}", peer_addr);
    let resp = ws::start(WebSocketActor::new(), &req, stream);
    
    match &resp {
        Ok(_) => info!("WebSocket connection established successfully"),
        Err(e) => error!("Failed to establish WebSocket connection: {:?}", e),
    }
    
    resp
}

/// Health check endpoint
#[get("/health")]
async fn health_check() -> impl Responder {
    HttpResponse::Ok().body("OK")
}

#[actix_web::main]
async fn main() -> Result<(), std::io::Error> {
    // Initialize logging
    if env::var("RUST_LOG").is_err() {
        env::set_var("RUST_LOG", "info");
    }
    env_logger::init_from_env(env_logger::Env::default().default_filter_or("info"));
    
    // Load configuration
    let config_result = Config::builder()
        .add_source(config::File::with_name("configuration.toml"))
        .build();
        
    let config = match config_result {
        Ok(cfg) => cfg,
        Err(e) => {
            error!("Failed to load configuration: {}", e);
            return Err(std::io::Error::new(
                std::io::ErrorKind::Other,
                format!("Configuration error: {}", e)
            ));
        }
    };
    
    let settings_result = config.try_deserialize::<Settings>();
    let settings = match settings_result {
        Ok(s) => s,
        Err(e) => {
            error!("Failed to deserialize configuration: {}", e);
            return Err(std::io::Error::new(
                std::io::ErrorKind::Other,
                format!("Configuration deserialization error: {}", e)
            ));
        }
    };
    
    info!("Server configuration: {:?}", settings);
    
    // Create HTTP server with common configuration
    let server = HttpServer::new(|| {
        App::new()
            .wrap(Logger::default())
            .service(add_balances)
            .service(ws_index)
            .service(health_check)
    });
    
    // Bind server with or without SSL based on configuration
    let bind_address = format!("{}:{}", settings.host, settings.port);
    
    if settings.use_ssl {
        info!("Starting server on {} using SSL", bind_address);
        
        // Configure SSL
        let mut builder = match SslAcceptor::mozilla_modern(SslMethod::tls()) {
            Ok(b) => b,
            Err(e) => {
                error!("Failed to create SSL acceptor: {}", e);
                return Err(std::io::Error::new(
                    std::io::ErrorKind::Other,
                    format!("SSL error: {}", e)
                ));
            }
        };
        
        // Set private key
        if let Err(e) = builder.set_private_key_file(&settings.private_key, SslFiletype::PEM) {
            error!("Failed to set private key: {}", e);
            return Err(std::io::Error::new(
                std::io::ErrorKind::Other,
                format!("SSL private key error: {}", e)
            ));
        }
        
        // Set certificate chain
        if let Err(e) = builder.set_certificate_chain_file(&settings.cert_chain) {
            error!("Failed to set certificate chain: {}", e);
            return Err(std::io::Error::new(
                std::io::ErrorKind::Other,
                format!("SSL certificate error: {}", e)
            ));
        }
        
        // Set minimum TLS version to 1.2
        if let Err(e) = builder.set_min_proto_version(Some(openssl::ssl::SslVersion::TLS1_2)) {
            error!("Failed to set minimum TLS version: {}", e);
            return Err(std::io::Error::new(
                std::io::ErrorKind::Other,
                format!("SSL protocol version error: {}", e)
            ));
        }
        
        // Set cipher list
        if let Err(e) = builder.set_cipher_list(&settings.cipher_list) {
            error!("Failed to set cipher list: {}", e);
            return Err(std::io::Error::new(
                std::io::ErrorKind::Other,
                format!("SSL cipher list error: {}", e)
            ));
        }
        
        // Start server with SSL
        server.bind_openssl(&bind_address, builder)?.run().await
    } else {
        info!("Starting server on {} without SSL", bind_address);
        
        // Start server without SSL
        server.bind(&bind_address)?.run().await
    }
}
