use thiserror::Error;
use diesel::result::Error as DieselError;

#[derive(Debug, Error)]
pub enum CustomError {
    #[error("An I/O error occurred: {0}")]
    Io(#[from] std::io::Error),

    #[error("A parsing error occurred: {0}")]
    Parse(String),

    #[error("A network error occurred: {0}")]
    Network(String),

    #[error("An unknown error occurred.")]
    Unknown,

    #[error("An unknown error occurred.")]
    New(i32, String),

    #[error("Diesel error: {0}")]
    DieselError(#[from] DieselError),
}


impl CustomError {


    pub fn new(err_num:i32,value: impl Into<String>) -> Self {
        CustomError::New(err_num,value.into())
    }

    pub fn from_json_error(err:serde_json::Error) -> Self {
        CustomError::Parse(format!("json conversion error: {}", err))

    }

    pub fn from_request_error(err:reqwest::Error) -> Self {
        CustomError::Parse(format!("json conversion error: {}", err))

    }
   
 }

