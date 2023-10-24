use actix_web::{web, App, HttpRequest, HttpServer, Responder, get, post, HttpResponse, Error};
use actix_web_actors::ws;

mod websocket;
use self::websocket::WebSocketActor;

#[post("/private/account/user/balances/{user_id}/{currency}/{amount}")]
async fn add_balances(path: actix_web::web::Path<(i32, String, i32)>) -> impl Responder {
    let (user_id, _currency, _amount) = path.into_inner();
    HttpResponse::Ok().body(format!("User Created and balances sent for user: {}", user_id))
}

#[get("/")]
async fn ws_index(req: HttpRequest, stream: web::Payload) -> Result<HttpResponse, Error> {
    let resp = ws::start(WebSocketActor::new(), &req, stream);
    println!("{:?}", resp);
    resp
}



#[actix_web::main]
async fn main() -> std::io::Result<()> {
    HttpServer::new(|| {
        App::new()
        .service(add_balances)
        .service(ws_index)
    })
        .bind(("127.0.0.1", 8888))?
        .run()
        .await
}

