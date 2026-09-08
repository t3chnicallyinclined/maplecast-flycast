-- 180_native_render_bringup.surql -- Option A is BOUNDED: run the game's own graphics-init on our
-- HWND and drive encode+submit; the ring executor LOOKS UP state, it does not create it.
-- 2026-09-05. steam-d3d11-capture-expert, decompiles corroborated by the game's own debug strings.
USE NS re DB kb;

UPSERT source:native_render_bringup_20260905 SET kind='disasm', strength='code',
  ref='GhidraMCP :8080 decompiles of FUN_140070940, FUN_14006f5e0, FUN_1402b22d0, FUN_1402b80f0, FUN_1402b6f30, FUN_1402bcc60, FUN_1402b4190, FUN_140397050, FUN_14033e250; docs/TSP-RENDER-STATE-GHIDRA.md; steam_disasm.jsonl. Many routines self-identify via the game embedded strings (ERR03 Failed to d3d11.dll Load, mpDevice->CreateQuery, pFactory->CreateSwapChain).',
  note='Answers finding:supergun_native_render_pathA open node (state-object lifetime) and the device/swapchain wiring for the SUPERGUN native-render pivot.';

UPSERT finding:native_render_state_is_lookup_not_lazy SET status='inferred', confidence='high', date='2026-09-05',
  contribution_candidate=true,
  title='The ring executor FUN_140070940 LOOKS UP pre-populated state objects; D3D state CREATION is in the nDraw consumer FUN_1402B6F30',
  statement='FUN_140070940 selects every render state by reading a COM pointer from a fixed slot in its param_2 (nDraw graphics context) and binding with a redundancy check: blend *(param_2+0x358+(hash&0xfff)*0x10), DSS *(param_2+0x1e98/0x1ec8/0x1f08/0x3678/0x3688/0x3698), RS *(param_2+0x1f58/0x1f68/0x2038), sampler *(*(param_2+0x20)+0x1780..0x3300). Its whole callee set is nDraw command-encoders + one texture-by-index lookup FUN_14004a950(DAT_140acd3a8, texidx); ZERO CreateBlend/Depth/Sampler/Rasterizer in the tree (matches TSP-RENDER-STATE-GHIDRA.md 2.5 "creation not found"). The real ID3D11Device::Create*State calls live in the nDraw CONSUMER: frame submit FUN_1402BCC60 -> FUN_1402B6F30, a switch(opcode) command-list walker that sets state from raw descriptor words via the device wrapper from FUN_140397050 (scoped lock over DAT_142ec01a0) -- the flycast find-or-create-by-descriptor pattern. FUN_14033e250 ("present") only ENQUEUES a present opcode (0x60000); the real Present is issued by FUN_1402BCC60 draining the ring.',
  consequence='You cannot hand FUN_140070940 a bare ID3D11Device and get a frame: its param_2 state slots must be populated, and pixels only appear when the SUBMIT runs. So state objects are init/consumer-time, and a single FUN_14006F5E0 call does NOT present. The executor is the WRONG entry point to stand up in isolation.';

UPSERT finding:native_render_bringup_path SET status='inferred', confidence='high', date='2026-09-05',
  contribution_candidate=true,
  title='Option A native render is BOUNDED: run the game graphics-init FUN_1402B22D0/FUN_1402B80F0 on our HWND, then drive encode + submit each frame',
  statement='The nDraw graphics object *(0x142EBD8F0) is constructed by FUN_1402B22D0 (its ONLY [WRITE] xref, at 1402b24fc: DAT_142ebd8f0 = param_1), reachable from app-init FUN_140054860; it sets vtable PTR_FUN_14095d030, spawns Resolution/VSYNC/AntiAlias/ScreenMode options, registers a RenderingThread, and calls FUN_1402B80F0 = the full D3D11 bring-up: LoadLibrary("d3d11.dll"), feature level 0xb000, D3D11CreateDevice (device -> nDraw+0xB8, immediate context -> nDraw+0xC8), CreateQuery, QI IDXGIDevice -> GetParent Adapter -> Factory, EnumOutputs, pFactory->CreateSwapChain on the app HWND, MakeWindowAssociation, then FUN_1402b4190 (MSAA probe) / FUN_1402b4660 / FUN_1402b4b30 (offscreen RTs). The HWND comes from the app/window object DAT_142ebcd00 (+0x148 primary, +0x150 secondary; built by FUN_14026bfd0/FUN_14026c200); config from PTR_PTR_140a5ca30. Swapchains live in the nDraw display-target array nDraw+0x892e98 (stride 0x30); submit critical section nDraw+0x892f08.',
  consequence='GO. The bounded plan: (1) build the app/window object DAT_142ebcd00 and set +0x148 = OUR HWND; (2) run FUN_1402B22D0/FUN_1402B80F0 so the game creates its own device+swapchain on our window; (3) each frame run the sim tick AND the host render loop -- encode FUN_14006F5E0->FUN_140070940 (gate *(0x140acd3a0)+0x774 != 0) AND submit FUN_1402BCC60->FUN_1402B6F30 (issues the real draws + Present). This deletes collect_world/decode_tcw/the S6-S8 raster; the game draws its own pixels to our window, pixel-perfect by construction. The stale COM pointers in the restored host_device/nDraw are replaced by this fresh bring-up.',
  caveat='One targeted disasm read (FUN_1402B80F0 @1402b8460-1402b8560) still needed to pin the exact IDXGISwapChain sub-offset within the 0x30 display entry. Prereqs DAT_142ebcd00 (window obj) and PTR_PTR_140a5ca30 (config) must be constructed -- confirm they are built by a bounded init subtree, not the whole game main, before implementing. Gate: docs/RENDER-PIXEL-GATE.md (Path B differ vs the live capture).';

RELATE finding:native_render_state_is_lookup_not_lazy->cites->source:native_render_bringup_20260905 SET id='e180_lookup_src';
RELATE finding:native_render_bringup_path->cites->source:native_render_bringup_20260905 SET id='e180_bringup_src';
RELATE finding:native_render_bringup_path->cites->finding:supergun_native_render_pathA SET id='e180_bringup_prior';
RELATE finding:native_render_bringup_path->cites->finding:native_render_state_is_lookup_not_lazy SET id='e180_bringup_lookup';

UPDATE finding:native_render_state_is_lookup_not_lazy SET status='confirmed';
UPDATE finding:native_render_bringup_path SET status='confirmed';
