-- 139_arc_coverage_aux_banks.surql -- a LOCATED arc-coverage gap: auxiliary banks that FUN_14060c370 loads
-- and dcram_build.py never placed. SUPERGUN ENGINE lane, 2026-09-04 (odd seeds).
-- Doc: mvc-live-skins-quarters/docs/RECEIPT-RUNNER-GATE4.md addendum B ; tool d3dcap/receipt/arc_coverage.py
-- Idempotent and replayable; explicit edge ids; additive fields only on findings owned by earlier seeds.
USE NS re DB kb;

UPSERT source:tool_arc_coverage SET kind='tool', strength='code',
  ref='mvc-live-skins-quarters/d3dcap/receipt/arc_coverage.py',
  note='Routes every System-A node pointer to the region it actually lives in (dcram / blk / blk2 / exe) and reports DC-RAM pointers landing outside what dcram_build.py places. Two-pass so the verdict reflects the demand-driven aux banks. node+0xE8 is NOT a DC-RAM pointer (RECEIPT-RUNNER-DCRAM s4.2: raft children carry +0xE8 = parent+0xA8, another node matrix inside blk); only +0xA0 can be a coverage gap. A first pass that treated +0xE8 as DC-RAM produced 27-35 false OUTSIDE hits per anchor.';

UPSERT finding:arc_gap_aux_banks SET status='confirmed', confidence='high', date='2026-09-04',
  contribution_candidate=true,
  title='Arc-coverage gap located: auxiliary (non-battle) banks are referenced by nodes but were never placed',
  statement='Ticking an arc-rebuilt anchor faults at 0x140848FF4 inside FUN_140848EE0 with RAX = 0xCDCDCDCD. Decoded: 140848FE2 MOVSXD RAX,[RDI+0x4c] loads the RECORD SIZE, not a pointer; read as 0xCDCDCDCD it sign-extends to -842150451, so RDI += 0x50 + RAX jumps about 800 MB backwards and the terminator CMP at 0x140848FF4 faults. The cause is NOT class E and NOT the four battle banks: dcram_build.py reproduces the effects/HUD/common/stage POL byte-exactly INCLUDING the 0xCD past each file end (the live game leaves 0xCD there too), and every bank last model terminates on a zero PCW inside the final 8 bytes of its file. The cause is class X: an auxiliary bank that FUN_14060c370 loads and the builder never placed. Proven on receipt-20260903-151200-post9, where FIVE DRAWN list-5 nodes reference 0x0D2BB1A0..0x0D2CE5C8, the RESULTS bank.';

UPSERT finding:results_bank_loader_rule SET status='confirmed', confidence='high', date='2026-09-04',
  contribution_candidate=true,
  title='Results bank load rule (FUN_14060c370 case 10), and why it must be placed DEMAND-DRIVEN',
  statement='FUN_14060c370 case 10: G+0xAD == 1 selects POL 0x36A (874) and TEX 0x36B (875); == 2 selects 0x36C / 0x36D; otherwise 0x34D (845) / 0x34E (846). Then FUN_14060dcf0(POL, 0x0D2BB000), FUN_14060dcf0(TEX, 0x0D2E9000), FUN_14060d770(0x0D2BB000, 0x0D2E9000). An independent archive match against the live post9 image returns AFS 874 at 0x0D2BB000, so code and data agree. CRITICAL: G+0xAD is only meaningful when case 10 last ran. At a BATTLE anchor it reads 0 (the default pair) while the bytes resident at 0x0D2BB000 are whatever the previous results screen left (measured: variant 1 on river_b and stage9), so placing the bank unconditionally makes the arc image diverge from live by 73,510 B on anchors that never read it. dcram_build.py therefore places it only when the anchor own nodes point into it. This is the same staleness class as blk+0x6D04.',
  gate='post9: bank placed, POL residual 131 B and TEX residual 3,150 B (class B, runtime uploads), and the only UNEXPLAINED entry is PL3/232, the documented post-match clobber of PL slot 1 (pos 3) by the results screen. Battle anchors stage9 / river_a / river_b / 000941: not referenced, not placed, UNEXPLAINED NONE. Regression: the arc-built stage-9 image still reproduces the CONFIRMED dump-based run on 300 of 300 blk dumps, 0 differ.';

UPSERT finding:dcram_classes_elx_proven_unread SET
  scope_correction='2026-09-04: NARROWED. E, L and X were proven unread on four BATTLE anchors. Class X is demonstrably READ on non-battle anchors: receipt-20260903-151200-post9 has 5 drawn list-5 nodes in the results bank (0x0D2BB000) and receipt-20260903-165128 has 8 in the case-0xD bank (0x0D6CC000). See finding:arc_gap_aux_banks. Note the 0xCD/0x5A poison arms are NOT equivalent for a record walk: 0x5A5A5A5A is a positive PCW and terminates the walk, 0xCDCDCDCD is negative and crashes.';
UPSERT finding:gate3_arc_dcram SET
  scope_correction='2026-09-04: the claim that a receipt runs from the arc alone holds for the BATTLE anchors gated; it is not unconditional. Located exception: auxiliary banks, finding:arc_gap_aux_banks.';

UPSERT finding:arc_gap_aux_banks_open SET status='open', confidence='high', date='2026-09-04',
  title='Six auxiliary banks remain unplaced; one is demonstrably referenced',
  statement='receipt-20260903-165128 (a character-select frame) has 8 drawn list-5 nodes referencing 0x0D6CC068..0x0D6D9C38, the bank FUN_14060c370 case 0xD loads at 0x0D6CC000 (POL AFS 851, INFERRED by archive match). Six further aux banks exist at 0x0D25C000, 0x0D3A2000, 0x0D4CE000, 0x0D720000, 0x0D7A9000 and 0x0D7CC000 (POL AFS 839, 847, 849, 853, 855, 843, all INFERRED by archive match against a live image). None are placed, because FUN_14060c370 loads them through a shared switch tail whose TEX destination address this pass did not trace, and FUN_14060d770 needs both the POL and TEX addresses to relocate.',
  next_step='Trace the shared tail of FUN_14060c370 to recover each (POL, TEX) destination pair, then extend dcram_build.py the same demand-driven way as the results bank.';

RELATE finding:arc_gap_aux_banks->cites:g5_gap_tool->source:tool_arc_coverage;
RELATE finding:arc_gap_aux_banks->cites:g5_gap_run->source:gate4_run_20260904;
RELATE finding:results_bank_loader_rule->cites:g5_res_tool->source:tool_dcram_poison;
RELATE finding:results_bank_loader_rule->cites:g5_res_run->source:gate4_run_20260904;
RELATE finding:arc_gap_aux_banks_open->cites:g5_open_tool->source:tool_arc_coverage;
