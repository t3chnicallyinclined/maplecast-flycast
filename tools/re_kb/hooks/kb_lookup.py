# -*- coding: utf-8 -*-
"""kb_lookup.py -- PreToolUse hook: answer from the graph before it gets re-derived.

WHY
---
re_kb/README.md has said "query the KB before re-deriving an address" since
June. Nothing enforced it, because there was no hook of any kind in this repo
(.claude/ contained only agents/). The result is the failure the graph exists
to prevent: a session spends an hour rediscovering that char+0x144 is the
authoritative cell selector, or re-walks a dead end that is already recorded as
one.

So: whenever a tool call mentions an MVC2 address, an SH4 PC or a struct
offset, this looks it up and hands back what is ALREADY known -- including what
has already been RULED OUT, which is the part that saves the most time and the
part no other system stores.

CONTRACT
--------
Reads the hook JSON on stdin, writes a JSON object on stdout carrying
`additionalContext`, exits 0. It is advisory: it NEVER blocks a tool call and
NEVER fails the session. If SurrealDB is not running, if the query errors, if
anything at all goes wrong, it exits 0 silently -- a knowledge aid that can
break your session is worse than no knowledge aid.

Install: see .claude/settings.json (PreToolUse).
Test:    echo '{"tool_name":"Bash","tool_input":{"command":"grep 0x8C268340"}}' \
           | python tools/re_kb/hooks/kb_lookup.py
"""
import json
import os
import re
import sys

# NOTE: urllib.request and base64 are imported LAZILY, inside sql().
#
# This hook runs on EVERY Bash/Grep/Read/Edit call, and the overwhelming
# majority of those name no MVC2 address at all -- they return at the
# `if not (addrs or pcs or offs)` guard without touching the network. Measured
# on this machine: bare interpreter start 72ms, +json/sys/re 79ms,
# +urllib.request/base64 168ms. Importing them at module scope therefore cost
# ~88ms on every tool call to support the minority that need them. Moving them
# into sql() halves the tax on the common path.

URL = os.environ.get("REKB_URL", "http://127.0.0.1:8001/sql")
AUTH = os.environ.get("REKB_AUTH", "root:root")
TIMEOUT = float(os.environ.get("REKB_HOOK_TIMEOUT", "2.5"))
MAX_CHARS = 2600          # keep the injection small; this is a pointer, not a dump
MAX_LINES = 12            # curated lines win this budget; doc restatements fill leftovers

ADDR = re.compile(r"0x[0-9A-Fa-f]{6,8}")
PC = re.compile(r"\bloc_8c[0-9a-f]{6}\b", re.I)
OFFSET = re.compile(r"\+0x[0-9A-Fa-f]{1,4}\b")


def sql(stmt):
    import base64                     # lazy: see the import note at the top
    import urllib.request
    req = urllib.request.Request(
        URL, data=("USE NS re DB kb; " + stmt).encode("utf-8"), method="POST")
    req.add_header("Accept", "application/json")
    req.add_header("Authorization", "Basic " + base64.b64encode(AUTH.encode()).decode())
    with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
        return json.loads(r.read().decode("utf-8"))


def rows(res):
    out = []
    for b in res if isinstance(res, list) else []:
        if isinstance(b, dict) and b.get("status") == "OK" and isinstance(b.get("result"), list):
            out.extend(b["result"])
    return out


def lit(s):
    return "'" + str(s).replace(chr(92), chr(92) * 2).replace("'", chr(92) + "'") + "'"


def trim(s, n):
    s = " ".join(str(s or "").split())
    return s if len(s) <= n else s[:n - 1] + "…"


def main():
    raw = sys.stdin.read()
    try:
        ev = json.loads(raw) if raw.strip() else {}
    except ValueError:
        return 0

    blob = json.dumps(ev.get("tool_input", {}))
    addrs = {a.upper() for a in ADDR.findall(blob)}
    pcs = {p.lower() for p in PC.findall(blob)}
    offs = {o.upper() for o in OFFSET.findall(blob)}
    if not (addrs or pcs or offs):
        return 0

    # cap the fan-out; a grep over a whole disassembly can name hundreds
    addrs = sorted(addrs)[:6]
    pcs = sorted(pcs)[:6]
    offs = sorted(offs)[:6]

    # Two buckets, not one list. CURATED knowledge (a routine, a hand-written
    # address row, a finding, a dead end) always wins the budget; doc-derived
    # address rows are restatements of the same fact scraped out of markdown
    # and only fill space that is left over.
    #
    # This is not cosmetic. `address` is 87% doc-derived and about to grow by
    # ~357 more rows from the pending re-ingest. Measured before this split, a
    # 6-address grep injected TWELVE doc_* restatements and exactly ONE curated
    # routine -- the useful line was buried in noise the reader has to skim.
    strong, weak = [], []
    try:
        if addrs:
            got = rows(sql(
                "SELECT record::id(id) AS id, addr, name, note FROM address "
                "WHERE string::uppercase(addr) IN [%s] LIMIT 40;"
                % ", ".join(lit(a) for a in addrs)))
            by_addr = {}
            for r in got:
                by_addr.setdefault(r.get("addr"), []).append(r)
            for addr in sorted(by_addr):
                cur = [r for r in by_addr[addr]
                       if not str(r.get("id", "")).startswith("doc_")]
                doc = [r for r in by_addr[addr]
                       if str(r.get("id", "")).startswith("doc_")]
                for r in cur[:2]:
                    strong.append("  %s  %s -- %s"
                                  % (addr, r.get("name") or r.get("id"),
                                     trim(r.get("note"), 150)))
                # at most ONE doc restatement per address, and only if no
                # curated row exists for it
                if not cur and doc:
                    weak.append("  %s  (doc) %s"
                                % (addr, trim(doc[0].get("note"), 130)))

            # The curated claims ABOUT that address -- the highest-value hit,
            # and the one a grep of the source will never surface.
            about = rows(sql(
                "SELECT record::id(in) AS f, in.status AS status, "
                "in.statement AS statement FROM about "
                "WHERE record::tb(out)='address' "
                "AND string::uppercase(out.addr) IN [%s] "
                "AND string::starts_with(record::id(in), 'doc_') = false LIMIT 5;"
                % ", ".join(lit(a) for a in addrs)))
            # dedupe by finding id: one finding reachable through two
            # anchors (the slot COUNT table and the slot POINTER table, say)
            # would otherwise be printed twice and eat the line budget.
            seen_f = set()
            for r in about:
                if r.get("f") in seen_f:
                    continue
                seen_f.add(r.get("f"))
                strong.append("  finding:%s [%s] %s"
                              % (r.get("f"), r.get("status"),
                                 trim(r.get("statement"), 170)))
        lines = strong
        if pcs:
            got = rows(sql(
                "SELECT record::id(id) AS id, pc, role, summary FROM routine "
                "WHERE record::id(id) IN [%s] LIMIT 8;"
                % ", ".join(lit(p) for p in pcs)))
            for r in got:
                lines.append("  routine:%s (%s) %s -- %s"
                             % (r.get("id"), r.get("pc"), r.get("role") or "",
                                trim(r.get("summary"), 150)))
        if offs:
            got = rows(sql(
                "SELECT record::id(id) AS id, offset, name, note FROM field "
                "WHERE string::uppercase(offset) IN [%s] LIMIT 8;"
                % ", ".join(lit(o) for o in offs)))
            for r in got:
                lines.append("  %s  %s -- %s"
                             % (r.get("offset"), r.get("name") or r.get("id"),
                                trim(r.get("note"), 120)))

        # The expensive-to-rediscover half: what has already been eliminated.
        terms = addrs + pcs
        if terms:
            clause = " OR ".join(
                "string::contains(string::uppercase(statement), %s)"
                % lit(t.upper()) for t in terms[:6])
            dead = rows(sql(
                "SELECT record::id(id) AS id, status, statement, tried FROM finding "
                "WHERE status IN ['ruled_out','superseded'] AND (%s) LIMIT 5;" % clause))
            for r in dead:
                lines.append("  [%s] %s -- %s"
                             % (r.get("status").upper(), r.get("id"),
                                trim(r.get("tried") or r.get("statement"), 170)))
    except Exception:
        return 0                      # advisory only; never break the session

    # curated first, doc restatements only in whatever budget is left
    lines = (lines + weak)[:MAX_LINES]

    if not lines:
        return 0

    body = "re_kb already records these (query the graph before re-deriving):\n" \
           + "\n".join(lines)
    body = trim(body, MAX_CHARS) if len(body) > MAX_CHARS else body
    body += ("\n(source: tools/re_kb -- ask with tools/re_kb/rekb.sh. "
             "A RULED_OUT row means it was investigated and eliminated; "
             "do not re-walk it without new evidence.)")

    json.dump({"hookSpecificOutput": {"hookEventName": "PreToolUse",
                                      "additionalContext": body}}, sys.stdout)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception:
        sys.exit(0)
