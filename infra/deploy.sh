#!/bin/bash
# deploy.sh — run from the development machine to build, copy, and restart
set -e

SERVER="writeonce.de"
REMOTE_DIR="/opt/writeonce"

echo "Building release binary..."
cargo build --release -p wo-rt --bin writeonce

echo "Copying binary to server..."
scp target/release/writeonce "$SERVER:$REMOTE_DIR/writeonce.new"

echo "Syncing content..."
rsync -az --delete content/ "$SERVER:$REMOTE_DIR/content/"

echo "Syncing templates..."
rsync -az --delete templates/ "$SERVER:$REMOTE_DIR/templates/"

echo "Syncing static assets..."
rsync -az --delete static/ "$SERVER:$REMOTE_DIR/static/"

echo "Swapping binary and restarting service..."
ssh "$SERVER" "
    sudo mv $REMOTE_DIR/writeonce.new $REMOTE_DIR/writeonce
    sudo chmod +x $REMOTE_DIR/writeonce
    sudo chown writeonce:writeonce $REMOTE_DIR/writeonce
    sudo systemctl restart writeonce
"

echo "Checking status..."
ssh "$SERVER" "sudo systemctl status writeonce --no-pager"

echo "Deploy complete."
