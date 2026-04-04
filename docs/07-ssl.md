# SSL and Deployment

## Problem

In [01-problem.md](./01-problem.md), the infrastructure overhead was identified — multiple repos, AWS dependencies, separate deployment pipelines. But one problem went unaddressed: the server-side infrastructure that sits in front of the application — nginx reverse proxy, SSL certificates, systemd service management, and deployment to the production host.

Currently this requires manual SSH, manual nginx config, manual certbot runs. For a single-binary platform, the deployment should be as simple as the architecture.

## Target

Given:
- SSH access to `writeonce.de` is configured
- nginx exists at the default path `/etc/nginx/`
- The writeonce binary listens on a local port (e.g., `127.0.0.1:3000`)

The deployment pipeline should:
1. Build the binary
2. Copy it to the server
3. Create/update the systemd service
4. Restart the service
5. Configure nginx as a reverse proxy
6. Obtain and auto-renew SSL certificates via Let's Encrypt

## Systemd Service

The writeonce binary runs as a systemd service for automatic restart, logging, and boot-start.

### Service File

```ini
# /etc/systemd/system/writeonce.service
[Unit]
Description=writeonce content platform
After=network.target

[Service]
Type=simple
User=writeonce
Group=writeonce
WorkingDirectory=/opt/writeonce
ExecStart=/opt/writeonce/writeonce
Restart=on-failure
RestartSec=5
StandardOutput=journal
StandardError=journal

# Security hardening
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=/opt/writeonce/data
PrivateTmp=true

[Install]
WantedBy=multi-user.target
```

### Directory Layout on Server

```
/opt/writeonce/
  writeonce              # the binary
  content/               # article .json + .md files
  data/                  # derived .seg + .idx (rebuilt on start)
  templates/             # .htmlx templates
  static/                # CSS, images
```

### Service Management

```bash
# Install / update
sudo systemctl daemon-reload
sudo systemctl enable writeonce
sudo systemctl restart writeonce

# Check status
sudo systemctl status writeonce
journalctl -u writeonce -f
```

## Nginx Reverse Proxy

Nginx sits in front of the writeonce binary, handling SSL termination and proxying requests to `127.0.0.1:3000`.

### Nginx Config

```nginx
# /etc/nginx/sites-available/writeonce.de
server {
    listen 80;
    server_name writeonce.de www.writeonce.de;
    return 301 https://$server_name$request_uri;
}

server {
    listen 443 ssl http2;
    server_name writeonce.de www.writeonce.de;

    ssl_certificate /etc/letsencrypt/live/writeonce.de/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/writeonce.de/privkey.pem;
    ssl_protocols TLSv1.2 TLSv1.3;
    ssl_ciphers HIGH:!aNULL:!MD5;
    ssl_prefer_server_ciphers on;

    # HSTS
    add_header Strict-Transport-Security "max-age=31536000; includeSubDomains" always;

    location / {
        proxy_pass http://127.0.0.1:3000;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;

        # Keep connections open for database subscriptions
        proxy_http_version 1.1;
        proxy_set_header Connection "";
        proxy_read_timeout 86400s;
        proxy_send_timeout 86400s;
    }

    # Static assets — let nginx serve directly for better caching
    location /static/ {
        alias /opt/writeonce/static/;
        expires 1y;
        add_header Cache-Control "public, immutable";
    }
}
```

### Enable Site

```bash
sudo ln -sf /etc/nginx/sites-available/writeonce.de /etc/nginx/sites-enabled/
sudo nginx -t
sudo systemctl reload nginx
```

## SSL with Let's Encrypt

### Initial Certificate

```bash
sudo apt install certbot python3-certbot-nginx
sudo certbot --nginx -d writeonce.de -d www.writeonce.de
```

Certbot modifies the nginx config to add SSL directives and obtains the certificate.

### Auto-Renewal

Certbot installs a systemd timer that runs twice daily:

```bash
# Check timer
systemctl list-timers | grep certbot

# Manual test
sudo certbot renew --dry-run
```

Certificates auto-renew before expiry. Nginx reloads automatically via certbot's deploy hook.

### Deploy Hook for Nginx Reload

```bash
# /etc/letsencrypt/renewal-hooks/deploy/reload-nginx.sh
#!/bin/bash
systemctl reload nginx
```

## Deployment Script

A single script that builds, copies, and restarts:

```bash
#!/bin/bash
# deploy.sh — run from the development machine
set -e

SERVER="writeonce.de"
REMOTE_DIR="/opt/writeonce"

echo "Building release binary..."
cargo build --release -p wo-rt --bin writeonce

echo "Copying binary to server..."
scp target/release/writeonce $SERVER:$REMOTE_DIR/writeonce.new

echo "Syncing content and templates..."
rsync -az --delete content/ $SERVER:$REMOTE_DIR/content/
rsync -az --delete templates/ $SERVER:$REMOTE_DIR/templates/
rsync -az --delete static/ $SERVER:$REMOTE_DIR/static/

echo "Swapping binary and restarting..."
ssh $SERVER "
    sudo mv $REMOTE_DIR/writeonce.new $REMOTE_DIR/writeonce
    sudo systemctl restart writeonce
"

echo "Deployed. Checking status..."
ssh $SERVER "sudo systemctl status writeonce --no-pager"
```

### First-Time Setup

Run once on the server to create the user, directory, and service:

```bash
#!/bin/bash
# setup.sh — run on the server
set -e

# Create user
sudo useradd -r -s /bin/false writeonce

# Create directory
sudo mkdir -p /opt/writeonce/{content,data,templates,static}
sudo chown -R writeonce:writeonce /opt/writeonce

# Install service
sudo cp writeonce.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable writeonce

# Configure nginx
sudo cp writeonce.de.nginx /etc/nginx/sites-available/writeonce.de
sudo ln -sf /etc/nginx/sites-available/writeonce.de /etc/nginx/sites-enabled/
sudo nginx -t
sudo systemctl reload nginx

# SSL
sudo certbot --nginx -d writeonce.de -d www.writeonce.de
```

## What This Replaces

| Before | After |
|--------|-------|
| Pulumi IaC managing Lambda + S3 | `deploy.sh` with scp + rsync |
| AWS Lambda deployment pipeline | `systemctl restart writeonce` |
| S3 bucket for content hosting | `rsync content/` to server |
| Docker Compose for API + DB | Single binary, one systemd service |
| Multiple nginx configs for API + frontend | One nginx config, one proxy_pass |
| Manual SSL setup | `certbot --nginx` with auto-renewal |

## Connection Keepalive for Subscriptions

The nginx config sets `proxy_read_timeout 86400s` (24 hours) to keep persistent connections open for the database subscription model. When a browser visits an article page and the connection transitions to `Subscribed` state, nginx must not timeout and close the upstream connection.

If nginx is removed in the future (the binary handles TLS directly via `rustls`), this concern disappears — the binary owns the socket end-to-end.
