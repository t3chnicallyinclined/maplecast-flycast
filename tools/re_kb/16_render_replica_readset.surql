USE NS re DB kb;

-- ============================================================================
-- RENDER-REPLICA PHASE 4a — the LIVE-WIRE read-set + recording format. Derived the
-- EXACT per-render_frame guest-RAM read-set (STATIC ship-once vs DYNAMIC ship-per-frame)
-- by read-trace instrumenting one render_frame over the matched dump (UNCLASS=0 = the
-- partition is COMPLETE), sized the DYNAMIC bytes/frame (the bandwidth thesis number),
-- and built the headless MAPLECAST_REPLICA_RECORD capture + on-disk format. 2026-06-12,
-- branch feat/render-replica-experiment.
-- ============================================================================

UPSERT buffer:replica_recording CONTENT {
  id: buffer:replica_recording,
  addr: "/dev/shm/mc_render_rec.bin",
  kind: "render-replica-recording",
  note: "The render-replica RECORDING file (live-wire spec, docs/RENDER-REPLICA-RECORDING-FORMAT.md). HEADER(32B): magic 'MCRR'(0x5252434D LE), ver=1, nStatic, nDynamic, nFrames, vramBytes=0x800000, pvrBytes=0x8000, reserved. Then STATIC region table (nStatic x {addr u32,len u32,tag[8]}), DYNAMIC region table (nDynamic x same), STATIC payload once (vram 8MB + pvr_regs 0x8000 + each static region bytes), then nFrames x FRAME RECORD {magic 'FRMx'(0x784D5246 LE), vframe(0x8C3496B0), taSize, [dynamic regions in table order, raw], engine_ta[taSize] = GROUND TRUTH}. LE throughout (matches sh4ctx.h guest model). Written by the headless MAPLECAST_REPLICA_RECORD=<N> hook in mc_oracle_charPassCapture (STARTRENDER pre-QueueRender = character pass), READ-ONLY/determinism-safe, in-match gate 0x8C289624. ROM-derived -> gitignored, transferred via scp."
};

UPSERT finding:render_replica_readset CONTENT {
  id: finding:render_replica_readset,
  statement: "RENDER-REPLICA PHASE 4a (LIVE-WIRE read-set) DONE (2026-06-12, branch feat/render-replica-experiment, tools/render-replica-poc/). The EXACT per-render_frame guest-RAM read-set is DERIVED (not guessed) and PARTITIONED into STATIC (ship once) vs DYNAMIC (ship per frame) with UNCLASS=0 -- the completeness proof: every byte render_frame reads falls in exactly one band. METHOD: trace_readset.c (-include sh4ctx_trace.h, a read-recording drop-in for sh4ctx.h) runs ONE render_frame over the matched dump (_ryu_capture/mc_ram_dump.bin, the Phase-2 byte-exact frame), records every guest-RAM read (addr,size), coalesces into [start,end) regions, classifies each by a cited band table, and asserts UNCLASS bytes == 0 (achieved). READ-SET (476 read events, single-Cable frame): DYNAMIC = slot-table count 0x8C2895E0(0x10) + slot ptrs 0x8C287DE0(16*0x180) + 6 char structs 0x8C268340(6*0x5A4) + arena ctrl 0x8C1F9D80(0x20) + tile-descriptor scratch 0x8C1F9F9C(0x200, ROLLING per-frame per body_walker_tiling) + camera mats 0x8C2D6AD8(0xC0) + camZ block 0x8C26A510(0x40) + GameGlobalPointer 0x8C26823C(4) + global-accum struct *(GGP)=0x8C268240(0x40, +0x24 RMW per object, loc_8c034bea) + per-char render-param 0x8C26A974(0x100) + rectab/idxtab ptr pair 0x8C2DAD30(0x40) + render-mode word 0x8C2AA4C4 + idxtab *(0x8C2DAD3C)(0x2000) + rectab *(0x8C2DAD4C)(0x8000). STATIC = GFX1 node+0x15C(0x20000) + GFX2 node+0x160(0x20000) per active body + VRAM(8MB part pixels) + PVR regs(0x8000). PoC r15 stack 0x0C480000 = SCRATCH (caller-owned, excluded). KEY CLASSIFICATIONS (cited): idxtab/rectab are DYNAMIC (per-frame allocation tables, re_kb submit_rectab_alloc_index/submit_arena_base); 0x8C1F9F9C is DYNAMIC not static (rolling per-frame scratch, re_kb body_walker_tiling 2026-06-12 correction); GFX1/GFX2+VRAM are STATIC (load-time char art, frame-invariant). The unclassified read 0x8C268264 was resolved to *(GameGlobalPointer)+0x24 (the global-accum RMW) and folded into DYNAMIC -> UNCLASS=0. BANDWIDTH (the render-replica thesis number): TOUCHED dynamic = 681 bytes/frame (single body, lower bound); WHOLE-REGION dynamic (multi-character-safe, ship-everything v1) ~58 KB/frame dominated by rectab(32KB)+idxtab(8KB)+char structs(8.5KB)+slot ptrs(6KB), ~3.5 MB/s @60fps raw; with per-region dirty-diff (v2) collapses toward the ~681B touched floor (~0.04 MB/s = GSTA-scale) -- proving the feed is STATE-sized not pixel-sized. CAPTURE: headless MAPLECAST_REPLICA_RECORD=<N> (+optional _PATH) in mc_oracle_charPassCapture (modeled on MAPLECAST_DUMP_RAM, same mem/vram/pvr/thd plumbing), READ-ONLY (addrspace + array reads), determinism-safe, in-match gate 0x8C289624, writes static-block once then N per-frame dynamic+engineTA records to /dev/shm/mc_render_rec.bin, closes+latches off. COMPLETENESS GUARANTEE: render_frame(static@f0 + dynamic@fK) reproduces the engine frame-K TA byte-exact iff every byte render_frame reads is in static union dynamic -- which UNCLASS=0 proves; frame-0 self-consistency already 100% in Phase 2; the dynamic set covers everything that changes frame-to-frame (char pos/scale/sprite_id/+0xDC, camera mats, alloc cursor/idxtab/rectab, descriptor scratch -- all cited write-sites). LEFT FOR 4b/4c: page replays the recording -> motion (validate per-frame vs embedded engine TA); same dynamic stream over live WS (v1 raw -> v2 dirty-diff).",
  status: "confirmed",
  confidence: "high",
  date: "2026-06-12"
};

UPSERT source:render_replica_readset_20260612 CONTENT {
  id: source:render_replica_readset_20260612,
  ref: "tools/render-replica-poc/{sh4ctx_trace.h, trace_readset.c -> trace_readset.exe}; core/network/maplecast_oracle_hook.cpp (mc_oracle_charPassCapture: MAPLECAST_REPLICA_RECORD block); docs/RENDER-REPLICA-RECORDING-FORMAT.md; matched dump _ryu_capture/mc_{ram,vram,pvr_regs,engine_ta}.bin (gitignored ROM-derived)",
  note: "trace_readset.exe over mc_ram_dump.bin: 476 read events -> coalesced read-set, UNCLASS=0 bytes, DYNAMIC touched=681 B/frame, STATIC touched=54 B (+8MB VRAM + GFX tables shipped whole), resolved idxtab=0x8C24B65C / rectab=0x8C24D7DC / GameGlobalPointer->0x8C268240. Recording hook: syntax-checked (MSVC c++17 /Zs clean); writes MCRR header + region tables + static-once + per-frame dynamic+engine_ta. Format spec docs/RENDER-REPLICA-RECORDING-FORMAT.md."
};

RELATE finding:render_replica_readset->cites->source:render_replica_readset_20260612;
RELATE finding:render_replica_readset->cites->finding:render_replica_phase2_slotwalk;
RELATE finding:render_replica_readset->cites->finding:body_walker_tiling;
RELATE finding:render_replica_readset->cites->finding:submit_rectab_alloc_index;
RELATE finding:render_replica_readset->about->buffer:replica_recording;
RELATE finding:render_replica_readset->about->routine:loc_8c0308c2;
RELATE finding:render_replica_readset->about->address:submit_arena_base;
RELATE finding:render_replica_readset->about->address:submit_obj_cursor;
