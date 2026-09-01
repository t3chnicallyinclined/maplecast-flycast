# -*- coding: utf-8 -*-
"""marv_calls.py -- build the routine call graph from the marvelous2 disassembly.

WHY
---
Measured 2026-09-01: the graph holds **11** `calls` edges against **3,670**
static `bsr loc_8c......` call sites in ../_marv_re/build/*.asm. The call graph
effectively does not exist, so every "who calls this?" / "what does this reach?"
question is answered by grep -- including the two questions that have already
cost this project real time:

  * loc_8c120220 (the XMTRX back-bank loader) is reached ONLY by a static `bra`
    with no #data reference, so boundary scanners missed it. That one routine
    was the entire 52-byte executor residual.
  * loc_8c044f12 (the object-pool allocator) is reached by `jsr @rN` from
    loc_8c0e3098. Missing it dropped the whole pool subsystem.

Both are INDIRECT-REACH misses. So the edge records HOW it was reached
(`via`), which turns that bug class into a query instead of a memory:

    SELECT in.pc, out.pc, via FROM calls
     WHERE via IN ['jsr_indirect','jmp_indirect','bra_tail']
       AND out.transpiled = false;

WHAT IT WRITES
--------------
`routine` rows and `calls` edges ONLY. No `finding`, ever -- bulk ingest may
not manufacture claims (that is how 576 markdown bullets once became 90% of the
finding table). A routine row is an addressable artifact, not an assertion:
its id IS its PC, and every field here is mechanically derived.

  routine.bank / .bank_lines   where the label is DEFINED
  routine.in_tick_trace        PC appears in _handoff/trace_distinct_pcs.txt
  routine.transpiled           gen_tick_all.c defines void fn_<pc>(...)
  calls.via / .resolved_by     how the call site reaches the target

Existing rows are ENRICHED, never overwritten: a hand-written `role`/`summary`
/`note` is left alone.

    PYTHONIOENCODING=utf-8 python tools/re_kb/ingest/marv_calls.py
    tools/re_kb/rekb.sh @tools/re_kb/ingest/generated/marv_calls.surql   # or apply_seed.py
    tools/re_kb/rekb.sh @tools/re_kb/07_dedup_edges.surql                # RELATE is not idempotent
"""
import glob
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
MARV = os.path.join(REPO, "..", "_marv_re", "build")
POC = os.path.join(REPO, "tools", "render-replica-poc")
OUT = os.path.join(HERE, "generated", "marv_calls.surql")

LABEL_DEF = re.compile(r"(?im)^\s*(loc_8c[0-9a-f]{6})\s*:")
# a call/branch site naming a loc_ target
SITE = re.compile(r"(?i)\b(bsr|bra|jsr|jmp|braf|bsrf)\b[^\n;]*?(loc_8c[0-9a-f]{6})")
PCLINE = re.compile(r"(?i)\b(loc_8c[0-9a-f]{6})\b")

VIA = {"bsr": "bsr", "bra": "bra_tail", "jsr": "jsr_indirect",
       "jmp": "jmp_indirect", "braf": "braf", "bsrf": "bsrf"}


def scan_disasm():
    """-> (defined{pc:(bank,line)}, sites[(bank, marks, line_idx, target, via)])

    Call sites are recorded with their POSITION, not yet attributed to a
    caller. Attribution needs the set of real functions, which is not known
    until the worklists are read -- and attributing to the nearest label
    instead drops most edges, because the nearest label is usually a local
    branch target inside the function rather than the function itself.
    """
    defined = {}
    per_bank = {}
    for path in sorted(glob.glob(os.path.join(MARV, "*.asm"))):
        bank = os.path.splitext(os.path.basename(path))[0]
        lines = io.open(path, encoding="utf-8", errors="replace").read().splitlines()
        marks = []
        for i, l in enumerate(lines):
            m = LABEL_DEF.match(l)
            if m:
                pc = m.group(1).lower()
                defined.setdefault(pc, (bank, i + 1))
                marks.append((i, pc))
        sites = []
        for i, l in enumerate(lines):
            m = SITE.search(l)
            if m:
                sites.append((i, m.group(2).lower(), VIA.get(m.group(1).lower(),
                                                             m.group(1).lower())))
        per_bank[bank] = (marks, sites)
    return defined, per_bank


def attribute(per_bank, functions):
    """Map each call site to the nearest ENCLOSING FUNCTION label."""
    import bisect
    edges = set()
    for bank, (marks, sites) in per_bank.items():
        fmarks = [(i, pc) for (i, pc) in marks if pc in functions]
        if not fmarks:
            continue
        idxs = [i for i, _ in fmarks]
        for line_i, tgt, via in sites:
            j = bisect.bisect_right(idxs, line_i) - 1
            if j < 0:
                continue
            caller = fmarks[j][1]
            if caller != tgt:
                edges.add((caller, tgt, via))
    return edges


def read_worklist(name):
    p = os.path.join(POC, "_handoff", name)
    if not os.path.exists(p):
        return {}
    out = {}
    for line in io.open(p, encoding="utf-8", errors="replace"):
        if line.startswith("#") or not line.strip():
            continue
        m = PCLINE.match(line.strip())
        if not m:
            continue
        pc = m.group(1).lower()
        bl = re.search(r"body_lines=(\S+)", line)
        out[pc] = bl.group(1) if bl else None
    return out


def read_trace():
    p = os.path.join(POC, "_handoff", "trace_distinct_pcs.txt")
    if not os.path.exists(p):
        return set()
    pcs = set()
    for line in io.open(p, encoding="utf-8", errors="replace"):
        m = re.search(r"(?i)0x?(8c[0-9a-f]{6})", line)
        if m:
            pcs.add("loc_" + m.group(1).lower())
    return pcs


def read_indirect():
    """Resolved register-indirect calls from _marv_re/calltree_indirect.txt.

    Format: caller \\t op \\t reg \\t target. These are the edges a static scan
    CANNOT find -- `jsr @r14` names no label -- and they are the class that has
    twice cost this project days (the object-pool allocator was reached exactly
    this way). The file is produced by _marv_re/resolve_indirect.py and was
    entirely un-ingested until 2026-09-01.
    """
    p = os.path.join(MARV, "..", "calltree_indirect.txt")
    if not os.path.exists(p):
        return set()
    out = set()
    for line in io.open(p, encoding="utf-8", errors="replace"):
        parts = line.strip().split("\t")
        if len(parts) < 4:
            continue
        caller, op, _reg, tgt = parts[0].lower(), parts[1].lower(), parts[2], parts[3].lower()
        if not caller.startswith("loc_8c") or not tgt.startswith("loc_8c"):
            continue
        if caller == tgt:
            continue
        out.add((caller, tgt, "jsr_indirect" if op == "jsr" else "jmp_indirect"))
    return out


def read_transpiled():
    p = os.path.join(POC, "gen_tick_all.c")
    if not os.path.exists(p):
        return set()
    out = set()
    with io.open(p, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = re.match(r"\s*void\s+fn_(8c[0-9a-f]{6})\s*\(", line)
            if m:
                out.add("loc_" + m.group(1).lower())
    return out


def main():
    if not os.path.isdir(MARV):
        print("disassembly not found at %s" % MARV)
        return 2
    defined, per_bank = scan_disasm()
    tick = read_worklist("funclist.txt")
    full = read_worklist("funclist_full.txt")
    spl = read_worklist("spl_funclist.txt")
    traced = read_trace()
    transpiled = read_transpiled()
    indirect = read_indirect()

    # WHAT COUNTS AS A ROUTINE.
    #
    # Not "every label a branch points at". The first version of this script
    # emitted 23,594 routines and 17,190 edges, because it treated a `bra` to a
    # local label -- a loop, an if-else join, a switch arm -- as a call. That is
    # not a call graph, it is a basic-block graph with the wrong table name, and
    # it would have buried the ~2,200 real functions in 20k noise rows.
    #
    # A routine is a label with independent evidence of being a FUNCTION:
    #   * it is in an executed-tick worklist, or the PC trace, or
    #   * the transpiler emitted a function body for it, or
    #   * something CALLS it (bsr/bsrf/jsr) rather than branching to it.
    #
    # `bra`/`jmp` targets are then kept only when the target is independently a
    # function -- i.e. a real tail call -- and dropped otherwise.
    all_sites = [(t, v) for (_marks, ss) in per_bank.values() for (_i, t, v) in ss]
    called = {t for t, v in all_sites if v in ("bsr", "bsrf", "jsr_indirect")}
    called |= {t for _c, t, _v in indirect}
    functions = (set(tick) | set(full) | set(spl) | traced | transpiled | called
                 | {c for c, _t, _v in indirect})
    want = {pc for pc in functions if pc in defined}
    edges = attribute(per_bank, want)
    # resolved indirects carry their own attribution already
    edges |= {(c, t, v) for (c, t, v) in indirect if c in want and t in want}

    lines = [
        "-- " + "=" * 74,
        "-- marv_calls.surql -- GENERATED by ingest/marv_calls.py. Do not hand-edit.",
        "--",
        "-- routine rows + the static call graph, derived from",
        "--   ../_marv_re/build/*.asm            (label definitions + call sites)",
        "--   tools/render-replica-poc/_handoff/ (executed-tick worklists, PC trace)",
        "--   tools/render-replica-poc/gen_tick_all.c (which PCs are transpiled)",
        "--",
        "-- The graph held 11 `calls` edges against %d attributed call sites." % len(edges),
        "-- NO `finding` rows are written here, by design: bulk ingest records",
        "-- artifacts, never claims.",
        "--",
        "-- Re-apply is safe (UPSERT), but RELATE is not idempotent -- run",
        "-- 07_dedup_edges.surql afterwards.",
        "-- " + "=" * 74,
        "USE NS re DB kb;",
        "",
        "-- ---- routines (%d) " % len(want) + "-" * 40,
    ]
    for pc in sorted(want):
        bank, line = defined[pc]
        bl = tick.get(pc) or full.get(pc) or spl.get(pc)
        sets = ["label='%s'" % pc,
                "pc='0x8C%s'" % pc[6:].upper(),
                "bank='%s'" % bank,
                "def_line=%d" % line,
                "in_tick_trace=%s" % ("true" if (pc in traced or pc in tick) else "false"),
                "transpiled=%s" % ("true" if pc in transpiled else "false")]
        if bl:
            sets.append("bank_lines='%s:%s'" % (bank + ".asm", bl))
        lines.append("UPSERT routine:%s SET %s;" % (pc, ", ".join(sets)))

    lines.append("")
    ok = [(c, t, v) for (c, t, v) in sorted(edges) if c in want and t in want]
    lines.append("-- ---- calls (%d; %d dropped as unanchored) "
                 % (len(ok), len(edges) - len(ok)) + "-" * 20)
    for c, t, v in ok:
        by = "calltree" if v in ("jsr_indirect", "jmp_indirect") else "static"
        lines.append("RELATE routine:%s->calls->routine:%s "
                     "SET via='%s', resolved_by='%s';" % (c, t, v, by))

    io.open(OUT, "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")
    print("routines : %d  (in-tick %d, transpiled %d)"
          % (len(want), sum(1 for p in want if p in traced or p in tick),
             sum(1 for p in want if p in transpiled)))
    print("calls    : %d  (%d dropped as unanchored)" % (len(ok), len(edges) - len(ok)))
    print("-> %s" % OUT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
