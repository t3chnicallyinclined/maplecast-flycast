# -*- coding: utf-8 -*-
"""overseer.py -- rung 0/1 of the observing agent.

Watches what a working agent just produced, and answers one question:
does the knowledge graph ALREADY know something about what it is touching?

No model. No embeddings. Rung 0 is regex extraction plus an index lookup;
rung 1 is a graph walk on the hit. Both are effectively free, which is the
point: a check this cheap can run on every single turn, so it never has to
be decided-about, scheduled, or remembered.

Rung 2 (vector similarity over the 440 note fields, for symptom-shaped input
with no identifier in it) is deliberately NOT here yet. Measure rung 0 first
and find out how much of the problem it already solves.

    import overseer
    overseer.check("let me probe blk+0x3F24 for the object base")
"""
import re
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kb  # noqa: E402

# --- rung 0: what does this domain use as an exact identifier? -------------
# RE talk is unusually precise. When a worker is about to re-derive an
# address it nearly always SAYS the address, which is why a hash lookup
# beats a vector here.
PATTERNS = {
    "routine": re.compile(r"\bloc_8c[0-9a-f]{6}\b", re.I),
    "address": re.compile(r"\b0x[0-9A-Fa-f]{3,8}\b"),
    "srcline": re.compile(r"\b[\w/]+\.(?:rs|py|mjs|cpp|c|h|asm):\d+\b"),
    "symbol": re.compile(r"\b(?:blk|G|H)\+0x[0-9A-Fa-f]{2,6}\b"),
}

# Addresses that carry no information. Extracted constantly, mean nothing.
NOISE = {
    "0x0", "0x00", "0x000", "0x0000", "0x1", "0x01", "0x2", "0x4", "0x8",
    "0x10", "0x100", "0x1000", "0xff", "0xFF", "0xffff", "0xFFFF",
}


def extract(text):
    """Pull every exact identifier out of a blob of agent output."""
    found = {}
    for kind, pat in PATTERNS.items():
        for m in pat.findall(text or ""):
            tok = m.lower() if kind in ("routine", "symbol") else m
            if tok in NOISE:
                continue
            found.setdefault(tok, kind)
    return found


def _q(s):
    return str(s).replace("\\", "\\\\").replace("'", "\\'")


def lookup(token, before=None):
    """Rung 0 + 1. Find KB rows that mention this identifier.

    `before` is an ISO date string. When given, only rows the graph could
    ALREADY have known by then are returned -- which is what makes replay
    evaluation honest rather than retrospective.
    """
    t = _q(token)
    date_gate = " AND date < '%s'" % _q(before) if before else ""

    rows = kb.query(
        "SELECT record::id(id) AS id, status, date, "
        "  string::slice(statement ?? note ?? '', 0, 200) AS claim "
        "FROM finding "
        "WHERE (statement ?? '') CONTAINS '%s' OR (note ?? '') CONTAINS '%s'"
        "%s;" % (t, t, date_gate))

    # rung 1: a routine id IS its PC, so a direct node hit is possible
    if token.startswith("loc_8c"):
        rows += kb.query(
            "SELECT record::id(id) AS id, 'entity' AS status, "
            "  string::slice(note ?? '', 0, 200) AS claim "
            "FROM routine:%s;" % t)
    return rows


#: statuses worth interrupting a worker for, most urgent first
ACTIONABLE = ("ruled_out", "superseded", "masks_only", "confirmed")


def check(text, before=None, max_hits=6):
    """The overseer's whole job.

    Returns rows the worker should see, ordered so that 'we already
    disproved this' outranks 'we already confirmed this'. A ruled_out row is
    the expensive one to rediscover, so it goes first.
    """
    out = []
    for token, kind in extract(text).items():
        for r in lookup(token, before=before):
            r["token"] = token
            r["kind"] = kind
            out.append(r)

    def rank(r):
        s = (r.get("status") or "").lower()
        return (ACTIONABLE.index(s) if s in ACTIONABLE else len(ACTIONABLE),
                r.get("date") or "")

    seen, uniq = set(), []
    for r in sorted(out, key=rank):
        k = (r.get("id"), r.get("token"))
        if k in seen:
            continue
        seen.add(k)
        uniq.append(r)
    return uniq[:max_hits]


def render(rows):
    """Format for injection. Provenance and status are NOT optional.

    The overseer may only emit retrieved rows. It has no channel for
    opinion, by construction -- a confident wrong overseer is worse than
    no overseer.
    """
    if not rows:
        return ""
    lines = ["[overseer] the graph already has rows on what you are touching:"]
    for r in rows:
        lines.append("  %-12s %-11s %s  %s"
                     % (r.get("token", "?"),
                        (r.get("status") or "?").upper(),
                        r.get("date") or "no-date",
                        (r.get("claim") or "").replace("\n", " ")[:140]))
    return "\n".join(lines)


if __name__ == "__main__":
    probe = " ".join(sys.argv[1:]) or "checking blk+0x3F24 and loc_8c0344d4"
    print(render(check(probe)) or "(no hits)")
