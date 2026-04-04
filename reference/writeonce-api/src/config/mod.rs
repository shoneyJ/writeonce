// src/config/mod.rs
use std::env;

pub fn init_config() {
    dotenv::dotenv().ok();  // Load environment variables from a .env file
    env::set_var("RUST_LOG", "actix_web=info");
}


pub fn get_server_port() -> u16 {
    // Try to read the `PORT` variable from the environment
    // Default to 8080 if `PORT` is not set
    env::var("PORT")
        .unwrap_or_else(|_| "3001".to_string())
        .parse()
        .expect("PORT must be a valid u16")
}

 pub fn get_api_access_token() -> String {    
    return env::var("API_ACCESS_TOKEN")        
         .expect("API_ACCESS_TOKEN must be set")
 }

 pub fn get_api_access_admin_token() -> String {    
    return env::var("API_ACCESS_ADMIN_TOKEN")        
         .expect("API_ACCESS_POST_TOKEN must be set")
 }

 pub fn get_aws_infra_base_url() -> String {    
    return env::var("AWS_INFRA_BASE_URL")        
         .expect("AWS_INFRA_BASE_URL must be set")
 }

pub fn get_aws_region() -> String {    
   return env::var("AWS_REGION")        
        .expect("AWS_REGION must be set")
}

pub fn get_aws_acess_key_id() -> String {    
    return env::var("AWS_ACCESS_KEY_ID")        
         .expect("AWS_ACCESS_KEY_ID must be set")
 }

 pub fn get_aws_secret_acess_key_id() -> String {    
    return env::var("AWS_SECRET_ACCESS_KEY")        
         .expect("AWS_SECRET_ACCESS_KEY must be set")
 }

 pub fn get_aws_bucket_name() -> String {    
    return env::var("AWS_BUCKET_NAME")        
         .expect("AWS_BUCKET_NAME must be set")
 }

