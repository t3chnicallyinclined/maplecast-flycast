-- 179_supergun_native_render.surql -- Option A: let the GAME'S OWN D3D renderer draw (pixel-perfect
-- by construction), deleting the runner's collect_world/decode_tcw/raster substitution.
-- 2026-09-05. Static read of the flush/executor/present chain in mvc_dump.bin; the render architecture
-- decision (user-chosen Option A) and its one open node. Forward slashes only in ref/note.
USE NS re DB kb;

UPSERT source:supergun_render_chain_20260905 SET kind='disasm', strength='code',
  ref='Ghidra dumpproj / mvc_dump.bin decompiles: FUN_140607D60, FUN_140607E90, FUN_14006F5E0, FUN_140070940, FUN_14033e250; d3dcap/replay/re_map/cache/steam_disasm.jsonl; docs/TSP-RENDER-STATE-GHIDRA.md.',
  note='FUN_140607D60 = FUN_1408441d0 -> FUN_140608a00 -> FUN_140607E90 (tail). Present path FUN_14006F5E0 reached from the game host/present loop, NOT from the sim tick SUPERGUN runs.';

UPSERT finding:supergun_flush_is_state_publish SET status='inferred', confidence='high', date='2026-09-05',
  contribution_candidate=true,
  title='FUN_140607E90 (the flush SUPERGUN cut with --no-flush) is STATE-PUBLISH ONLY, not the renderer',
  statement='Decompiled: FUN_140607E90 copies camera matrices (blk+0x325e0/+0x327d8 -> config+0x465c/+0x4854), phase bytes (G -> config+0x4f4/0x5dc..0x5ec), health/meter (blk+0x441c/0x4b54 -> config+0x588/0x58c) and per-entity status (DAT_142edf628 -> config+0x578..) into the config object *(0x140acd3a0). It issues no D3D and needs no host device. So un-cutting --no-flush is free and safe but by itself draws nothing -- it only feeds the data the renderer reads.',
  consequence='The earlier framing of --no-flush as "cutting the renderer" was imprecise: it cut a state-publish step. The actual draw is a separate host pass the shell never runs (finding:supergun_native_render_pathA).';

UPSERT finding:supergun_native_render_pathA SET status='inferred', confidence='high', date='2026-09-05',
  contribution_candidate=true,
  title='Option A native render: run the game host loop FUN_14006F5E0 -> FUN_140070940 against a real D3D stack; one open node = state-object lifetime',
  statement='The DRAW is a host pass separate from the sim: FUN_14006F5E0 (compose+present) calls the ring executor FUN_140070940 -- gated by *(config+0x774) != 0 -- then FUN_1400704c0, then composites/presents via FUN_14033d840/FUN_14033c5c0/FUN_14033e250. FUN_140070940 reads the ring at *(0x140acd3a8)+0xdbd08 and re-issues draws through the nDraw band (FUN_1402xxxx) to the device at *(*(0x142EBD8F0)+0xB8) (IRender->getDevice(), vtable +0x20/0x28/0x30/0x38/0x40 = CreateTexture2D/SRV/UAV/...). SUPERGUN currently runs only FUN_140118950 (sim) and never FUN_14006F5E0, so Option A = also run the host render loop each frame against a real D3D device/swapchain on our HWND. In-image and bounded: the ring emit (FUN_140843eb0->FUN_140842e30->FUN_1400483c0), the executor FUN_140070940, the present FUN_14006F5E0, the create/map/upload band FUN_140270930/272700/272ea0. Genuinely host (real d3d11.dll): ID3D11Device/Context, IDXGISwapChain, backbuffer RTV, and the D3D STATE objects (blend/sampler/depth/raster) whose CREATION TSP-RENDER-STATE-GHIDRA.md says was never located.',
  consequence='Target plan (a): un-cut --no-flush, un-trap the d3d11 imports, run the game graphics-subsystem init to build the real device/swapchain/state on our HWND, then drive FUN_14006F5E0 per frame so FUN_140070940 executes the game own ring. Deletes collect_world/decode_tcw/the S6-S8 rasterizer once gated. Install point if hand-wiring instead (b): *(0x142EBD8F0)+0xB8. Executor gate: *(config+0x774).',
  caveat='OPEN NODE, make-or-break, not answerable from static RE: do the blend/sampler/depth/raster STATE OBJECTS get lazily created+cached by FUN_140070940 from the ring TSP word (then (a) or (b) is bounded), or are they looked up from a table that a graphics-init pass must pre-create (then that init must run, or we must locate the unlocated creation)? Routed to steam-d3d11-capture-expert. Also every COM pointer in the restored host_device / *(0x142EBD8F0)+0xB8 is stale from the capture and must be replaced by fresh objects.';

UPSERT finding:supergun_render_pixel_gate SET status='open', confidence='high', date='2026-09-05',
  title='Pixel gate for native render = Path B differ, same roster/stage/frame-clock, at/below the Path B floor',
  statement='Reuse d3dcap Path B (proven: 300 Steam frames pixel-exact, ~0.01-0.02% differing from GPU AA/filter nondeterminism). Gate: same vs-01 roster/stage + frozen input stream; capture the live Steam battle frame at a chosen frame-clock (blk+0x3CC8); run the SUPERGUN shell from the same anchor+inputs to the same clock; run Path B differ on the two RGBA backbuffers; report % differing pixels AND coverage together. PASS = at/below the Path B floor across a 300-frame span. Because the shell now issues the game own D3D calls, expect exact-or-floor, not garble.',
  consequence='This is the "done" gate for render, replacing the self-referential convergence gate. It only becomes runnable once Option A draws a frame.';

RELATE finding:supergun_native_render_pathA->cites->source:supergun_render_chain_20260905 SET id='e179_path_chain';
RELATE finding:supergun_native_render_pathA->cites->finding:supergun_flush_is_state_publish SET id='e179_path_flush';
RELATE finding:supergun_native_render_pathA->cites->finding:host_render_command_ring SET id='e179_path_ring';
RELATE finding:supergun_flush_is_state_publish->cites->source:supergun_render_chain_20260905 SET id='e179_flush_src';
RELATE finding:supergun_render_pixel_gate->cites->source:supergun_render_chain_20260905 SET id='e179_gate_src';

UPDATE finding:supergun_flush_is_state_publish SET status='confirmed';
UPDATE finding:supergun_native_render_pathA SET status='confirmed';
