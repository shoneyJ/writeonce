#!/bin/bash

# Exit on error
set -e

# Define variables
BUILD_IMAGE="blog-app-build"
NGINX_CONTAINER_NAME="writeonce-webserver"

# Step 1: Build Angular application
echo "Building Angular application..."
docker build -t $BUILD_IMAGE -f Dockerfile .

echo "Copying build artifacts..."
 docker run --rm $BUILD_IMAGE sh -c "tar -czf - -C browser . " |  docker exec -i writeonce-webserver tar -xzf - -C /usr/share/nginx/html/


echo "Restarting Nginx container..."
docker restart $NGINX_CONTAINER_NAME

echo "Deployment completed successfully."
