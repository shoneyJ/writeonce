#!/bin/sh

set -e  # Exit immediately if a command exits with a non-zero status
apk add --no-cache openssh-client
echo "$SSH_PRIVATE_KEY" > /tmp/remote-login-key.pem
chmod 600 /tmp/remote-login-key.pem # Restrict permissions
ssh-keyscan -H "$SERVER_DNS_OR_IP" >> /tmp/known_hosts 2>/dev/null