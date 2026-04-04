#!/bin/bash
# setup.sh — run once on the server to create user, directory, service, nginx, and SSL
set -e

echo "Creating writeonce user..."
sudo useradd -r -s /bin/false writeonce 2>/dev/null || echo "User already exists"

echo "Creating directory structure..."
sudo mkdir -p /opt/writeonce/{content,data,templates,static}
sudo chown -R writeonce:writeonce /opt/writeonce

echo "Installing systemd service..."
sudo cp writeonce.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable writeonce

echo "Configuring nginx..."
sudo cp writeonce.de.nginx /etc/nginx/sites-available/writeonce.de
sudo ln -sf /etc/nginx/sites-available/writeonce.de /etc/nginx/sites-enabled/
sudo nginx -t
sudo systemctl reload nginx

echo "Setting up SSL with Let's Encrypt..."
sudo apt install -y certbot python3-certbot-nginx
sudo certbot --nginx -d writeonce.de -d www.writeonce.de

echo "Installing certbot reload hook..."
sudo mkdir -p /etc/letsencrypt/renewal-hooks/deploy
sudo tee /etc/letsencrypt/renewal-hooks/deploy/reload-nginx.sh > /dev/null << 'HOOK'
#!/bin/bash
systemctl reload nginx
HOOK
sudo chmod +x /etc/letsencrypt/renewal-hooks/deploy/reload-nginx.sh

echo "Setup complete."
echo "Run deploy.sh from your development machine to deploy the binary."
