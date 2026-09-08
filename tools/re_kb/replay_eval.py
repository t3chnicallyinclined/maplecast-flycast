# -*- coding: utf-8 -*-
"""replay_eval.py -- measure the overseer against sessions that already happened.

THE POINT
---------
"Does an overseer help?" is exactly the kind of claim this project has
declared falsely won before. So it is not evaluated by running it live and
forming an impression. It is evaluated offline, deterministically, against
ground truth that already exists:

  * agent transcripts carry an ISO `timestamp` on every record
  * KB findings carry a `date`

So for any past turn we can ask a falsifiable question:

    did the worker at time T touch something the graph ALREADY knew at T?

Every yes is a re-derivation that actually happened and that the overseer
would have interrupted. The KB's own dates are the ground truth; no model
and no human judgement is needed to produce the number.

Findings dated on or after the turn are excluded, so the overseer is never
credited with knowing something that had not been learned yet.

    PYTHONIOENCODING=utf-8 python tools/re_kb/replay_eval.py
    PYTHONIOENCODING=utf-8 python tools/re_kb/replay_eval.py --max-turns 400
"""
import glob
import json
import os
import sys
import time
import collections

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import overseer  # noqa: E402

SESSIONS = os.path.expanduser(
    "~/.claude/projects/c--Users-trist-projects-maplecast-flycast")


def turn_text(rec):
    """Everything the worker emitted on this turn: prose AND tool inputs.

    Tool inputs matter more than the prose. 'let me read 0x3F24' is the
    moment to interrupt; the paragraph explaining it afterwards is too late.
    """
    m = rec.get("message")
    if not isinstance(m, dict):
        return ""
    c = m.get("content")
    if isinstance(c, str):
        return c
    parts = []
    if isinstance(c, list):
        for b in c:
            if not isinstance(b, dict):
                continue
            if b.get("type") == "text":
                parts.append(b.get("text") or "")
            elif b.get("type") == "tool_use":
                parts.append(json.dumps(b.get("input") or {})[:4000])
    return "\n".join(parts)


def main():
    max_turns = None
    if "--max-turns" in sys.argv:
        max_turns = int(sys.argv[sys.argv.index("--max-turns") + 1])

    files = sorted(glob.glob(os.path.join(SESSIONS, "*.jsonl")))
    if not files:
        print("no transcripts at %s" % SESSIONS)
        return 1

    # ---- pass 1: extract identifiers per turn (no DB yet) -----------------
    turns = []           # (session, ts, {token: kind})
    scanned = 0
    for f in files:
        with open(f, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                try:
                    rec = json.loads(line)
                except Exception:
                    continue
                if rec.get("type") != "assistant":
                    continue
                scanned += 1
                if max_turns and scanned > max_turns:
                    break
                toks = overseer.extract(turn_text(rec))
                if toks:
                    turns.append((os.path.basename(f)[:8],
                                  (rec.get("timestamp") or "")[:10], toks))
        if max_turns and scanned > max_turns:
            break

    unique = {}
    for _, _, toks in turns:
        unique.update(toks)

    print("=" * 76)
    print("OVERSEER REPLAY EVALUATION  (rung 0: exact identifier lookup)")
    print("=" * 76)
    print("transcripts       : %d" % len(files))
    print("assistant turns   : %d scanned, %d carried >=1 identifier"
          % (scanned, len(turns)))
    print("unique identifiers: %d" % len(unique))
    print()

    # ---- one query per unique identifier, cached --------------------------
    print("querying the graph once per identifier ...")
    t0 = time.time()
    cache, errors = {}, 0
    for i, tok in enumerate(unique):
        try:
            cache[tok] = overseer.lookup(tok)
        except Exception:
            cache[tok] = []
            errors += 1
        if i and i % 200 == 0:
            print("   %d/%d" % (i, len(unique)))
    elapsed = time.time() - t0
    print("   done in %.1fs  (%.1f ms per identifier)%s"
          % (elapsed, 1000.0 * elapsed / max(1, len(unique)),
             "  [%d query errors]" % errors if errors else ""))
    print()

    # ---- pass 2: replay with the date gate --------------------------------
    fired = 0
    actionable_turns = 0
    by_status = collections.Counter()
    examples = []

    for sess, ts, toks in turns:
        hits = []
        for tok in toks:
            for r in cache.get(tok, []):
                d = r.get("date")
                # only credit knowledge the graph could already have had
                if not d or not ts or d >= ts:
                    continue
                hits.append((tok, r))
        if not hits:
            continue
        fired += 1
        statuses = {(r.get("status") or "?").lower() for _, r in hits}
        for s in statuses:
            by_status[s] += 1
        if statuses & {"ruled_out", "superseded", "masks_only"}:
            actionable_turns += 1
            if len(examples) < 8:
                examples.append((sess, ts, hits[0]))
        elif "confirmed" in statuses and len(examples) < 8:
            examples.append((sess, ts, hits[0]))

    print("-" * 76)
    print("RESULTS")
    print("-" * 76)
    pct = 100.0 * fired / max(1, len(turns))
    print("turns where the overseer would have fired : %d / %d  (%.1f%%)"
          % (fired, len(turns), pct))
    print("  ... citing a ruled_out/superseded/masks_only row: %d"
          % actionable_turns)
    print()
    print("fires by status of the cited row:")
    for s, n in by_status.most_common():
        print("   %5d  %s" % (n, s))

    if examples:
        print()
        print("-" * 76)
        print("SAMPLE FIRES  (the graph knew this BEFORE the turn happened)")
        print("-" * 76)
        for sess, ts, (tok, r) in examples:
            print("%s  %s  %s" % (sess, ts, tok))
            print("   -> %-11s %s  %s"
                  % ((r.get("status") or "?").upper(), r.get("date"),
                     (r.get("claim") or "").replace("\n", " ")[:150]))
            print()

    print("=" * 76)
    print("READ THIS BEFORE BELIEVING THE NUMBER")
    print("=" * 76)
    print("""A fire is NOT automatically a prevented re-walk. It means the worker
mentioned an identifier the graph already had a row for. Sometimes that is a
re-derivation about to happen; sometimes the worker is correctly building ON
that row. Only a labelled sample separates the two, and that is the next step.

What the number DOES establish without labelling:
  * whether rung 0 fires often enough to matter at all
  * whether it fires so often it would be ignored (noise is the real risk)
  * the latency, which decides if this can run on every turn

The P0 gate, borrowed from the incident-KB rollout: if a hand-labelled sample
of ~30 fires contains almost no genuine re-walks, the problem is not worth
tooling and you should stop here.""")
    return 0


if __name__ == "__main__":
    sys.exit(main())
