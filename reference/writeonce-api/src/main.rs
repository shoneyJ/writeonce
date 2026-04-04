use actix_web::{App, HttpServer};
use writeonce_manage_article_api::db;
use writeonce_manage_article_api::api;
use writeonce_manage_article_api::config;



#[actix_web::main]
async fn main() -> std::io::Result<()>{
    
    config::init_config();
    db::init();
    let port = config::get_server_port();
    println!("Starting server on port {port}");
    HttpServer::new(|| {
        App::new()
        .configure(api::init_routes)
    })
    .bind(("0.0.0.0", port))?
    .run()
    .await
}

