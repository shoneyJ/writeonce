use std::{env, fs};

use actix_web::{get,HttpResponse, Responder};
use serde_json::{from_str, Value};
// Serve the Swagger UI HTML page

#[get("/open-api-spec")]
// Read the OpenAPI spec from an external JSON file
async fn openapi_spec() -> impl Responder {
    // File path to the OpenAPI spec (can be an absolute or relative path)
    let current_dir = env::current_dir().expect("Failed to get current directory");
    println!("{}", &current_dir.display());
    let file_path = current_dir.join("doc/openapi.json");
  
    // Read the file contents asynchronously
    match fs::read_to_string(file_path) {
        Ok(content) => {
            // Parse the JSON content into a Value
            match from_str::<Value>(&content) {
                Ok(json) => HttpResponse::Ok().json(json), // Return the parsed JSON as response
                Err(_) => HttpResponse::InternalServerError().body("Error parsing OpenAPI JSON"),
            }
        },
        Err(_) => HttpResponse::InternalServerError().body("Error reading OpenAPI JSON file"),
    }
}

#[get("/doc")]
pub async fn swagger_ui() -> impl Responder {
    HttpResponse::Ok()
        .content_type("text/html")
        .body(r#"
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Manage Blog API</title>
    <link rel="stylesheet" type="text/css" href="https://cdnjs.cloudflare.com/ajax/libs/swagger-ui/4.5.0/swagger-ui.css">
    <script src="https://cdnjs.cloudflare.com/ajax/libs/swagger-ui/4.5.0/swagger-ui-bundle.js"></script>
</head>
<body>
    <div id="swagger-ui"></div>
    <script>
      const ui = SwaggerUIBundle({
        url: "./open-api-spec",  // Path to the OpenAPI spec
        dom_id: '#swagger-ui',
        deepLinking: true,
        presets: [
          SwaggerUIBundle.presets.apis,
          SwaggerUIBundle.presets.topbar
        ],
        layout: "BaseLayout",
        requestInterceptor: (request) => {
         const token = localStorage.getItem("auth_token");  // Optionally save token in localStorage
        if (token) {
          request.headers['Authorization'] = 'Bearer ' + token;
        }else {
            // Optionally handle the case where no token is available
            console.log('No token found. Authorization header will not be added.');
          }
        return request;
      },
      // Add an "Authorize" button to handle token input
      authActions: {
        authorize: (authorize) => {
         const token = prompt("Please enter your Bearer token");
          if (token) {
              // Save token to localStorage
              localStorage.setItem("auth_token", token);
              // Manually trigger the authorization process
              authorize({ BearerAuth: { token: token } });
            }
        }
      }
      });
    </script>
</body>
</html>
    "#)
}