#!/usr/bin/env bash
# start.sh — start the re_kb graph. Idempotent: safe to run when it is already up.
#
# WHY THIS EXISTS
# ---------------
# The graph is session-bound and nothing started it at boot. Both hooks exit 0
# silently when it is down -- deliberately, so a knowledge aid can never break a
# session -- which means the ENTIRE system goes quiet and looks like it is
# simply finding nothing. Silence is indistinguishable from "no results".
#
# It also has to start from the REPO ROOT: `rocksdb:re_kb_data/re_kb` is a
# RELATIVE path, and SurrealDB does not error when it resolves elsewhere -- it
# silently creates a NEW EMPTY store on the same port. Every query then returns
# nothing, which looks exactly like data loss. This script pins the directory so
# that cannot happen.
#
#   tools/re_kb/start.sh          # start (or report already-running)
#   tools/re_kb/start.sh --status # is it up, and does it have data?
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
URL="${REKB_URL:-http://127.0.0.1:8001/sql}"
PORT="$(printf '%s' "$URL" | sed -E 's#.*:([0-9]+).*#\1#')"

alive() {
  curl -s -m 3 -X POST "$URL" -u "${REKB_AUTH:-root:root}" \
       -H "Accept: application/json" \
       --data-binary "USE NS re DB kb; SELECT count() AS n FROM finding GROUP ALL;" 2>/dev/null \
    | grep -q '"status":"OK"'
}

report() {
  local n
  n="$(curl -s -m 5 -X POST "$URL" -u "${REKB_AUTH:-root:root}" \
        -H "Accept: application/json" \
        --data-binary "USE NS re DB kb; SELECT count() AS n FROM finding GROUP ALL;" \
      | grep -oE '"n":[0-9]+' | head -1 | cut -d: -f2)"
  echo "re_kb up on :$PORT — ${n:-0} findings"
  if [ "${n:-0}" = "0" ]; then
    echo "WARNING: zero findings. The server is probably pointed at an EMPTY store" >&2
    echo "         created in the wrong directory. It must run from $ROOT." >&2
  fi
}

if [ "${1:-}" = "--status" ]; then
  if alive; then report; exit 0; else echo "re_kb DOWN on :$PORT"; exit 1; fi
fi

if alive; then report; exit 0; fi

command -v surreal >/dev/null 2>&1 || {
  echo "surreal not on PATH (expected ~/AppData/Local/SurrealDB/surreal.exe)" >&2; exit 1; }

cd "$ROOT" || exit 1
mkdir -p re_kb_data
echo "starting surreal from $ROOT ..."
nohup surreal start --user root --pass root --bind "127.0.0.1:$PORT" \
      "rocksdb:re_kb_data/re_kb" > re_kb_data/surreal.log 2>&1 &

for _ in $(seq 1 30); do
  sleep 0.5
  if alive; then report; exit 0; fi
done
echo "surreal did not come up in 15s; see re_kb_data/surreal.log" >&2
exit 1
