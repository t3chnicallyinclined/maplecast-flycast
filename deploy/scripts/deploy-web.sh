#!/bin/bash
# ============================================================================
# MAPLECAST WEB — Deploy static files to VPS
#
# Usage:
#   ./deploy/scripts/deploy-web.sh [VPS_HOST]
#
# Example:
#   ./deploy/scripts/deploy-web.sh root@66.55.128.93
#
# What this does:
#   1. Backs up the current production web files on the VPS
#   2. Syncs web/ directory from git to /var/www/maplecast/ on the VPS
#   3. Verifies the deploy by checking key files exist
#
# SAFETY:
#   - Always creates a timestamped backup before overwriting
#   - Shows a diff summary before deploying
#   - Requires confirmation before proceeding
#   - Backup lives at /var/www/maplecast-backup-YYYYMMDD-HHMMSS/
# ============================================================================

set -euo pipefail

VPS_HOST="${1:-root@66.55.128.93}"
REMOTE_DIR="/var/www/maplecast"
SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "$SCRIPT_DIR/../.." &> /dev/null && pwd )"
LOCAL_WEB="$REPO_ROOT/web"

echo "============================================"
echo "  MAPLECAST WEB DEPLOY"
echo "  Source:  $LOCAL_WEB"
echo "  Target:  $VPS_HOST:$REMOTE_DIR"
echo "============================================"

# Verify local web directory exists
if [ ! -f "$LOCAL_WEB/king.html" ]; then
    echo "ERROR: $LOCAL_WEB/king.html not found"
    exit 1
fi

# Check for uncommitted changes
if ! git -C "$REPO_ROOT" diff --quiet -- web/; then
    echo ""
    echo "WARNING: You have uncommitted changes in web/"
    git -C "$REPO_ROOT" diff --stat -- web/
    echo ""
    read -p "Deploy uncommitted changes? [y/N] " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        echo "Aborted. Commit your changes first."
        exit 1
    fi
fi

# Step 1: Backup current production
TIMESTAMP=$(date +%Y%m%d-%H%M%S)
BACKUP_DIR="/var/www/maplecast-backup-$TIMESTAMP"
echo ""
echo "[1/4] Backing up production to $BACKUP_DIR..."
ssh "$VPS_HOST" "cp -a $REMOTE_DIR $BACKUP_DIR && echo 'Backup created: $(du -sh $BACKUP_DIR | cut -f1)'"

# Step 2: Show what will change
echo ""
echo "[2/4] Files that will be updated:"
# Compare local vs remote by listing both and diffing
LOCAL_FILES=$(cd "$LOCAL_WEB" && find . -type f -name '*.html' -o -name '*.mjs' -o -name '*.js' -o -name '*.css' | sort)
echo "  Local files to deploy:"
echo "$LOCAL_FILES" | while read f; do
    local_size=$(wc -c < "$LOCAL_WEB/$f" 2>/dev/null || echo "0")
    echo "    $f ($local_size bytes)"
done

# Step 3: Confirm
echo ""
read -p "Deploy these files to $VPS_HOST:$REMOTE_DIR? [y/N] " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo "Aborted."
    exit 1
fi

# Step 4: Deploy
echo ""
echo "[3/4] Deploying..."

# Sync HTML files
scp "$LOCAL_WEB/king.html" "$VPS_HOST:$REMOTE_DIR/king.html"
echo "  king.html deployed"

# Sync JS modules
scp "$LOCAL_WEB/js/"*.mjs "$VPS_HOST:$REMOTE_DIR/js/" 2>/dev/null && echo "  js/*.mjs deployed" || echo "  (no .mjs files)"
scp "$LOCAL_WEB/js/"*.js "$VPS_HOST:$REMOTE_DIR/js/" 2>/dev/null && echo "  js/*.js deployed" || echo "  (no .js files)"

# Sync other static files (relay.js etc at root level)
for f in relay.js; do
    if [ -f "$LOCAL_WEB/$f" ]; then
        scp "$LOCAL_WEB/$f" "$VPS_HOST:$REMOTE_DIR/$f"
        echo "  $f deployed"
    fi
done

# Sync skin picker and client settings if they exist
for f in skin-picker.html client-settings.html; do
    if [ -f "$LOCAL_WEB/$f" ]; then
        scp "$LOCAL_WEB/$f" "$VPS_HOST:$REMOTE_DIR/$f"
        echo "  $f deployed"
    fi
done

# Step 5: Verify
echo ""
echo "[4/4] Verifying..."
ssh "$VPS_HOST" "
    echo '  king.html: $(wc -c < $REMOTE_DIR/king.html) bytes'
    echo '  JS modules: $(ls $REMOTE_DIR/js/*.mjs 2>/dev/null | wc -l) files'
    echo '  Backup at: $BACKUP_DIR'
"

echo ""
echo "============================================"
echo "  DEPLOY COMPLETE"
echo "  Backup: $BACKUP_DIR"
echo "  Rollback: ssh $VPS_HOST 'rm -rf $REMOTE_DIR && mv $BACKUP_DIR $REMOTE_DIR'"
echo "============================================"
