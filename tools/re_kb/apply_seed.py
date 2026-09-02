# -*- coding: utf-8 -*-
"""apply_seed.py -- apply re_kb seed files ONE STATEMENT AT A TIME.

WHY NOT JUST POST THE FILE
--------------------------
SurrealDB parses a whole POST body as one script. If a statement contains an
unbalanced quote, the parser does not stop -- it keeps consuming the following
statements as string content until the quote closes. The request returns 200,
every returned block says OK, and the swallowed statements simply never
happened.

Measured 2026-09-01: 36_hud_real_ta_root_cause.surql contains 127 statements.
Posting the file returned FOUR OK blocks and created none of the 24 findings
it defines. `rekb.sh @file` reported success. That is how 58 seed findings
across 12 files were missing from the live graph while every apply looked
clean.

Splitting first means one malformed statement can only lose itself, and it
says which one.

    PYTHONIOENCODING=utf-8 python tools/re_kb/apply_seed.py                 # all seeds
    PYTHONIOENCODING=utf-8 python tools/re_kb/apply_seed.py tools/re_kb/36_*.surql
    PYTHONIOENCODING=utf-8 python tools/re_kb/apply_seed.py --dry-run

Server: surreal start --user root --pass root --bind 127.0.0.1:8001 \
          rocksdb:re_kb_data/re_kb     (from the REPO ROOT -- the path is relative)
"""
import base64
import glob
import json
import os
import re
import sys
import urllib.error
import urllib.request

URL = os.environ.get("REKB_URL", "http://127.0.0.1:8001/sql")
AUTH = os.environ.get("REKB_AUTH", "root:root")

KEYWORD = re.compile(r"^\s*(USE|UPSERT|UPDATE|CREATE|INSERT|RELATE|DELETE|REMOVE|DEFINE|SELECT|BEGIN|COMMIT|LET|IF)\b",
                     re.I)


def is_script_scoped(text):
    """True if the file MUST be applied whole, never statement-by-statement.

    `LET $x = ...` binds a variable for the rest of the SCRIPT. Split it up and
    each statement runs in its own request, where the variable does not exist.
    07_dedup_edges.surql is built exactly that way:

        LET $reads = (SELECT in, out FROM reads GROUP BY in, out);
        DELETE reads;
        FOR $p IN $reads { RELATE ($p.in)->reads->($p.out); };

    Run standalone, `DELETE reads;` is not a dedup -- it wipes the table, and
    the FOR that would put the rows back silently restores nothing. Doing this
    on 2026-09-01 emptied `owns` (3233 rows), `has_field`, `lives_at`,
    `part_of`, `instance_of`, `maps_to`, `confirms` and truncated `cites` and
    `about`; they had to be restored from an export. Hence this guard.
    """
    return "LET $" in text


def split_statements(text):
    """Split a .surql file into statements.

    A statement starts on a line beginning with a SurrealQL keyword and runs
    until a line whose stripped form ends in ';' -- which is the convention
    every seed file in this directory follows. Comment lines outside a
    statement are dropped; inside one they are kept verbatim (they may sit
    inside a CONTENT block).
    """
    out, cur = [], []
    for line in text.split("\n"):
        if not cur:
            if line.lstrip().startswith("--") or not line.strip():
                continue
            if KEYWORD.match(line):
                cur = [line]
                if line.rstrip().endswith(";"):
                    out.append("\n".join(cur))
                    cur = []
            # a line that is neither comment, blank, nor a keyword start while
            # no statement is open is a continuation of nothing -- keep it so
            # it shows up as an error rather than vanishing
            elif out:
                out[-1] += "\n" + line
        else:
            cur.append(line)
            if line.rstrip().endswith(";"):
                out.append("\n".join(cur))
                cur = []
    if cur:
        out.append("\n".join(cur) + "   -- UNTERMINATED")
    return [s for s in out if s.strip()]


def post(stmt, timeout=60):
    body = ("USE NS re DB kb;\n" + stmt).encode("utf-8")
    req = urllib.request.Request(URL, data=body, method="POST")
    req.add_header("Accept", "application/json")
    req.add_header("Authorization", "Basic " + base64.b64encode(AUTH.encode()).decode())
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            res = json.loads(r.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        try:
            detail = e.read().decode("utf-8", "replace")
        except Exception:
            detail = ""
        try:
            detail = json.loads(detail).get("information") or detail
        except Exception:
            pass
        return ["HTTP %s: %s" % (e.code, " ".join(detail.split())[:260])]
    except urllib.error.URLError as e:
        return ["unreachable: %s" % e]
    except (TimeoutError, OSError) as e:
        # A socket timeout used to propagate and KILL the whole run mid-file,
        # which loses every result gathered so far and reports nothing. A slow
        # statement is a FAILED statement, not a crash.
        return ["timed out after %ss: %s" % (timeout, e)]
    return [str(b.get("result"))[:200] for b in res
            if isinstance(b, dict) and b.get("status") != "OK"]


STATUS_SET = re.compile(r"(,\s*)?status\s*=\s*'([a-zA-Z_]+)'")
STATUS_CONTENT = re.compile('(,\\s*)?\\s*["\']?status["\']?\\s*:\\s*"([a-zA-Z_]+)"')
UPSERT_ID = re.compile(r"^\s*(?:UPSERT|UPDATE|CREATE)\s+(finding:[A-Za-z0-9_]+)")


def split_status(stmt):
    """-> (statement_without_status, deferred_status_statement or None)

    The promotion ASSERT (77_epistemics.surql) tests a finding's cites at the
    moment `status` is written. The seeds are UPSERT-then-RELATE, so the status
    lands BEFORE the citation that justifies it and a correct claim is rejected
    for ordering alone.

    So statuses are deferred: pass 1 writes every row and every edge with the
    status assignment stripped, pass 2 writes the statuses once the cites
    exist. This is why `apply_seed.py` is the only supported way to load the
    seeds under the ASSERT.
    """
    m = UPSERT_ID.match(stmt)
    if not m:
        return stmt, None
    found = STATUS_SET.search(stmt)
    if not found:
        # CONTENT { ... status: "confirmed" ... } -- the other spelling. Missing
        # it left two findings writing their status in the SAME statement that
        # creates the row, i.e. before any RELATE could cite them, and the gate
        # rejected them for ordering exactly as predicted.
        cfound = STATUS_CONTENT.search(stmt)
        if not cfound:
            return stmt, None
        stripped = STATUS_CONTENT.sub("", stmt, count=1)
        stripped = re.sub(r"\{\s*,\s*", "{ ", stripped)
        stripped = re.sub(r",\s*,", ",", stripped)
        stripped = re.sub(r",\s*\}", " }", stripped)
        return stripped, "UPDATE %s SET status = '%s';" % (m.group(1), cfound.group(2))

    stripped = STATUS_SET.sub("", stmt, count=1)

    # Repair the separators. The pattern eats an optional LEADING comma, so a
    # `status=` that happened to come first leaves `SET , confidence=...` and
    # the statement no longer parses. Normalise all three shapes:
    stripped = re.sub(r"\bSET\s*,\s*", "SET ", stripped)   # status was first
    stripped = re.sub(r",\s*,", ",", stripped)             # status was middle
    stripped = re.sub(r",\s*;", ";", stripped)             # status was last

    # an UPSERT whose ONLY assignment was status would become a no-op; keep the
    # row so pass 2 has something to update
    stripped = re.sub(r"\bSET\s*;", "SET touched_at = time::now();", stripped)
    return stripped, "UPDATE %s SET status = '%s';" % (m.group(1), found.group(2))


def main(argv):
    dry = "--dry-run" in argv
    argv = [a for a in argv if not a.startswith("--")]
    files = argv or sorted(glob.glob(os.path.join("tools", "re_kb", "*.surql")),
                           key=lambda p: [int(t) if t.isdigit() else t
                                          for t in re.split(r"(\d+)", os.path.basename(p))])
    total = bad = 0
    deferred_status = []          # pass 2 -- see split_status()
    for path in files:
        text = open(path, encoding="utf-8", errors="replace").read()
        if is_script_scoped(text):
            errs = [] if dry else [(os.path.basename(path), e)
                                   for e in post(text, timeout=900)]
            print("%-52s  whole-script (LET-scoped)%s"
                  % (os.path.basename(path), "  <-- FAILED" if errs else ""))
            for head, e in errs[:3]:
                print("        -> %s" % e)
            total += 1
            bad += len(errs)
            continue
        stmts = split_statements(text)
        errs = []
        for s in stmts:
            if dry:
                continue
            # PASS 1: row + edges, status stripped. See split_status().
            body, deferred = split_status(s)
            if deferred:
                deferred_status.append(deferred)
            for e in post(body):
                errs.append((body.split("\n")[0][:88], e))
        total += len(stmts)
        bad += len(errs)
        # --dry-run never calls post(), so `errs` is necessarily empty. Saying
        # "0 failed" there would be the SAME fail-open this file exists to
        # prevent: a clean-looking report that never asked the server anything.
        # (It read exactly that way until 2026-09-01 -- "1804 statements, 0
        # failed" with the server untouched.)
        flag = ("  (not executed)" if dry else
                ("  <-- %d FAILED" % len(errs) if errs else ""))
        print("%-52s %4d stmts%s" % (os.path.basename(path), len(stmts), flag))
        for head, e in errs[:6]:
            print("      %s" % head)
            print("        -> %s" % e)
        if len(errs) > 6:
            print("      ... +%d more" % (len(errs) - 6))
    # PASS 2 -- the statuses, now that every row and every cites edge exists.
    if deferred_status and not dry:
        print("-" * 70)
        print("pass 2: %d deferred status writes" % len(deferred_status))
        for st in deferred_status:
            for e in post(st):
                bad += 1
                print("      %s" % st[:88])
                print("        -> %s" % e)
        total += len(deferred_status)

    print("-" * 70)
    if dry:
        print("%d statements parsed. NOT EXECUTED -- this says nothing about "
              "whether they apply." % total)
        return 0
    print("%d statements, %d failed" % (total, bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
