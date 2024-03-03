use actix_web::middleware::Logger;
use actix_web::{get, post, web, App, Error, HttpRequest, HttpResponse, HttpServer, Responder};
use actix_web_actors::ws;
use openssl::ssl::{SslAcceptor, SslMethod, SslFiletype, SslAcceptorBuilder, SslOptions, SslVersion};
use config::Config;
use serde::{Deserialize, Serialize};
use openssl::engine::Engine;
use std::ptr;

#[macro_use]
extern crate log;
extern crate env_logger;

mod websocket;
mod websocket_message_types;
use self::websocket::WebSocketActor;

#[derive(Deserialize, Serialize)]
struct Settings {
    use_ssl: bool,
    use_openssl_engine: bool,
    engine_id: String,
    engine_so_path: String,
    cipher_list: String,
    private_key: String,
    cert_chain: String,
}

#[post("/private/account/user/balances/{user_id}/{currency}/{amount}")]
async fn add_balances(path: actix_web::web::Path<(i32, String, i32)>) -> impl Responder {
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

#[get("/")]
async fn ws_index(req: HttpRequest, stream: web::Payload) -> Result<HttpResponse, Error> {
    info!("Websocket connection received");
    let resp = ws::start(WebSocketActor::new(), &req, stream);
    info!("Websocket response: {:?}", resp);
    resp
}
fn setup_open_ssl_engine(settings: &Settings) {
    let _ = Engine::load_builtin_engines();
    let _ = Engine::register_all_complete();
    let mut e = Engine::by_id(&settings.engine_id).unwrap();
    println!("cmd so path: {}", e.ctrl_cmd_string("SO_PATH", &settings.engine_so_path, 0).unwrap());
    println!("cmd load {}", e.load().unwrap());
    println!("init = {}", e.init().unwrap());
    println!("add = {}", e.add().unwrap());
    println!("cmd set default {}", e.set_default(0xFFFF).unwrap());
    println!("cmd set default ciphers {}",e.set_default_ciphers().unwrap());
    println!("name={}, id={}", e.get_name().unwrap(), e.get_id().unwrap());
}
#[actix_web::main]
async fn main() -> Result<(),  std::io::Error> {
   // iterate_through_engines();
    env_logger::init_from_env(env_logger::Env::default().default_filter_or("info"));
    let config = Config::builder()
                    .add_source(config::File::with_name("configuration.toml"))
                    .build()
                    .unwrap();
    let settings: Settings =  config.try_deserialize::<Settings>().unwrap();

    if settings.use_ssl {
        if settings.use_openssl_engine {
            setup_open_ssl_engine(&settings);
        }
        let mut builder: SslAcceptorBuilder = SslAcceptor::mozilla_intermediate(SslMethod::tls()).unwrap();
        builder.set_max_proto_version(Some(SslVersion::TLS1_2)).unwrap();
        builder.set_private_key_file(&settings.private_key, SslFiletype::PEM).unwrap();
        builder.set_certificate_chain_file(&settings.cert_chain).unwrap();
        builder.set_cipher_list(&settings.cipher_list).unwrap();
        info!("Starting server on 0.0.0.0:8888 using SSL");
        return HttpServer::new(|| { App::new().wrap(Logger::default())
            .service(add_balances)
            .service(ws_index) }).bind_openssl("0.0.0.0:8888", builder)?.run().await;
    } else {
        info!("Starting server on 0.0.0.0:8888 without SSL");
        return HttpServer::new(|| { App::new().wrap(Logger::default())
            .service(add_balances)
            .service(ws_index) }).bind(("0.0.0.0", 8888))?.run().await;
    };
}


