use actix_web::{get,web, HttpResponse,HttpRequest, Responder};
use crate::services::aws_s3;
use crate::config;
use crate::api::valid;


#[get("/image/{sys_title}/{image_name}")]
pub async fn get_image_by_sys_title_file_name( req: HttpRequest,path: web::Path<(String,String)>) -> impl Responder {
    if let Err(response) = valid::validate_token(&req).await {
        return response; // Return Unauthorized response
    }
    let (sys_title, image_name) = path.into_inner();
    let s3_path = format!("{}/images/{}.png",sys_title, image_name);
    
     let client = aws_s3::init_s3_client().await.unwrap();
     let bucket = config::get_aws_bucket_name();
     match aws_s3::image_from_s3(&client, &bucket, &s3_path).await {
        Ok((content_type, bytes)) => {
            HttpResponse::Ok()
                .content_type(content_type) // Set content type
                .body(bytes) // Return the JSON response
        }
        Err(e) => {
            // Handle error, return a 404 or 500 response as appropriate
            eprintln!("Error reading from S3: {}", e);
            HttpResponse::InternalServerError().body("Error reading the article")
        }
    }
}
