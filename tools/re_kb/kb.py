# -*- coding: utf-8 -*-
"""kb.py -- the enforced write surface for the MapleCast RE knowledge graph.

WHY THIS EXISTS
---------------
rekb.sh hands a caller arbitrary SQL and root credentials. Given that
interface, `UPSERT finding:x SET status='confirmed'` with no citation is a
legal one-liner and nothing in the path can reject it. That is not a
discipline failure -- it is the only outcome the interface allows.

Measured 2026-09-01: of the hand-curated RE findings in the live graph,
51 are `confirmed` and 0 are `inferred`. (The graph's other 576 findings are
bulk doc ingestion from ingest/docs_parse.py; those DO carry 93 `inferred`,
which flatters every aggregate and is why the split matters.) The committed
seeds tell the same story independently: 162 confirmed, 1 inferred.

So the rules stop being prose in a README and become function signatures:

  * confirm() requires a `source`, and rejects one that is not
    reproduction- or code-grade. No source, no call.
  * propose() can only ever write status='inferred'.
  * rule_out() exists at all, so dead ends get a status instead of being
    buried in a note field.
  * record_attempt() requires an outcome, and `masks_only` is one of them.

The promotion rule and the evidence ladder are documented in
77_epistemics.surql. This file is where they are enforced.

USAGE
-----
    import kb
    kb.health()                       # the audit -- needs no arguments
    kb.query("SELECT * FROM finding WHERE status='open'")
    kb.propose('facing_neg_r10', 'neg r10 mirrors the pen origin', about='field:facing')
    kb.confirm('facing_neg_r10', source='marv_bank03')
    kb.record_attempt('reflect_whole_rect', on='facing_neg_r10',
                      outcome='ineffective',
                      note='injected a spurious -w, ~50-70 game-px, sign-flipped with facing')

Server: surreal start --user root --pass root --bind 127.0.0.1:8001 \
          rocksdb:re_kb_data/re_kb
"""
import json
import os
import re
import urllib.error
import urllib.request

URL = os.environ.get("REKB_URL", "http://127.0.0.1:8001/sql")
AUTH = os.environ.get("REKB_AUTH", "root:root")

# --- the evidence ladder, strongest first ----------------------------------
STRENGTH = ["reproduction", "code", "metric", "recurrence", "attestation"]

#: strengths that are sufficient, on their own, to promote inferred -> confirmed
CONFIRMING = {"reproduction", "code"}

FINDING_STATUS = {"open", "inferred", "confirmed", "ruled_out", "superseded", "resolved"}

#: Stamped on every row this module writes.
#:
#: Without it the confirmed:inferred ratio is UNINTERPRETABLE, which was the
#: process review's first finding. 236 findings carry a human-typed `date` (a
#: claim date), exactly one carried `updated_at`, and NONE carried a creation
#: time or a write path -- so "the rule works" and "writes still bypass it"
#: made identical predictions about the table. The cohort query that separates
#: them needs a marker the gate itself applies:
#:
#:   SELECT status, via, count() FROM finding
#:    WHERE wrote_at > '<date the gate landed>' GROUP BY status, via;
#:
#: A post-gate cohort that is again ~100% confirmed, or ANY confirmed row with
#: via != 'kb.py', disproves "the rule is working".
PROVENANCE = "via='kb.py', wrote_at=time::now()"
OUTCOMES = {"effective", "masks_only", "ineffective", "unproven"}


class KBError(RuntimeError):
    """A rule was violated, or the graph is unreachable."""


def _sql(stmt):
    """Execute SurrealQL. Internal -- callers use the typed helpers below."""
    body = ("USE NS re DB kb; " + stmt).encode("utf-8")
    req = urllib.request.Request(URL, data=body, method="POST")
    req.add_header("Accept", "application/json")
    import base64
    req.add_header("Authorization", "Basic " +
                   base64.b64encode(AUTH.encode()).decode())
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            return json.loads(r.read().decode("utf-8"))
    except urllib.error.URLError as e:
        raise KBError(
            "cannot reach the graph at %s (%s).\n"
            "start it with:  surreal start --user root --pass root "
            "--bind 127.0.0.1:8001 rocksdb:re_kb_data/re_kb" % (URL, e)) from None


def _q(s):
    """Escape a value for a single-quoted SurrealQL literal."""
    return str(s).replace("\\", "\\\\").replace("'", "\\'")


def _rows(res):
    """Collect result rows, RAISING if any statement failed.

    This used to skip non-OK blocks silently, which made every failure look
    like an empty result. On 2026-09-01 that turned health() into a liar: its
    unbacked-claims query hit `string::slice()` on a NULL statement, the whole
    statement errored, the error was dropped here, and health() reported
    ZERO confirmed findings without qualifying evidence. The true count was 37.

    A metric that fails open is worse than no metric -- it produces a clean
    bill of health on demand. So: errors raise.
    """
    out, errs = [], []
    for block in res if isinstance(res, list) else []:
        if not isinstance(block, dict):
            continue
        if block.get("status") == "OK":
            r = block.get("result")
            if isinstance(r, list):
                out.extend(r)
        else:
            errs.append(str(block.get("result"))[:300])
    if errs:
        raise KBError("the graph rejected a statement:\n  " + "\n  ".join(errs))
    return out


# --------------------------------------------------------------------------
# read
# --------------------------------------------------------------------------

def query(sql):
    """Run a read-only query. Rejects anything that writes.

    Writes go through the typed helpers, so the rules cannot be sidestepped
    by passing raw SQL to a function called `query`.
    """
    # WORD boundaries, not substrings. A plain `"RELATE" in sql` also matches
    # the TABLE `relates_to`, so `SELECT ... FROM relates_to` -- a read -- was
    # rejected as a write. A safety check that blocks legitimate reads gets
    # worked around, and a worked-around check protects nothing.
    banned = ("UPSERT", "UPDATE", "DELETE", "CREATE", "RELATE", "REMOVE",
              "DEFINE", "INSERT")
    hit = re.search(r"\b(%s)\b" % "|".join(banned), sql, re.I)
    if hit:
        raise KBError("query() is read-only; use the typed helpers for writes "
                      "(found %r)" % hit.group(1).upper())
    return _rows(_sql(sql))


def get(finding):
    """Fetch one finding WITH its status, evidence strengths and age.

    IKB, section 08: "Status is never optional in a result." A caller must not
    be able to receive a bare statement that looks like settled fact, so this
    never returns the statement on its own.
    """
    fid = finding if ":" in str(finding) else "finding:" + str(finding)
    rows = _rows(_sql(
        "SELECT id, statement, status, confidence, date, "
        "->cites->source.kind AS evidence_kind, "
        "->cites->source.strength AS evidence_strength, "
        "->supersedes->finding.id AS supersedes, "
        "<-supersedes<-finding.id AS superseded_by "
        "FROM %s;" % fid))
    return rows[0] if rows else None


# --------------------------------------------------------------------------
# write -- the rules live in these signatures
# --------------------------------------------------------------------------

def propose(slug, statement, about=None, reasoning=None, date=None):
    """Record a new claim. ALWAYS lands as status='inferred'.

    There is deliberately no `status` parameter. A model may propose; it may
    not promote. Promotion goes through confirm(), which requires evidence.
    """
    fid = "finding:" + slug
    sets = ["statement='%s'" % _q(statement), "status='inferred'", PROVENANCE]
    if reasoning:
        sets.append("reasoning='%s'" % _q(reasoning))
    if date:
        sets.append("date='%s'" % _q(date))
    _sql("UPSERT %s SET %s;" % (fid, ", ".join(sets)))
    if about:
        _sql("RELATE %s->about->%s;" % (fid, about))
    return fid


def confirm(finding, source, note=None):
    """Promote a finding to confirmed. REQUIRES qualifying evidence.

    `source` is mandatory -- there is no way to call this without one -- and
    it must be reproduction- or code-grade. An attestation-grade source
    (a doc, an anotak page, a note) is enough for `inferred` and is rejected
    here, per the promotion rule in 77_epistemics.surql.
    """
    fid = finding if ":" in str(finding) else "finding:" + str(finding)
    sid = source if ":" in str(source) else "source:" + str(source)

    rows = _rows(_sql("SELECT id, kind, strength FROM %s;" % sid))
    if not rows:
        raise KBError("no such source %r. Record the source first -- "
                      "provenance is not optional." % sid)
    strength = rows[0].get("strength")
    if strength not in CONFIRMING:
        raise KBError(
            "%s is %s-grade (kind=%s). confirmed requires reproduction or code.\n"
            "This claim can be recorded as inferred; say what would confirm it."
            % (sid, strength or "unranked", rows[0].get("kind")))

    sets = ["status='confirmed'", "confidence='high'", PROVENANCE]
    if note:
        sets.append("note='%s'" % _q(note))
    _sql("UPSERT %s SET %s;" % (fid, ", ".join(sets)))
    _sql("RELATE %s->cites->%s;" % (fid, sid))
    return fid


def rule_out(slug, statement, tried, evidence, date=None):
    """Record something investigated and ELIMINATED.

    IKB: "Negative results are the cheapest knowledge to record and the most
    expensive to rediscover." These currently exist in the graph only as
    prose inside note fields, which means they are greppable but not
    queryable -- so the next session re-walks them.
    """
    fid = "finding:" + slug
    sets = ["statement='%s'" % _q(statement),
            "status='ruled_out'",
            "tried='%s'" % _q(tried),
            "evidence='%s'" % _q(evidence),
            PROVENANCE]
    if date:
        sets.append("date='%s'" % _q(date))
    _sql("UPSERT %s SET %s;" % (fid, ", ".join(sets)))
    return fid


def record_attempt(approach, on, outcome, note=None, date=None, how=None):
    """Record that an approach was tried against a problem, WITH its outcome.

    The outcome lives on the edge, not on the approach, because the same
    approach can work against one problem and fail against another -- and
    that difference is the most valuable data in the graph.

    outcome must be one of:
      effective    worked, mechanism understood (or 2+ reproductions)
      masks_only   right output, mechanism NOT understood. A tuned constant,
                   a fudged coordinate, a clamp hiding the real bug. Ship it
                   if you must; it is not a fix.
      ineffective  tried, did not help
      unproven     tried once, seemed to help. Correlation, not causation.
    """
    if outcome not in OUTCOMES:
        raise KBError("outcome must be one of %s (got %r). If it produced the "
                      "right pixels but you cannot explain why, that is "
                      "'masks_only', not 'effective'."
                      % (sorted(OUTCOMES), outcome))
    aid = approach if ":" in str(approach) else "approach:" + str(approach)
    tid = on if ":" in str(on) else "finding:" + str(on)

    sets = ["name='%s'" % _q(str(approach).split(":")[-1]), PROVENANCE]
    if how:
        sets.append("how='%s'" % _q(how))
    _sql("UPSERT %s SET %s;" % (aid, ", ".join(sets)))

    edge = ["outcome='%s'" % _q(outcome)]
    if note:
        edge.append("note='%s'" % _q(note))
    if date:
        edge.append("date='%s'" % _q(date))
    _sql("RELATE %s->tried_on->%s SET %s;" % (aid, tid, ", ".join(edge)))
    return aid


def supersede(old, new, why):
    """Replace a claim. The old row STAYS, with its reason.

    A superseded claim with a visible date is still a lead. Deleting it
    throws away the record of what was believed and why it was wrong.
    """
    o = old if ":" in str(old) else "finding:" + str(old)
    n = new if ":" in str(new) else "finding:" + str(new)
    _sql("UPSERT %s SET status='superseded', superseded_why='%s', %s;"
         % (o, _q(why), PROVENANCE))
    _sql("RELATE %s->supersedes->%s;" % (n, o))
    return n


# --------------------------------------------------------------------------
# the meta-queries -- questions the graph asks about itself
# --------------------------------------------------------------------------

def health():
    """The IKB health metric, section 10.

    "Suspected-to-confirmed ratio -- health of the store. All-confirmed means
    the promotion rules aren't being enforced."
    """
    dist = query("SELECT status, count() AS n FROM finding "
                 "GROUP BY status ORDER BY n DESC;")
    # A cite may land on a source OR on a routine. A routine id IS its PC in
    # the marvelous2 disassembly, so it is code-grade evidence and satisfies
    # the rule. Counting only `source` under-reports compliance badly.
    # NOTE: no string::slice here. `statement` is NULL on some rows (the claim
    # lives in `note`), and string::slice(NONE) errors out the whole statement.
    # Truncation happens in Python, where a missing field is just a missing
    # field. See _rows() for what that error used to cost.
    unbacked = query(
        "SELECT id, status, date, statement, note FROM finding "
        "WHERE status='confirmed' "
        "AND count(->cites->source[WHERE strength IN ['reproduction','code']]) = 0 "
        "AND count(->cites->routine) = 0;")
    for r in unbacked:
        r["claim"] = _claim(r)

    # The STRICTER count. The query above lets any ->cites->routine edge stand
    # in for evidence, but that edge carries only (in, out) -- no line, no
    # instruction, no excerpt. `finding:x -> cites -> routine:loc_8c0344d4`
    # asserts "this claim mentions that routine", not "this claim is visible at
    # these instructions". A locator is not a citation. Report both numbers so
    # the gap between them is visible instead of flattering.
    strict = query(
        "SELECT count() AS n FROM finding WHERE status='confirmed' "
        "AND count(->cites->source[WHERE strength IN ['reproduction','code']]) = 0 "
        "GROUP ALL;")

    # A confirmed finding with no `about` edge cannot be reached by the
    # PreToolUse hook's address/PC lookup at all -- it is in the graph and
    # invisible to the mechanism built to surface it.
    unattached = query(
        "SELECT count() AS n FROM finding WHERE status='confirmed' "
        "AND count(->about) = 0 GROUP ALL;")

    # A finding with NO status at all. The promotion ASSERT declares `status`
    # as option<string> so the two-phase apply can write a row before its
    # status -- which also means a hand-written UPSERT that simply forgets one
    # is ACCEPTED. That happened the first time 89 was applied: five ruled_out
    # findings landed statusless and read as neither confirmed nor ruled out.
    # A row with no status is invisible to every status query, so it has to be
    # its own number.
    statusless = query("SELECT count() AS n FROM finding "
                       "WHERE status = NONE GROUP ALL;")

    # DRIFT. The promotion ASSERT (88) fires when `status` is WRITTEN. Demote a
    # source's strength afterwards and the finding stays `confirmed` on
    # evidence that no longer qualifies -- the gate is a check at the door, not
    # an invariant over time.
    #
    # This is the one thing a real truth-maintenance system does that this
    # graph does not: propagate a retracted justification to the beliefs
    # resting on it. Until it does, the propagation is THIS AUDIT, and an audit
    # nobody runs is not a safeguard -- so it is reported by default rather
    # than being an extra call.
    drift = query(
        "SELECT count() AS n FROM finding WHERE status='confirmed' "
        "AND count(->cites->source[WHERE strength IN ['reproduction','code']]) = 0 "
        "AND count(->cites->routine) = 0 GROUP ALL;")

    # A claim with a newer claim SUPERSEDING it that still reads as confirmed.
    # Traversal returns it, the digest carries it as settled, the hook offers
    # it as current. Ten of these were live the moment 93 made the supersession
    # graph queryable -- including a five-generation HUD chain where every
    # generation read `confirmed` at once. `corrects` deliberately does NOT
    # count: that is the narrower relation and the corrected claim still stands.
    stale = query(
        "SELECT record::id(id) AS id FROM finding "
        "WHERE count(<-supersedes) > 0 AND status = 'confirmed';")

    return {"status_distribution": dist,
            "confirmed_but_superseded": [r["id"] for r in stale],
            "findings_with_NO_status": (statusless[0]["n"] if statusless else 0),
            "DRIFT_confirmed_that_would_fail_the_gate_today":
                (drift[0]["n"] if drift else 0),
            "confirmed_without_qualifying_evidence": unbacked,
            "confirmed_without_qualifying_SOURCE (strict)":
                (strict[0]["n"] if strict else 0),
            "confirmed_unreachable_by_hook (no about edge)":
                (unattached[0]["n"] if unattached else 0),
            "dangling_edges": dangling_edges()}


def _claim(r):
    """Pull a displayable claim off a finding row.

    Reads statement -> summary -> title -> result -> note. This chain matters: the
    `finding` table has 55 distinct keys and 11 rows carry their claim in
    `summary` rather than `statement`. Reading only `statement` made
    finding:render_frame_positions_validated -- "maxDX=0.00px vs engine
    ASMTRACE, both fighters, every part", one of the best-evidenced rows in the
    graph -- render as an EMPTY LINE in the PreCompact digest.
    """
    for k in ("statement", "summary", "title", "result", "note"):
        v = r.pop(k, None)
        if v:
            r.pop("summary", None)
            r.pop("note", None)
            v = " ".join(str(v).split())
            return v[:110] + ("…" if len(v) > 110 else "")
    return ""


#: every edge table in the graph. Named explicitly so a new one has to be
#: added here deliberately -- the integrity check that only knew about `cites`
#: reported "0 dangling" while two `about` edges pointed at nothing.
EDGE_TABLES = ["cites", "about", "supersedes", "corrects", "fixes", "tried_on",
               "reads", "writes", "calls", "lives_at", "maps_to", "owns",
               "has_field", "part_of", "instance_of", "confirms"]


def dangling_edges():
    """Edges whose `in` or `out` record does not exist, per table.

    RELATE does not require its target to exist, so a citation can look
    satisfied in every traversal and resolve to nothing.
    """
    out = {}
    for t in EDGE_TABLES:
        try:
            rows_ = query("SELECT count() AS n FROM %s "
                          "WHERE out.id = NONE OR in.id = NONE GROUP ALL;" % t)
        except KBError:
            continue                       # table not present in this graph
        n = rows_[0]["n"] if rows_ else 0
        if n:
            out[t] = n
    return out


def docnote_queue():
    """The doc-ingest backlog, as a QUEUE with a drain rate.

    `docnote` is not an archive. Every ingest run adds to it; nothing removes
    from it unless someone triages. 900 of its rows carry doc_cue='confirmed',
    which is a CHECKMARK SCRAPED FROM MARKDOWN -- the same value that, when it
    was called `status`, made 478 changelog ticks indistinguishable from
    disassembly-backed findings. Reporting the queue depth is what stops that
    pile being mistaken for knowledge again.
    """
    total = query("SELECT count() AS n FROM docnote GROUP ALL;")
    done = query("SELECT count() AS n FROM docnote WHERE triaged = true GROUP ALL;")
    promoted = query("SELECT count() AS n FROM docnote "
                     "WHERE promoted_to != NONE GROUP ALL;")
    by_disp = query("SELECT disposition, count() AS n FROM docnote "
                    "WHERE triaged = true GROUP BY disposition ORDER BY n DESC;")
    t = total[0]["n"] if total else 0
    d = done[0]["n"] if done else 0
    return {"total": t, "triaged": d, "remaining": t - d,
            "promoted_to_findings": promoted[0]["n"] if promoted else 0,
            "by_disposition": by_disp}


def dead_ends(about=None):
    """What has already been ruled out, and what has already failed.

    The query that stops a re-walk. Almost no system stores this.
    """
    ruled = query("SELECT id, statement, tried, evidence, date FROM finding "
                  "WHERE status='ruled_out';")
    failed = query(
        "SELECT in.name AS approach, out.statement AS problem, outcome, note, date "
        "FROM tried_on WHERE outcome IN ['ineffective','masks_only'];")
    return {"ruled_out": ruled, "failed_or_masking": failed}


def false_wins():
    """Everything currently resting on a fix nobody can explain.

    The `masks_only` set. This project has repeatedly declared false wins;
    this is the query that lists them.
    """
    return query(
        "SELECT in.name AS approach, out.statement AS problem, note, date "
        "FROM tried_on WHERE outcome='masks_only';")


def status_contradictions():
    """Findings whose TEXT and STATUS disagree.

    Cheap, and it catches a real class. finding:stage_pernode_matrices_closed
    has the slug `_closed`, a statement opening "STAGE PROP/SCENERY RENDER --
    CLOSED", and status='open'. One of those is stale and the graph cannot say
    which -- but it can say that they conflict, which is the part a query can
    do and a reader cannot be relied on to notice.

    Deliberately NOT auto-corrected. Guessing which side is right is how a
    status becomes decorative.
    """
    closed_words = "CLOSED|RESOLVED|SOLVED|FIXED"
    open_but_closed = query(
        "SELECT record::id(id) AS id, status, string::slice(statement ?? '', 0, 90) AS claim "
        "FROM finding WHERE status IN ['open','inferred'] "
        "AND string::matches(string::slice(statement ?? '', 0, 80), "
        "'(?i)\\\\b(%s)\\\\b');" % closed_words)
    # the reverse: still `confirmed` while the text says it was disproved
    confirmed_but_refuted = query(
        "SELECT record::id(id) AS id, status, string::slice(statement ?? '', 0, 90) AS claim "
        "FROM finding WHERE status = 'confirmed' "
        "AND string::matches(statement ?? '', "
        "'(?i)\\\\b(SUPERSEDED BY|DISPROVEN|was wrong|no longer true)\\\\b');")
    # CANDIDATES, not defects. Each still has to be READ -- a `confirmed`
    # finding may legitimately contain the word RESOLVED. The value is that the
    # list is short enough to read, and anchored to the statement's OPENING,
    # where a verdict is actually stated. An unanchored match returned mostly
    # noise ("re-catalog records a RESOLVED conclusion" in an `open` row) --
    # the same loose-regex error that produced the handover's bogus "44 buried
    # dead ends".
    return {"candidates_open_but_text_says_closed": open_but_closed,
            "candidates_confirmed_but_text_says_superseded": confirmed_but_refuted}


def unreconciled_claims(min_claims=2, max_claims=5):
    """Small clusters of LIVE claims about one entity, NONE of them linked.

    NOT a contradiction detector, and the name says so. Nothing here reads the
    claims -- it cannot know that two statements disagree. What it finds is
    claims that share a subject and have no relationship recorded between them,
    which is where a genuine either/or hides.

    Nobody linked them, so traversal returns all of them and each looks
    authoritative. A graph does not detect this on its own -- you have to ask.

    This query was broken until 2026-09-01, and broken in the way that matters:
    it RETURNED something. 164 rows of mostly-empty arrays, because `about`
    was carrying both finding->entity and finding->finding, so grouping claims
    by "the thing they are about" grouped them by other claims. A detector that
    answers is never re-examined. 91_about_split.surql separated the two edges;
    this reads the fixed one.

    THE SIZE CAP IS THE WHOLE TRICK. Without it, routine:loc_8c0344d4 comes
    back with 1,035 "unarbitrated pairs" because 46 findings are about the body
    walker -- that is a POPULAR ENTITY, not a conflict, and burying the real
    pairs under it is the same false-positive failure that produced the
    handover's bogus "44 buried dead ends". A subject carrying 2-5 live claims
    is where an either/or plausibly lives; a subject carrying 46 is a topic.

    Arbitrated pairs are excluded: supersedes, corrects, AND relates_to. If
    someone recorded ANY relationship, the pair has been looked at.
    """
    edges = query(
        "SELECT record::id(in) AS f, in.status AS st, "
        "record::tb(out) AS tb, record::id(out) AS subj "
        "FROM about WHERE in.status IN ['confirmed','inferred'];")
    arb = set()
    for e in query("SELECT record::id(in) AS a, record::id(out) AS b "
                   "FROM supersedes, corrects, relates_to;"):
        arb.add((e["a"], e["b"]))
        arb.add((e["b"], e["a"]))

    by_subject = {}
    status = {}
    for e in edges:
        key = "%s:%s" % (e["tb"], e["subj"])
        by_subject.setdefault(key, set()).add(e["f"])
        status[e["f"]] = e["st"]

    out = []
    for subj, claims in by_subject.items():
        claims = sorted(claims)
        if not (min_claims <= len(claims) <= max_claims):
            continue
        unarbitrated = [(a, b) for i, a in enumerate(claims)
                        for b in claims[i + 1:] if (a, b) not in arb]
        if not unarbitrated:
            continue
        out.append({"subject": subj,
                    "claims": [{"id": c, "status": status.get(c)} for c in claims],
                    "unarbitrated_pairs": len(unarbitrated)})
    # fewest claims first: a 2-claim subject with no link between them is the
    # cleanest candidate for a real either/or
    out.sort(key=lambda r: (len(r["claims"]), r["subject"]))
    return out


def contradictions(*a, **kw):
    """Deprecated alias -- the old name overclaimed. See unreconciled_claims()."""
    return unreconciled_claims(*a, **kw)


if __name__ == "__main__":
    import pprint
    pprint.pprint(health())
