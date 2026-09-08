-- 210_supergun_netcode_library_verdict.surql
-- The SUPERGUN netcode library decision, 2026-09-05..07. AUTHORED, NOT APPLIED.
--
-- WHY THIS EXISTS. Every sibling verdict from the same design work is already in the graph --
-- supergun_v1_broker_transport_verdict, supergun_v3_snap_verdict, supergun_v4_adaptive_delay_verdict,
-- supergun_xdp_ebpf_verdict, supergun_state_streaming_verdict, supergun_two_mode_verdict,
-- supergun_server_authority_ruled_out. The library verdict was the only one living solely in a doc.
-- A query for 'ggrs' or 'gekko' over every finding returned ZERO on 2026-09-07. RE-METHOD: "a doc
-- alone is not a result", so the next session would have re-derived the whole comparison.
--
-- STATUS IS DELIBERATELY 'inferred', NOT 'confirmed'. Nothing was compiled or run; the load-bearing
-- inference (handle indirection, EVAL section 6.1 / U-L1) is untested and its gate cannot currently
-- fail. The schema's confirm gate would reject it anyway, correctly.
--
-- Paths use forward slashes only: a backslash in a single-quoted SurrealQL string is an invalid
-- escape, the request returns HTTP 400, and rekb.sh used to report that as success.

USE NS re DB kb;

UPSERT source:supergun_netcode_library_docs SET kind='static', strength='doc',
  ref='supergun/docs/SUPERGUN-NETCODE-LIBRARY-EVAL.md (commits ba1ec65, 9b44618, dc19b9c) and supergun/docs/SUPERGUN-NETCODE-LIBRARY-AUDITS.md (commit d7ffd03)',
  note='The audits are line-cited source reads at pinned revisions: gschup/ggrs main pushed 2026-08-25 (Cargo.toml declares 0.13.0; main is AHEAD of the v0.13.0 tag, so late spectators are Unreleased and a git rev must be pinned), and HeatXD/GekkoNet at 3b21722, 2026-09-02. The eval is the synthesis and carries the corrections applied after the 2026-09-06 cross-examination.';

UPSERT finding:supergun_netcode_library_verdict SET status='inferred', confidence='medium', date='2026-09-07',
  statement='SUPERGUN NETCODE LIBRARY VERDICT: adopt GGRS over GekkoNet for the rollback core, and do not fork it for v1. The two libraries are close on capability and NEITHER expresses the section 9.8 adaptive cap or the section 9.3 rotate-modulo-active_cap+1 ring rule, so that shared gap does not discriminate. The decision turns on three things. (1) VERIFICATION POSTURE, which is decisive on a project whose asset is its gates: GGRS has 118 tests, a CI test step, forbid(unsafe_code) and 4 open issues; GekkoNet has ZERO tests, a build-only CI, and correctness invariants that are assert-only and therefore ABSENT FROM RELEASE BUILDS (its own open issue 3, since 2024-07-15). (2) The predictor is a TYPE PARAMETER in GGRS (InputPredictor trait since 0.12.0, PredictRepeatLast is the shipped default) and hardcoded in GekkoNet -- and Gate N2 is the next gate, so in GGRS the control and the candidate are a type swap rather than a fork. (3) sg_runner is already Rust and passed Gates 1/2/3 byte-exact, so GGRS needs no FFI seam and no second toolchain. GekkoNet is the better DESIGN on spectators and match recording -- state-transfer-and-rebase rather than catch-up-from-frame-0, and a recorder whose write head rewinds on misprediction -- but both serve spectators off a PLAYING CLIENT uplink (GekkoNet re-blasts the whole 277,288 B state every 200 ms per joining spectator, about 1.4 MB/s, unicast), so the spectator tier is built outside the library either way and that advantage becomes a design to copy rather than a subsystem to inherit.',
  note='NOT ACTED ON YET, and two things gate acting. D1: whether section 9.8 adaptive cap and the extend-toward-64 overrun policy survive v1 is an OWNER RULING, not a library finding -- an unforked GGRS fixes max_prediction at construction, which retires both. See supergun_speculation_cap_is_a_recovery_ceiling: the cap is a loss-burst recovery ceiling and steady-state depth is owd, so the clamp rule buys ring working set (measured about 0.02 ms, 0.12 percent of a frame) and the real open question behind hard cap 64 is where a stall becomes preferable to a teleport. D2: Gate N7 as specified in EVAL section 9 CANNOT FAIL on the mechanism it names -- it sets max_prediction 32 against a 33-slot pool, so the recycle condition is false by arithmetic (M4), and it uses SyncTestSession rather than the P2P path the inference depends on. Respecify before believing U-L1. Reversal triggers R1-R6 are in EVAL section 8; R5 (dependency objection) is already largely dead because sg_runner/Cargo.lock has 54 package entries, not zero, so the marginal add is at most six crates. Adoption gate, non-negotiable: the library-driven runner must reproduce Gate N1 byte-exact at depths 0/8/30/60/120, ALSO comparing the 17 bank-pointer globals 0x142EDF588..0x142EDF620 which the current gate does not, with a depth-0 control arm that must PASS, a narrowed arm that must FAIL, and heap_wraps > 0 as a hard red. Adopting a library does not retire gate_n1c.py.';

RELATE finding:supergun_netcode_library_verdict->cites:e210_verdict_docs->source:supergun_netcode_library_docs;
