use actix_web::middleware::Logger;
use actix_web::{get, post, web, App, Error, HttpRequest, HttpResponse, HttpServer, Responder};
use actix_web_actors::ws;
use openssl::ssl::{SslAcceptor, SslMethod, SslFiletype};
use config::Config;
use serde::{Deserialize, Serialize};

#[macro_use]
extern crate log;
extern crate env_logger;

mod websocket;
mod websocket_message_types;
use self::websocket::WebSocketActor;

#[derive(Deserialize, Serialize)]
struct Settings {
    use_ssl: bool,
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

#[actix_web::main]
async fn main() -> Result<(),  std::io::Error> {
    env_logger::init_from_env(env_logger::Env::default().default_filter_or("info"));
    let config = Config::builder()
                    .add_source(config::File::with_name("configuration.toml"))
                    .build()
                    .unwrap();
    let settings: Settings =  config.try_deserialize::<Settings>().unwrap();

    if settings.use_ssl {
        let mut builder = SslAcceptor::mozilla_intermediate(SslMethod::tls()).unwrap();
        builder.set_private_key_file(&settings.private_key, SslFiletype::PEM).unwrap();
        builder.set_certificate_chain_file(&settings.cert_chain).unwrap();
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
