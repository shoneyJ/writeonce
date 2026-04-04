#!/bin/sh

if [ -z "$API_PORT" ] || [ -z "$AWS_INFRA_BASE_URL" ] || [ -z "$DATABASE_URL" ] || [ -z "$API_ACCESS_TOKEN" ] || [ -z "$API_ACCESS_ADMIN_TOKEN" ]; then
  echo "Error: Missing required environment variables" 
  exit 1
fi

 echo PORT="$API_PORT"

docker build \
 --build-arg PORT="$API_PORT" \
 --build-arg AWS_INFRA_BASE_URL="$AWS_INFRA_BASE_URL" \
 --build-arg DATABASE_URL="$DATABASE_URL" \
 --build-arg API_ACCESS_TOKEN="$API_ACCESS_TOKEN" \
 --build-arg API_ACCESS_ADMIN_TOKEN="$API_ACCESS_ADMIN_TOKEN" \
 -t "$CI_REGISTRY_IMAGE:$CI_COMMIT_SHORT_SHA" .