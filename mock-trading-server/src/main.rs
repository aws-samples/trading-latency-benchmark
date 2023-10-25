use actix_web::middleware::Logger;
use actix_web::{get, post, web, App, Error, HttpRequest, HttpResponse, HttpServer, Responder};
use actix_web_actors::ws;

#[macro_use]
extern crate log;
extern crate env_logger;

mod websocket;
use self::websocket::WebSocketActor;

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
async fn main() -> std::io::Result<()> {
    env_logger::init_from_env(env_logger::Env::default().default_filter_or("info"));

    info!("Starting server on 0.0.0.0:8888");

    HttpServer::new(|| {
        App::new()
            .wrap(Logger::default())
            .service(add_balances)
            .service(ws_index)
    })
    .bind(("0.0.0.0", 8888))?
    .run()
    .await
}
