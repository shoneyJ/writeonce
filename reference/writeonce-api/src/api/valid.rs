use actix_web::{HttpResponse,HttpRequest};
use crate::config;
pub async fn validate_token(req: &HttpRequest) -> Result<(), HttpResponse> {
    // Get the token from the authorization header
    if let Some(auth_header) = req.headers().get("Authorization") {
        if let Ok(token) = auth_header.to_str() {
            
            let api_token = config::get_api_access_token();
            if token.trim_start_matches("Bearer ") == api_token {
                return Ok(());
            }
        }
    }

    // If token is invalid or not present, return Unauthorized
    Err(HttpResponse::Unauthorized().finish())
}

pub async fn validate_admin_token(req: &HttpRequest) -> Result<(), HttpResponse> {
    // Get the token from the authorization header
    if let Some(auth_header) = req.headers().get("Authorization") {
        if let Ok(token) = auth_header.to_str() {
            
            let api_token = config::get_api_access_admin_token();
            if token.trim_start_matches("Bearer ") == api_token {
                return Ok(());
            }
        }
    }

    // If token is invalid or not present, return Unauthorized
    Err(HttpResponse::Unauthorized().finish())
}