# -*- coding: utf-8 -*-
"""sync_seed_status.py -- write the graph's decided statuses back into the seeds.

WHY
---
83 and 87 triaged statuses as OVERLAYS: files that run last and UPDATE a status
the original seed had already set. That works only because the overlay runs
after -- the seed still ASSERTS the old value, so the seeds and the graph
disagree about the one field the whole epistemics layer is built on.

Installing the promotion ASSERT (88) turned that disagreement into 29 hard
failures on rebuild: the seeds tried to write `status='confirmed'` for findings
87 had demoted, and off-vocabulary values (`implemented`, `proposed`, `closed`,
`RESOLVED`) that 83 had normalised. The gate was right; the seeds were stale.

This is the same divergence the whole exercise started with -- two stores
disagreeing because a decision was recorded in one and not the other. So the
decision goes back to the source of truth.

WHAT IT TOUCHES
---------------
Only the `status='...'` literal (and `status: "..."` in CONTENT blocks) of a
finding whose live status differs. Nothing else in the seed is rewritten. The
overlay files keep their UPDATE statements and their triage_note -- those are
the AUDIT TRAIL of why a status changed and must not be erased by making the
seed agree.

    PYTHONIOENCODING=utf-8 python tools/re_kb/sync_seed_status.py --dry-run
    PYTHONIOENCODING=utf-8 python tools/re_kb/sync_seed_status.py
"""
import base64
import glob
import io
import json
import os
import re
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
URL = os.environ.get("REKB_URL", "http://127.0.0.1:8001/sql")
AUTH = os.environ.get("REKB_AUTH", "root:root")

#: the triage overlays -- they carry the audit trail, not the original claim
OVERLAYS = {"83_status_triage.surql", "87_evidence_and_demotion.surql"}

sys.path.insert(0, HERE)
from apply_seed import split_statements            # noqa: E402

TARGET = re.compile(r"\s*(?:UPSERT|UPDATE|CREATE)\s+finding:(?P<fid>[A-Za-z0-9_]+)")
#: status='x'  or  status: "x"  (CONTENT blocks use the latter)
STATUS_LIT = re.compile("status" + r"\s*[=:]\s*" + "[\"']" +
                        r"(?P<cur>[A-Za-z_]+)" + "[\"']")


def live_status():
    req = urllib.request.Request(
        URL, data=("USE NS re DB kb; SELECT record::id(id) AS id, status "
                   "FROM finding;").encode("utf-8"), method="POST")
    req.add_header("Accept", "application/json")
    req.add_header("Authorization", "Basic " + base64.b64encode(AUTH.encode()).decode())
    with urllib.request.urlopen(req, timeout=120) as r:
        res = json.loads(r.read().decode("utf-8"))
    out = {}
    for b in res:
        if isinstance(b, dict) and b.get("status") == "OK" and isinstance(b.get("result"), list):
            for row in b["result"]:
                if row.get("id") and row.get("status"):
                    out[row["id"]] = row["status"]
    return out


def main(argv):
    dry = "--dry-run" in argv
    want = live_status()
    if not want:
        print("no findings returned -- is the graph running? tools/re_kb/start.sh")
        return 2
    print("live findings: %d" % len(want))

    changes = []
    for path in sorted(glob.glob(os.path.join(HERE, "*.surql"))):
        name = os.path.basename(path)
        if name in OVERLAYS:
            continue
        text = io.open(path, encoding="utf-8").read()
        orig = text

        # Operate per STATEMENT, using the same splitter apply_seed uses.
        #
        # A regex over the whole file cannot do this: the pattern has to run
        # from `UPSERT finding:x` to its `status=`, and seed statements contain
        # SEMICOLONS AND KEYWORDS INSIDE PROSE ("...; the walker then..."), so
        # any "stop at the first ;" guard truncates the match. That version
        # found 12 of the 29 stale literals and looked like it had worked.
        for stmt in split_statements(text):
            m = TARGET.match(stmt)
            if not m:
                continue
            fid = m.group("fid")
            new = want.get(fid)
            if not new:
                continue
            sm = STATUS_LIT.search(stmt)
            if not sm or sm.group("cur") == new:
                continue
            fixed = stmt.replace(sm.group(0),
                                 sm.group(0).replace(sm.group("cur"), new), 1)
            if fixed != stmt and stmt in text:
                text = text.replace(stmt, fixed, 1)
                changes.append((name, fid, sm.group("cur"), new))

        if text != orig and not dry:
            io.open(path, "w", encoding="utf-8", newline="\n").write(text)

    print("status literals %s: %d" % ("that WOULD change" if dry else "rewritten",
                                      len(changes)))
    for name, fid, cur, new in changes:
        print("   %-46s %-12s -> %s   (%s)" % (fid, cur, new, name))
    if dry:
        print("\n(dry run -- nothing written)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
