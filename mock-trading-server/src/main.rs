use actix_web::{App, HttpServer, Responder, post, HttpResponse};

// Create a POST endpoint that has the following spec:
// addBalances Request: 
//http://localhost:8888/private/account/user/balances/3002/USDT/100000000 POST
//addBalances Response:
//User Created and balances sent for user:3002
//OK: 200


#[post("/private/account/user/balances/{user_id}/{currency}/{amount}")]
async fn add_balances(path: actix_web::web::Path<(i32, String, i32)>) -> impl Responder {
    let (user_id, currency, amount) = path.into_inner();
    HttpResponse::Ok().body(format!("User Created and balances sent for user: {}", user_id))
}

#[actix_web::main]
async fn main() -> std::io::Result<()> {
    HttpServer::new(|| {
        App::new().service(add_balances)
    })
        .bind(("127.0.0.1", 8888))?
        .run()
        .await
}

