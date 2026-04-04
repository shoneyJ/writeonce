use writeonce_manage_article_api::models::*;
use lambda_runtime::{service_fn, Error, LambdaEvent};
use serde_json::{json, Value};

async fn handler(event: LambdaEvent<Value>) -> Result<Value, Error> {
    let payload = event.payload;

    let new_article: NewArticle = match serde_json::from_value(payload) {
        Ok(val) => val,
        Err(err) => {
            return Ok(json!({
                "status": "error",
                "message": format!("Invalid payload: {}", err)
            }));
        }
    };

    match Article::upsert(new_article) {
        Ok(article) => {

            return Ok(json!({
                "status": "success",
                "data": article
            }));
        },
        Err(err) =>{
            return Ok(json!({
                "status": "error",
                "message": format!("Invalid payload: {}", err)
            }));

        }
    }
}

#[tokio::main]
async fn main() -> Result<(), Error> {
    lambda_runtime::run(service_fn(handler)).await
}