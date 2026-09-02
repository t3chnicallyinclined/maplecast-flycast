# -*- coding: utf-8 -*-
"""gen_about_edges.py -- make confirmed findings REACHABLE by the lookup hook.

THE PROBLEM
-----------
Measured 2026-09-01: 86 of 197 `confirmed` findings have no `about` edge.
The PreToolUse hook (hooks/kb_lookup.py) answers "what do we already know about
this address / PC?" by walking address -> about -> finding. A finding with no
`about` edge is in the graph and STRUCTURALLY INVISIBLE to the one access path
that requires the reader to know nothing in advance.

That is worse than it sounds. The hook is the only mechanism that works for a
session which has never heard of re_kb. Knowledge it cannot reach may as well
not be recorded, and the health metric happily counted those 86 as confirmed
knowledge.

WHAT THIS DOES
--------------
For each unreachable confirmed finding, extract the addresses, PCs and struct
offsets ITS OWN TEXT names, and link it to the ones that exist as rows. This is
not inference: the finding already says it is about 0x8C268340; nobody recorded
the edge. Same shape as 82_evidence_recovery (which recovered CITES the text
already named); this recovers ABOUTNESS.

Rules kept deliberately tight:
  * only link to a row that EXISTS -- never create the target (that is how
    dangling edges got made in the first place, see 79/81/84)
  * prefer a CURATED address row over a doc_* scrape of the same address
  * cap the fan-out per finding: a long statement can name 30 addresses, and
    30 about-edges makes the finding noise on every one of them
  * `finding -> about -> finding` is NOT emitted. `about` is already overloaded
    (161 such edges) and that overload is why kb.contradictions() cannot work.

    PYTHONIOENCODING=utf-8 python tools/re_kb/ingest/gen_about_edges.py
    tools/re_kb/apply_seed.py tools/re_kb/86_about_reachability.surql
"""
import base64
import io
import json
import os
import re
import sys
import urllib.request

URL = os.environ.get("REKB_URL", "http://127.0.0.1:8001/sql")
AUTH = os.environ.get("REKB_AUTH", "root:root")
OUT = os.path.join("tools", "re_kb", "86_about_reachability.surql")

ADDR = re.compile(r"0x8[Cc][0-9A-Fa-f]{6}")
PC = re.compile(r"\bloc_8c[0-9a-f]{6}\b", re.I)
OFFSET = re.compile(r"\+0x[0-9A-Fa-f]{2,4}\b")

PER_TYPE = 2                 # per anchor TYPE (routine / address / field).
                             # A long statement can name 30 addresses; 30 about
                             # edges makes that finding noise on every one of them.


def sql(stmt):
    req = urllib.request.Request(
        URL, data=("USE NS re DB kb; " + stmt).encode("utf-8"), method="POST")
    req.add_header("Accept", "application/json")
    req.add_header("Authorization", "Basic " + base64.b64encode(AUTH.encode()).decode())
    with urllib.request.urlopen(req, timeout=120) as r:
        return json.loads(r.read().decode("utf-8"))


def rows(res):
    out, errs = [], []
    for b in res if isinstance(res, list) else []:
        if not isinstance(b, dict):
            continue
        if b.get("status") == "OK":
            if isinstance(b.get("result"), list):
                out.extend(b["result"])
        else:
            errs.append(str(b.get("result"))[:200])
    if errs:
        raise RuntimeError("query failed: " + "; ".join(errs))
    return out


def main():
    # ALL confirmed findings, not just the currently-unreachable ones.
    #
    # Scoping to `count(->about) = 0` looks efficient and is wrong on a second
    # run: a finding linked to two routines is no longer "unreachable", so it
    # is skipped -- and never gains the ADDRESS anchors that make it findable
    # by the lookup people actually perform. RELATE is deduped afterwards, so
    # re-linking an already-linked finding costs nothing.
    unreachable = rows(sql(
        "SELECT record::id(id) AS id, statement, note, summary, title, result "
        "FROM finding WHERE status='confirmed';"))
    print("confirmed findings considered: %d" % len(unreachable))

    # existing anchors
    addr_rows = rows(sql("SELECT record::id(id) AS id, addr FROM address;"))
    by_addr = {}
    for r in addr_rows:
        a = str(r.get("addr") or "").upper()
        if not a:
            continue
        by_addr.setdefault(a, []).append("address:" + r["id"])
    # curated beats a doc_* scrape of the same address
    for a in by_addr:
        by_addr[a].sort(key=lambda i: i.split(":", 1)[1].startswith("doc_"))

    routines = {r["id"].lower() for r in rows(sql("SELECT record::id(id) AS id FROM routine;"))}

    fld_rows = rows(sql("SELECT record::id(id) AS id, offset FROM field;"))
    by_off = {}
    for r in fld_rows:
        o = str(r.get("offset") or "").upper()
        if not o:
            continue
        by_off.setdefault(o, []).append("field:" + r["id"])
    for o in by_off:
        by_off[o].sort(key=lambda i: i.split(":", 1)[1].startswith("doc_"))

    lines = [
        "-- " + "=" * 74,
        "-- 86: reachability -- link confirmed findings to what they are ABOUT",
        "--",
        "-- %d confirmed findings had no `about` edge, so the PreToolUse lookup" % len(unreachable),
        "-- (address/PC -> about -> finding) could not surface them at all. They",
        "-- were counted as confirmed knowledge and were invisible to the only",
        "-- access path that requires no prior knowledge of the graph.",
        "--",
        "-- Every edge below links a finding to an anchor ITS OWN TEXT names and",
        "-- that already exists as a row. Nothing is created; nothing is inferred.",
        "-- GENERATED by ingest/gen_about_edges.py -- re-runnable.",
        "-- " + "=" * 74,
        "USE NS re DB kb;",
        "",
    ]

    linked = 0
    unanchorable = []
    for f in unreachable:
        text = " ".join(str(f.get(k) or "") for k in
                        ("statement", "note", "summary", "title", "result"))
        # BALANCED across anchor types, not first-N-in-order.
        #
        # Taking the first 4 of a single concatenated list starved addresses:
        # a finding naming several PCs filled the cap with routines and never
        # reached its addresses, so the first run produced 152 routine edges
        # and only 20 address edges. Addresses are what a session actually
        # greps, so each type gets its own budget.
        pcs = ["routine:" + p for p in sorted({p.lower() for p in PC.findall(text)})
               if p in routines]
        addrs = [by_addr[a][0] for a in sorted({x.upper() for x in ADDR.findall(text)})
                 if a in by_addr]
        offs = [by_off[o][0] for o in sorted({x.upper() for x in OFFSET.findall(text)})
                if o in by_off]
        targets = pcs[:PER_TYPE] + addrs[:PER_TYPE] + offs[:PER_TYPE]
        if not targets:
            unanchorable.append(f["id"])
            continue
        for t in targets:
            lines.append("RELATE finding:%s->about->%s;" % (f["id"], t))
        linked += 1

    if unanchorable:
        lines.append("")
        lines.append("-- ---- named no existing anchor (%d) -- NOT linked "
                     % len(unanchorable) + "-" * 18)
        for fid in sorted(unanchorable):
            lines.append("--   finding:%s" % fid)

    io.open(OUT, "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")
    print("linked        : %d findings" % linked)
    print("unanchorable  : %d (listed as comments; they name no address, PC or "
          "offset that exists)" % len(unanchorable))
    print("-> %s" % OUT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
