use crate::config;
use crate::error_handler::CustomError;
use reqwest::{Client, Url};
use serde_json::Value;

pub async fn get_s3_article(sys_title: String) -> Result<Value, CustomError> {
    let client = Client::new();

    let aws_url = config::get_aws_infra_base_url();
    let base_url = Url::parse(&aws_url).unwrap();

    let token = config::get_api_access_token();
    let url = base_url.join(&format!("/article/{}", sys_title)).unwrap();

    let response = client
        .get(url)
        .bearer_auth(token)
        .send()
        .await
        .map_err(CustomError::from_request_error)?;

    let json_value = response
        .json()
        .await
        .map_err(CustomError::from_request_error)?;

    Ok(json_value)
}
