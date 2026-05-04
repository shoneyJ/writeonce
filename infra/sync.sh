#!/bin/bash
# sync.sh — sync content, templates, and static assets without rebuilding or restarting
set -e

SERVER="shoney@192.168.0.217"
REMOTE_DIR="/opt/writeonce"

echo "Syncing content..."
rsync -az --delete content/ "$SERVER:/tmp/writeonce-content/"

echo "Syncing templates..."
rsync -az --delete templates/ "$SERVER:/tmp/writeonce-templates/"

echo "Syncing static assets..."
rsync -az --delete static/ "$SERVER:/tmp/writeonce-static/"

echo "Sync complete. Files staged in /tmp/writeonce-{content,templates,static}/"
