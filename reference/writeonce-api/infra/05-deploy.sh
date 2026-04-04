#!/bin/sh
ssh -v -i /tmp/remote-login-key.pem -o StrictHostKeyChecking=no -o UserKnownHostsFile=/tmp/known_hosts "$SERVER_USER@$SERVER_DNS_OR_IP" << EOF

  docker pull "$CI_REGISTRY_IMAGE:$CI_COMMIT_SHORT_SHA"
  docker stop "$CI_PROJECT_NAME" || true  # Stop any running instance
  docker rm "$CI_PROJECT_NAME" || true    # Remove stopped instance
  docker run -d --name "$CI_PROJECT_NAME" -p $API_PORT:$API_PORT --network writeonce-network "$CI_REGISTRY_IMAGE:$CI_COMMIT_SHORT_SHA"
EOF
