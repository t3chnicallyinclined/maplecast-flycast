#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# r2-sync-recordings.sh — offload dataset recordings to Cloudflare R2, then prune
# local copies. Runs on the RECORDING server (prod VPS) from cron. Decoupled from
# the sim — this never touches the authoritative flycast binary.
#
# Flow:  server writes .mcrec + .mctele locally  ->  this uploads to R2  ->  prunes
#        local files older than RETAIN_DAYS (kept as a short backstop after upload).
#
# PREREQS (one-time on the server):
#   1. Install rclone:            curl https://rclone.org/install.sh | sudo bash
#   2. Configure the R2 remote (NEVER commit these creds; rclone stores them in
#      ~/.config/rclone/rclone.conf, which is outside the repo):
#        rclone config create r2 s3 \
#          provider=Cloudflare \
#          access_key_id=<R2_ACCESS_KEY_ID> \
#          secret_access_key=<R2_SECRET_ACCESS_KEY> \
#          endpoint=https://<ACCOUNT_ID>.r2.cloudflarestorage.com \
#          acl=private \
#          no_check_bucket=true   # REQUIRED: bucket-scoped tokens can't CreateBucket
#   3. Cron, e.g. every 10 min:
#        */10 * * * * /opt/maplecast/deploy/scripts/r2-sync-recordings.sh >> /var/log/mc-r2.log 2>&1
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

REC_DIR="${MAPLECAST_RECORDINGS_DIR:-/var/lib/maplecast/recordings}"
R2_REMOTE="${R2_REMOTE:-r2:mvc2-dataset/recordings}"   # <bucket>/<prefix>
RETAIN_DAYS="${RETAIN_DAYS:-2}"                          # local backstop window after upload

command -v rclone >/dev/null || { echo "[r2-sync] rclone not installed"; exit 1; }
[ -d "$REC_DIR" ] || { echo "[r2-sync] no recordings dir: $REC_DIR"; exit 0; }

# 1. Upload new/changed recordings to R2 (idempotent; re-uploads only what changed).
echo "[r2-sync] $(date -u +%FT%TZ) uploading $REC_DIR -> $R2_REMOTE"
rclone copy "$REC_DIR" "$R2_REMOTE" \
  --include '*.mcrec' --include '*.mctele' --include '*.mctele.zst' \
  --s3-no-check-bucket --transfers 4 --checkers 8 --log-level INFO

# 2. Prune local files older than the backstop window (already uploaded in prior runs).
find "$REC_DIR" -type f \
  \( -name '*.mcrec' -o -name '*.mctele' -o -name '*.mctele.zst' -o -name '*.ckpt' \) \
  -mtime +"$RETAIN_DAYS" -print -delete
echo "[r2-sync] done"
