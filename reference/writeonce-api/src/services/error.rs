use aws_smithy_types::byte_stream::error::Error as AWSByteStream;
use std::{str::Utf8Error, string::FromUtf8Error};

/// S3ExampleError provides a From<T: ProvideErrorMetadata> impl to extract
/// client-specific error details. This serves as a consistent backup to handling
/// specific service errors, depending on what is needed by the scenario.
/// It is used throughout the code examples for the AWS SDK for Rust.

#[derive(Debug)]
pub struct S3ExampleError(String);
impl S3ExampleError {
    pub fn new(value: impl Into<String>) -> Self {
        S3ExampleError(value.into())
    }

    pub fn add_message(self, message: impl Into<String>) -> Self {
        S3ExampleError(format!("{}: {}", message.into(), self.0))
    }
}

impl<T: aws_sdk_s3::error::ProvideErrorMetadata> From<T> for S3ExampleError {
    fn from(value: T) -> Self {
        S3ExampleError(format!(
            "{}: {}",
            value
                .code()
                .map(String::from)
                .unwrap_or("unknown code".into()),
            value
                .message()
                .map(String::from)
                .unwrap_or("missing reason".into()),
        ))
    }
}

impl S3ExampleError {
    pub fn from_utf8_error(err: FromUtf8Error) -> Self {
        S3ExampleError(format!("UTF-8 conversion error: {}", err))
    }
}

impl S3ExampleError {
    pub fn from_byte_stream_error(err: AWSByteStream) -> Self {
        S3ExampleError(format!("UTF-8 conversion error: {}", err))
    }
}

impl S3ExampleError {
    pub fn from_utf_error(err: Utf8Error) -> Self {
        S3ExampleError(format!("UTF-8 conversion error: {}", err))
    }
}

impl S3ExampleError {
    pub fn from_json_error(err: serde_json::Error) -> Self {
        S3ExampleError(format!("json conversion error: {}", err))
    }
}

impl std::error::Error for S3ExampleError {}

impl std::fmt::Display for S3ExampleError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}", self.0)
    }
}
