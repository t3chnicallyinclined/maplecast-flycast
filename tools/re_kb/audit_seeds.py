# -*- coding: utf-8 -*-
"""audit_seeds.py -- run the Incident-KB health metric against re_kb.

Reads the committed *.surql seeds directly, so it needs no running SurrealDB.
That is not a workaround: the seeds ARE the source of truth (re_kb/README.md),
and re_kb_data/ is a rebuildable RocksDB cache.

The metric, from resume/incident-kb-spec.html section 10:

    "Suspected-to-confirmed ratio -- health of the store.
     All-confirmed means the promotion rules aren't being enforced."

Promotion rule (77_epistemics.surql): a finding may be `confirmed` only if it
cites at least one source of reproduction- or code-grade. Attestation-grade
evidence -- a doc, a third-party page, a note -- is enough for `inferred`
and never enough on its own.

    PYTHONIOENCODING=utf-8 python tools/re_kb/audit_seeds.py
"""
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

STRENGTH_OF = {}
for _k in ("oracle oracle_capture capture live_capture capture_analysis "
           "transpile_validation").split():
    STRENGTH_OF[_k] = "reproduction"
for _k in "code disasm disassembly memory marvelous2 js_function implementation".split():
    STRENGTH_OF[_k] = "code"
for _k in "measurement tool".split():
    STRENGTH_OF[_k] = "metric"
for _k in "anotak doc reference recatalog format format_decode expert_trace".split():
    STRENGTH_OF[_k] = "attestation"

CONFIRMING = {"reproduction", "code"}

# a statement runs until the next line-anchored statement keyword
STMT = r"(?=\n(?:UPSERT|RELATE|UPDATE|DEFINE|REMOVE|--)|\Z)"

NEGATIVE = re.compile(
    r"ruled out|dead.?end|disprov|does not work|doesn't work|did not work|"
    r"NOT the|no effect|failed|abandoned|wrong theory|superseded",
    re.I)


def strength_of_cite(table, ident, sources):
    """A cite may point at a source, a routine, or another finding.

    A `routine` id IS its PC (routine:loc_8c0344d4), i.e. a marvelous2
    disassembly location -- code-grade evidence by the ladder. A finding
    citing another finding is derived, not primary.
    """
    if table == "routine":
        return "code"
    if table == "finding":
        return "derived"
    return STRENGTH_OF.get(sources.get(ident), "unranked")


def load():
    findings, sources, cites = {}, {}, {}
    for path in sorted(glob.glob(os.path.join(HERE, "*.surql"))):
        text = open(path, encoding="utf-8", errors="replace").read()

        for m in re.finditer(r"UPSERT\s+source:([\w\d_]+)\s+SET(.*?)" + STMT,
                             text, re.S):
            sid, body = m.group(1), m.group(2)
            k = re.search(r"kind='([\w\d_]+)'", body)
            sources[sid] = k.group(1) if k else None

        for m in re.finditer(r"UPSERT\s+finding:([\w\d_]+)\s+SET(.*?)" + STMT,
                             text, re.S):
            fid, body = m.group(1), m.group(2)
            st = re.search(r"status='([\w\d_]+)'", body)
            prev = findings.get(fid, {})
            findings[fid] = {
                "status": st.group(1) if st else prev.get("status"),
                "body": body,
                "file": os.path.basename(path),
            }

        for m in re.finditer(
                r"RELATE\s+finding:([\w\d_]+)->cites->([a-z_]+):([\w\d_]+)", text):
            cites.setdefault(m.group(1), set()).add((m.group(2), m.group(3)))

    return findings, sources, cites


def main():
    findings, sources, cites = load()

    print("=" * 74)
    print("re_kb HEALTH AUDIT  --  Incident-KB metric, section 10")
    print("=" * 74)
    print("findings: %d   sources: %d   cite edges: %d"
          % (len(findings), len(sources),
             sum(len(v) for v in cites.values())))
    print()

    dist = {}
    for f in findings.values():
        dist[f["status"] or "(none)"] = dist.get(f["status"] or "(none)", 0) + 1
    print("-- status distribution " + "-" * 51)
    for k, v in sorted(dist.items(), key=lambda x: -x[1]):
        print("  %5d  %s" % (v, k))

    conf = sum(v for k, v in dist.items() if k == "confirmed")
    soft = sum(v for k, v in dist.items() if k in ("inferred", "open"))
    print()
    print("  confirmed : inferred+open  =  %d : %d   (%s)"
          % (conf, soft,
             "HEALTHY" if soft and conf / max(soft, 1) < 3 else
             "UNENFORCED -- see IKB section 10"))

    print()
    print("-- evidence strength available " + "-" * 43)
    sdist = {}
    for kind in sources.values():
        s = STRENGTH_OF.get(kind, "unranked")
        sdist[s] = sdist.get(s, 0) + 1
    for s in ("reproduction", "code", "metric", "attestation", "unranked"):
        if s in sdist:
            print("  %5d  %s" % (sdist[s], s))

    print()
    print("-- confirmed findings vs the promotion rule " + "-" * 30)
    no_cite, weak_only, ok = [], [], 0
    for fid, f in findings.items():
        if f["status"] != "confirmed":
            continue
        srcs = cites.get(fid, set())
        if not srcs:
            no_cite.append(fid)
            continue
        strengths = {strength_of_cite(t, i, sources) for (t, i) in srcs}
        if strengths & CONFIRMING:
            ok += 1
        else:
            weak_only.append((fid, sorted(strengths)))

    print("  %5d  confirmed WITH reproduction/code evidence     (rule satisfied)" % ok)
    print("  %5d  confirmed citing only weaker evidence         (RULE VIOLATION)"
          % len(weak_only))
    print("  %5d  confirmed with NO cites edge at all           (RULE VIOLATION)"
          % len(no_cite))
    if conf:
        print("         -> %.0f%% of confirmed claims cannot show qualifying evidence"
              % (100.0 * (len(weak_only) + len(no_cite)) / conf))

    if weak_only:
        print()
        print("  weakest-backed confirmed claims (first 8):")
        for fid, st in weak_only[:8]:
            print("    %-46s cites only: %s" % (fid[:46], ",".join(st)))
    if no_cite:
        print()
        print("  confirmed with no provenance at all (first 8):")
        for fid in no_cite[:8]:
            print("    %s" % fid)

    print()
    print("-- negative results buried in prose " + "-" * 38)
    buried = [(fid, f["file"]) for fid, f in findings.items()
              if NEGATIVE.search(f["body"]) and f["status"] != "ruled_out"]
    print("  %5d  findings whose text records a dead end / disproof," % len(buried))
    print("         but whose STATUS does not say so -- so no query finds them.")
    for fid, fl in buried[:8]:
        print("    %-46s %s" % (fid[:46], fl))

    print()
    print("=" * 74)
    print("Fix order: 77_epistemics.surql (schema) -> kb.py (gate) -> backfill.")
    print("The rule was never wrong. It was written as a norm, not a gate.")
    print("=" * 74)


if __name__ == "__main__":
    sys.exit(main())
