# Prod snapshots — preserved uncommitted production content

> **STALE PROD BOX - historical record.** Written when prod was `149.28.44.118`
> (Vultr, hostname `flycast-inputserver-nyc`). Since 2026-09-01 `nobd.net` /
> `play.nobd.net` are served by **rise3** (`15.204.141.58`, `ubuntu@`, key
> `~/.ssh/ovh_maplecast`, passwordless sudo), unit `maplecast-flycast.service`.
> Current server architecture lives in ONE place: forgily-creations
> `plans/rise3_handover.md` section 0 (copy `~/HANDOVER.md` on rise3).
> Read the host facts below as history, never as a deploy target.

Files here are EXACT captures of content found live on prod (`root@149.28.44.118`,
`/var/www/maplecast/`) that was **not present in any git commit** at capture time.
They exist so a later deploy can never silently erase prod-only work (the
2026-04-10 incident class). LF-normalized for clean diffing.

## webgpu-sprite-client.mjs.prod-2026-07-09

Prod's live `webgpu/sprite-client.mjs` as of 2026-07-09. It is git commit
`ba5848893` (an ancestor of `feat/render-replica-live` HEAD) plus a 2-hunk
hand-edit that was never committed: a **capes / intra-assembly z-order fix**.

The prod-only delta (see `webgpu-sprite-client.capes-hotfix.patch`):
- iterate `recs` by index and set `partZ = recs.length - _ri` so **record 0 is
  front-most** (drawn last, on top), instead of `z += r.z`.

**Status: SUPERSEDED in git HEAD, not a regression to chase.** Commit
`260663171` ("correct intra-sprite z-order (record 0 = front)") and the
`2026-06-14` engine-depth work replaced this heuristic with the authoritative
engine `1/W` depth (`owner.engZ`, from node+0xE8), keeping the same
`record 0 = front` rule as a fallback. So git's `sprite-client.mjs` is strictly
newer and encodes the same intent more correctly.

**Live discrepancy (open):** prod serves this OLD sprite-client under the same
`?v=85` cache tag that git's NEWER (engZ) version uses. Prod was never updated
to the engine-z drawer. This is one candidate lead for the residual cape
z-order artifacts observed on `webgpu-test.html?bodytex=local` — though note the
shipping body path there is `render_frame`, whose intra-run emit order is the
other known cape lever (handoff §5 item 2). Deploying git's sprite-client is
out of scope for the wire/relay fix and left for a dedicated, validated pass.
