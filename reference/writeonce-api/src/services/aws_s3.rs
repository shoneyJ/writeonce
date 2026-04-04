use crate::services::error::S3ExampleError;
use actix_web::web::Bytes;
use aws_config::BehaviorVersion;
use aws_sdk_s3 as s3;
use serde_json::{Error, Value};

pub async fn init_s3_client() -> Result<s3::Client, s3::Error> {
    let config = aws_config::defaults(BehaviorVersion::latest()).load().await;

    // Create the S3 client
    let s3_client = s3::Client::new(&config);

    Ok(s3_client)
}

pub async fn download_object(
    client: &s3::Client,
    bucket_name: &str,
    key: &str,
) -> Result<s3::operation::get_object::GetObjectOutput, S3ExampleError> {
    client
        .get_object()
        .bucket(bucket_name)
        .key(key)
        .send()
        .await
        .map_err(S3ExampleError::from)
}

/// Reads a JSON file from the specified S3 bucket and key.
pub async fn read_json_from_s3(
    client: &s3::Client,
    bucket: &str,
    key: &str,
) -> Result<Value, S3ExampleError> {
    // Get the object from S3
    //  let object = download_object(client, bucket, key).await?;

    let result = client
        .get_object()
        .bucket(bucket)
        .key(key)
        .response_content_type("application/json")
        .send()
        .await?;

    let bytes = result
        .body
        .collect()
        .await
        .map_err(S3ExampleError::from_byte_stream_error)?
        .into_bytes();
    let response = std::str::from_utf8(&bytes).map_err(S3ExampleError::from_utf_error)?;
    let json_value = serde_json::from_str(&response).map_err(S3ExampleError::from_json_error)?;

    Ok(json_value)
}

pub async fn image_from_s3(
    client: &s3::Client,
    bucket: &str,
    key: &str,
) -> Result<(String, Bytes), S3ExampleError> {
    let result = client.get_object().bucket(bucket).key(key).send().await?;

    let content_type = result
        .content_type()
        .unwrap_or("application/octet-stream")
        .to_string();

    let bytes = result
        .body
        .collect()
        .await
        .map_err(S3ExampleError::from_byte_stream_error)?
        .into_bytes();
    Ok((content_type, bytes))
}

pub async fn read_markdown_from_s3(
    client: &s3::Client,
    bucket: &str,
    key: &str,
) -> Result<String, S3ExampleError> {
    let result = client
        .get_object()
        .bucket(bucket)
        .key(key)
        .response_content_type("text/plain")
        .send()
        .await?;

    let bytes = result
        .body
        .collect()
        .await
        .map_err(S3ExampleError::from_byte_stream_error)?
        .into_bytes();
    let response = String::from_utf8(bytes.to_vec()).map_err(S3ExampleError::from_utf8_error)?;

    Ok(response)
}
