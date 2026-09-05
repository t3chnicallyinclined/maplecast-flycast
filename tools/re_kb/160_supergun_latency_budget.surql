-- ============================================================================
-- 160: SUPERGUN input-to-photon latency budget (senior-re-generalist, 2026-09-05)
-- Doc: C:\Users\trist\projects\supergun\docs\SUPERGUN-LATENCY-BUDGET.md
--
-- RE METHOD step 1/3/4. The Q1 result is an annotation transfer from seed 75
-- (DC input latch loc_8c010080 / Input_DEC 0x8C2681DC) to the Steam side by
-- global reference translation (blk+0x3C66, stride 0x14, cur/prev at +0x00/+0x02
-- on BOTH sides), then confirmed by reading both bodies AND by a deterministic
-- three-arm gate on a frozen anchor.
--
-- ALL nodes idempotent (UPSERT). Edges via RELATE -> replay reports
-- "already exists" (M2), which is an artefact, not a failure.
-- Do NOT run 07_dedup_edges.surql after this.
-- ============================================================================
USE NS re DB kb;

-- ---- routines (Steam) -------------------------------------------------------
UPSERT steam_routine:FUN_14060b7d0 SET name='FUN_14060b7d0', addr='0x14060b7d0',
  label='Input_Latch_Steam',
  role='per-frame input latch AND frame-clock increment; the Steam counterpart of DC loc_8c010080 (Input_Translate)',
  note='Called first thing in the frame body FUN_140608a00 (at 0x140608a09). Body: INC dword [G+0x10] at 0x14060b7e6 where G = *0x142edf580 = blk+0x3CB8, so the FRAME CLOCK blk+0x3CC8 is incremented by the input-latch routine itself. Then for i in 0..1: reads cur at blk+0x3C66+i*0x14 (0x14060b7f7) and copies it to prev at +0x02; calls FUN_140048630(i) to translate the raw pad word; stores cur (0x14060b81b), pressed = ~prev & cur (0x14060b835), released = ~cur & prev (0x14060b84c). Input_DEC layout on Steam: +0x00 cur, +0x02 prev, +0x04 pressed, +0x06 released, +0x10/+0x12 menu auto-repeat (FUN_14060b790). CONFIRMED by disassembly and by the whole-frame p-code trace (indices 80..145).';
UPSERT steam_routine:FUN_140048630 SET name='FUN_140048630', addr='0x140048630',
  label='Pad_Word_Bit_Remap',
  role='PURE STATELESS translate of one raw pad word into the CPS2-style Input_DEC bitmask',
  note='Reads one dword at game_state+0x218 + seat*4 (0x140048643), then twelve TEST/OR pairs against the (src,dst) mask table at DAT_140a4f780, returns the OR in EAX. NO memory, NO buffer, NO history of any kind: this is the place a one-frame input latch could have hidden and it does not exist. Table cross-checks against the DC bitmask in seed 75: raw 0x20 -> 0x0400 (RIGHT), raw 0x80 -> 0x0800 (LEFT), which are the values bank01.asm uses. CONFIRMED both sides.';
UPSERT steam_routine:FUN_140634920 SET name='FUN_140634920', addr='0x140634920',
  label='Fighter_Input_Latch',
  role='copies Input_DEC into every fighter slot, once per frame, BEFORE the battle logic runs',
  note='fighter[0]+0x668 <- blk+0x3C66 (0x140634947), +0x66A = previous value, +0x66C = ~prev & cur (pressed), +0x66E = ~cur & prev (released); the same for fighter[1] from blk+0x3C7A. Then MOVUPS-copies the seat-0 0x14-byte input block from blk+0x4420 to slots 2 (blk+0x5290) and 4 (blk+0x6100), and the seat-1 block from blk+0x4B58 to slots 3 (blk+0x59C8) and 5 (blk+0x6838) -- i.e. P1 = EVEN slots, P2 = ODD slots, assists share the point characters input. Slot base blk+0x3DB8 + k*0x738, input block at +0x668. Called from FUN_140619840, itself called from the mode driver FUN_140611a20 at 0x140611a36, BEFORE the mode dispatch at 0x140611a52. CONFIRMED.';
UPSERT steam_routine:FUN_140611a20 SET name='FUN_140611a20', addr='0x140611a20',
  label='Game_Mode_Driver',
  role='per-frame mode driver: input latch, then dispatch on the mode byte',
  note='CALL FUN_14062e090; INC [*0x142edf580 + 0x80]; CALL FUN_140619840 (which calls the fighter input latch FUN_140634920) at 0x140611a36; then JMP [0x140a6bfc8 + mode*8] at 0x140611a52 where mode = byte [*0x142edf580 + 0x01]. The ORDER is the load-bearing fact: input is latched into the fighter structs before any battle logic runs. Its own caller is an INDIRECT call and was NOT located statically (no imm/dataref to 0x140611a20 anywhere in the 29,678-function export) -- the frame-level ordering is anchored on the p-code trace instead.';
UPSERT steam_routine:FUN_1408436a0 SET name='FUN_1408436a0', addr='0x1408436a0',
  label='TA_Display_List_Emitter',
  role='NaomiLib TA / polygon record emitter -- writes the frames display list into ctx',
  note='Writes 15,990 B per frame into ctx[0x30 .. 0x1e0084) (measured from the chain p-code trace). Its output cursor is ctx+0x1e0080 and it bounds-checks that cursor against 0xE0000 = 917,504 B at 0x1408436c6, so the display-list arena is ~900 KB and a real frame uses ~16 KB. Contains 109 MOVSS/MOVUPS/MOVAPS instructions (fingerprint consistent with writing transformed vertex floats rather than pointers -- NOT a read, see finding:supergun_ctx_display_list_completeness_open). Pair: loc_8c127c80.';
UPSERT steam_routine:FUN_1402b80f0 SET name='FUN_1402b80f0', addr='0x1402b80f0',
  label='D3D11_Device_And_Swapchain_Bringup',
  role='the shipped games D3D11 device + DXGI swapchain creation',
  note='LoadLibrary("d3d11.dll") at 0x1402b84d1/0x1402b84d8 (string 0x14095d180, error "ERR03 : Failed to d3d11.dll Load." 0x14095d190) -- so d3d11 is NOT in this builds IAT at all. CALL [RAX+0x50] on the IDXGIFactory at 0x1402b87b1 = IDXGIFactory::CreateSwapChain (the LEGACY pre-1.2 interface), followed immediately by the assert string 0x14095d3b0. CALL [RAX+0x40] at 0x1402b87ed = MakeWindowAssociation (string 0x14095d400). The game QueryInterfaces IDXGIDevice (0x14095d2c0), NOT IDXGIDevice1.';

-- ---- fields / globals -------------------------------------------------------
UPSERT field:steam_input_dec SET name='Input_DEC (Steam)', offset='blk+0x3C66, stride 0x14, seat 1 at blk+0x3C7A',
  owner='blk', type='struct[0x14]', class='logical',
  note='The block-map image of DC Input_DEC 0x8C2681DC. +0x00 cur, +0x02 prev, +0x04 pressed (~prev & cur), +0x06 released (~cur & prev), +0x10/+0x12 menu auto-repeat state. Written ONLY by FUN_14060b7d0, once per frame, before the sim. Cur/prev at +0x00/+0x02 matches the DC layout exactly (seed 75).';
UPSERT field:fighter_input_block SET name='fighter input block', offset='fighter+0x668 (blk+0x3DB8 + k*0x738 + 0x668)',
  owner='fighter', type='struct[0x14]', class='logical',
  note='+0x668 cur, +0x66A prev, +0x66C pressed, +0x66E released. Written by FUN_140634920 from Input_DEC every frame; read by the fighter move logic FUN_140623b40 at 0x140623b4e. Slot 0 -> 2,4 (P1) and slot 1 -> 3,5 (P2).';
UPSERT global:ctx_ta_cursor SET name='ctx+0x1e0080 TA write cursor', address='ctx+0x1e0080',
  note='The NaomiLib display-list write cursor. FUN_1408436a0 checks cursor + len against 0xE0000 (917,504 B) at 0x1408436c6 before emitting. The list itself starts at ctx+0x30.';

-- ---- findings ---------------------------------------------------------------
UPSERT finding:mvc2_zero_internal_input_latency SET
  statement='MvC2 HAS ZERO FRAMES OF INTERNAL INPUT LATENCY ON THE STEAM RECOMPILE. The pad word handed to FUN_140118950 for frame N is read, translated, latched into the fighter struct, consumed by the fighter move logic, integrated into world position AND turned into the sprite walkers screen coordinates, all inside that same call. Two independent lines of evidence. (1) ORDER, from the whole-frame p-code trace (%TEMP%\\rrcap_emu\\frame_20260903-000941_tick_r0.trace.bin, trace index in brackets): FUN_140118950 writes game_state+0x218 [10-44] -> FUN_14060b7d0 increments the frame clock blk+0x3CC8 [80-85] -> FUN_140048630 READS game_state+0x218 [92] -> Input_DEC blk+0x3C66 cur/pressed/released written [108-145] -> FUN_140634920 latches fighter+0x668 [317-358] -> FUN_140623b40 reads fighter+0x668 [744/1355] -> FUN_140622ae0 writes fighter+0x50 world X [2134] -> the sprite walker writes fighter+0x124 screen X [196986] -> the submit reads it [197020]. (2) DETERMINISTIC GATE, rr_runner.exe on the frozen stage-9 anchor, six ticks, ONE variable changed (the seat-0 pad word), three arms: at TICK 1, world X is -134.167 (no input) / -128.333 (RIGHT 0x20) / -139.167 (LEFT 0x80) and screen X is 185.834 / 191.667 / 180.834. The zero arm was re-run and gave 6/6 byte-identical blk dumps. M4: the gate can fail and discriminates with correct signs. M6: px is stable at rest (py is NOT -- the idle bob moves it -4.286/-6.429/-8.571 -- and was deliberately excluded); walking also cancels the bob, a second independent signature. CONSEQUENCE: the DC lagProbe chain "kcode@T -> raw maple byte@T+1 -> gameplay@T+2" (finding:input_latch_once_per_vblank) does NOT transfer -- the T->T+1 step is a MAPLE DMA term and there is no maple on Steam. There is no engine-side frame for SUPERGUN or any competitor to reclaim.',
  status='confirmed', confidence='high', date='2026-09-05', contribution_candidate=true;
UPSERT finding:gate0_input_shift_is_tape_indexing SET
  statement='The Gate 0 "--input-shift 1" result (WORKSTREAM-RECEIPT-RUNNER.md:63, "row Ns seat_in are the inputs that PRODUCED frame N; shift 0 gave px 38/60 with a tick-2 divergence, shift 1 exact") is a TAPE-INDEXING CONVENTION, NOT engine latency. The engine-latency reading predicts that tick 1 with a direction held is byte-identical to the no-input arm and that divergence first appears at tick 2. finding:mvc2_zero_internal_input_latency ran exactly that test and tick 1 diverges in both world X and screen X, so the prediction is falsified. The observed shape corroborates: an off-by-one that only bites once the input stream stops being constant is what a mis-paired index looks like, not what a pipeline stage looks like. Separately, the "first direction frame 55, character moved frame 56" observation is a REPORTING artefact of play.rs, which captures px_dir AFTER the tick of the first-direction frame and can therefore never report 0.',
  status='confirmed', confidence='high', date='2026-09-05', contribution_candidate=true;
UPSERT finding:no_graphics_call_inside_the_tick SET
  statement='NO GRAPHICS-API CALL OF ANY KIND HAPPENS INSIDE FUN_140118950 / FUN_140607d60, for a steady in-battle frame. The p-code harness logs EVERY pc that leaves the executable image as an extcall record, regardless of whether it went through the IAT. Across a whole frame (tick and chain traces, 313,754 records) there are 24 extcalls at six distinct sites, ALL SIX UCRT (GetLastError, FlsGetValue, RtlAllocateHeap, FlsSetValue, HeapFree, SetLastError), 4 each, and 0 CALLOTHERs. This is decisive for graphics specifically because d3d11.dll is not in this builds IAT at all -- it is LoadLibrary-d by name (FUN_1402b80f0 at 0x1402b84d1) and called through stored pointers, so any such call targets an address outside 0x140000000..0x1440e8000 and would appear in the extcall log. NOT AN INDEPENDENT CONFIRMATION of SUPERGUN-NETCODE 12.1 -- it REPLACES that sections evidence: 12.1 correctly rejected the "252 IAT slots trapped, 0 hits" argument because that slice contains no d3d11/dxgi entries, and this instrument does not have that blind spot. SCOPE LIMIT (M4/M5): one frame of one state, idle-ish, 5 drawn nodes, no stage transition and no super. The GENERAL claim stays INFERRED. Test: re-run emu_gate.py frame on a super/hit-heavy frame and on a round-transition frame and re-check the extcall target set; a frame that loads a texture bank is the plausible counter-example.',
  status='confirmed', confidence='high', date='2026-09-05', contribution_candidate=true;
UPSERT finding:shipped_present_path_is_blt_model SET
  statement='THE SHIPPED STEAM MvC2 IS ARCHITECTURALLY INCAPABLE OF THE LOW-LATENCY PRESENT PATH. Read from the binary: the swapchain is created through IDXGIFactory::CreateSwapChain, the LEGACY pre-1.2 interface (FUN_1402b80f0, CALL [RAX+0x50] at 0x1402b87b1, assert string 0x14095d3b0 immediately after); MakeWindowAssociation at vtable +0x40 (0x1402b87ed); BufferCount = 1 (ResizeBuffers(1,...) strings 0x14095d8e0/0x14095d970 in FUN_1402b9930); SwapEffect = DISCARD, i.e. the BitBlt model, confirmed behaviourally by the live capture hook which depends on it (d3dcap/dllmain.cpp:1399: "Grabs the finished frame BEFORE Present, since SwapEffect=DISCARD leaves the backbuffer undefined afterwards"); and the game QueryInterfaces IDXGIDevice, not IDXGIDevice1 (strings 0x14095d2c0, 0x1409699e0). IDXGIFactory::CreateSwapChain cannot produce a flip-model swapchain and cannot set DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT -- both require IDXGIFactory2::CreateSwapChainForHwnd -- so no patch or config file can give the shipped binary FLIP_DISCARD or a waitable swapchain. INFERRED corollary: because SetMaximumFrameLatency lives on IDXGIDevice1, it is never called and DXGIs default queue depth of 3 frames applies; a negative cannot be proven from a static disassembly, so the test is to attach d3dcap, QueryInterface the device for IDXGIDevice1 and call GetMaximumFrameLatency. Closes the first half of SUPERGUN-NETCODE U4.',
  status='confirmed', confidence='high', date='2026-09-05', contribution_candidate=true;
UPSERT finding:supergun_present_path_built_but_not_wired SET
  statement='SUPERGUNs low-latency present path is BUILT AND NOT CALLED, and this is the highest millisecond-per-unit-of-effort item on the board. sg_runner/src/gfx.rs creates the swapchain through CreateSwapChainForHwnd (:214) with SwapEffect FLIP_DISCARD (:208), BufferCount 2 (:206) and DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT (:210), calls SetMaximumFrameLatency(1) (:227), retains the waitable object (:228) behind wait_for_frame() (:353), presents with a parameterised sync interval (:431) and already reads DXGI_FRAME_STATISTICS SyncQPCTime and PresentCount (:433-437). NEITHER wait_for_frame NOR present appears anywhere in play.rs or main.rs, and play.rs still paces with Sleep(s) at :295. WARNING for whoever wires it: Sleep must be deleted in the SAME change, because a loop that both waits on the swapchain and sleeps to a 16.667 ms budget is double-paced -- the sleep can put the pad sample back where it was and the waitable object becomes decorative, and that would be completely invisible in an fps counter. This is the projects recurring failure mode (a correct mechanism that never reaches the live path) in its most literal form.',
  status='confirmed', confidence='high', date='2026-09-05', contribution_candidate=true;
UPSERT finding:supergun_latency_floor_is_device_and_display SET
  statement='AFTER the 66.7 ms of GGPO frame delay is deleted, SUPERGUNs SOFTWARE IS NOT THE BOTTLENECK AND CANNOT BECOME ONE. Known software cost today, on the dev machine: pacing 0.58 ms (Sleep overshoot) + tick 0.053 ms p50 + tape build 0.221 ms + emitter ~0.32 ms amortised = about 1.2 ms, plus two unknowns (pad_read cost, the D3D11 submit+present path). After wiring the waitable swapchain and moving the tape build off the frame thread, the KNOWN software cost falls to about 0.06 ms -- the tick alone, 0.32 percent of a 16.667 ms frame at p50 and 0.82 percent at max. What remains is NOT ours: the device report interval (4-8 ms, so 2-4 ms of average sampling wait), scan-out (this machine measured at 1920x1080 @ 239 Hz = 4.184 ms refresh period, about 2.1 ms mean for a mid-screen flip) and panel pixel response. That is roughly 4-7 ms and it is the whole floor. RANKED LEVERS by ms per unit of effort: L1 wire the built waitable swapchain and delete Sleep (up to 8.4 ms of queued frames on this panel plus 0.58 ms of pacing, effort S); L2 buy a 1000 Hz stick (3.5 ms of average wait, 7 ms of worst case, effort = a purchase); L3 SyncInterval 0 + ALLOW_TEARING (about 2.1 ms mean on 239 Hz, 8.3 ms on 60 Hz, effort S, product decision); L4 move the tape build off the frame thread (0.22-0.27 ms, effort S). EXPLICITLY DO NOT OPTIMISE THE TICK. TAG: the software terms are CONFIRMED measurements; the claim that the floor is the device and the display is INFERRED and is what the present-path gate plus the report-interval histogram exist to confirm or kill.',
  status='inferred', confidence='medium', date='2026-09-05', contribution_candidate=true;
UPSERT finding:supergun_present_path_gate_specified SET
  statement='THE PRESENT-PATH GATE -- the deterministic gate that owns the entire back half of the SUPERGUN latency budget, and until it runs NOTHING about the native windows latency may be quoted as measured. Frozen scene (the stage-9 anchor with a fixed input tape so the geometry is identical frame to frame), N >= 600 frames, ONE variable per arm, recording per frame: QPC at wait_for_frame return, at pad_read return, at tick return and at Present return; plus SyncQPCTime, PresentCount and PresentRefreshCount from GetFrameStatistics. Report p50/p99/max of (SyncQPCTime - QPC(pad_read return)) -- the input-to-scan-out number, the only one that matters -- plus the count of frames where PresentCount advanced by more than 1 (dropped) or PresentRefreshCount advanced by other than the expected multiple (repeated). M1: GetFrameStatistics returns DXGI_ERROR_FRAME_STATISTICS_DISJOINT after a mode change and is unreliable for BitBlt and composed presents, so the harness MUST report the HRESULT and a disjoint count next to the number or the number is unquotable -- gfx.rs:435 currently checks >= 0 and silently keeps the previous value on failure, which is right for a render loop and dangerous for a measurement. M4: run SetMaximumFrameLatency(3) as a CONTROL ARM FIRST; if the 1-vs-3 arms produce the same number the instrument is not seeing the queue and nothing else it reports can be trusted. M6: SyncQPCTime advances on its own at the refresh cadence whether or not anything was presented -- establish what it does across an unchanged frame before treating a delta as a response. Also make the REPORTING platform-independent (QPC / clock_gettime deltas around named phases, display-timing statistic as optional enrichment), or the gate is a Windows artefact and the Bazzite port ships with no gate at all.',
  status='open', confidence='high', date='2026-09-05', contribution_candidate=true;
UPSERT finding:supergun_ctx_display_list_completeness_open SET
  statement='THE ENGINES OWN DISPLAY LIST IS IN ctx AND ITS COMPLETENESS IS OPEN -- this gates the lever that would retire the tape -> FrameRecord geometry re-derivation from the critical path. MEASURED (chain p-code trace, writes attributed by function body containment): a whole frame writes 145,120 B into ctx, of which FUN_1408436a0 (the TA emitter, pair loc_8c127c80) writes 15,990 B into ctx[0x30..0x1e0084) with its cursor at ctx+0x1e0080 bounded at 0xE0000; FUN_140800620/640 write 68,640 B of decoded texture pages into ctx[0x100030..0x110c50); the NaomiLib matrix routines write 57,696 B into a 212-byte window at ctx[0x1f80ec..0x1f81c0). By contrast *(game_state+0x208) receives only 3,000 B per frame, from FUN_1408456a0. THIS CONTRADICTS finding:ctx_out_blind_to_submitted_geometry, which states that the submitted geometry goes to *(game_state+0x208): (a) rr_runner.cpp:867 dumps the WHOLE ctx, so a 16 KB list at ctx+0x30 is inside the dump; (b) FRAME-READSET 3.5 identifies gs+0x208 as the TEXTURE staging buffer written by the per-fighter sprite-page upload. Three readings survive and they have different consequences. R1: the TA records hold REFERENCES into DC-RAM, so scrambled source vertices change pixels without changing ctx bytes -- test by decoding the 16 KB at ctx+0x30 as NaomiLib records and looking for pointers vs floats. R2: the scrambled objects were on a path not emitted through FUN_1408436a0 in that run -- test by re-running the scramble and diffing ctx[0x30..0x30000) specifically rather than the whole file. R3: the "goes to gs+0x208" clause is an inference that outran its evidence, in which case that findings HEADLINE (use the harvest, never ctx_out, to decide whether DC-RAM bytes are consumed) still stands but the mechanism clause needs correcting. Weak evidence against R1: FUN_1408436a0 contains 109 MOVSS/MOVUPS/MOVAPS instructions, a fingerprint consistent with writing transformed vertex floats -- a fingerprint, not a read. DO NOT COST THE DISPLAY-LIST LEVER UNTIL ONE OF R1/R2/R3 HAS RUN.',
  status='open', confidence='medium', date='2026-09-05', contribution_candidate=true;
UPSERT finding:supergun_pad_read_cost_unknown SET
  statement='pad_read() IS AN UNMEASURED TERM SITTING IN THE CRITICAL PATH, WITH A SPECIFIC REASON TO SUSPECT IT. sg_runner/src/play.rs:89-91 loops for i in 0..4 and calls XInputGetState(i, ...) UNCONDITIONALLY every frame, continuing on a non-zero return, then ORs in 10+ GetAsyncKeyState calls. That loop is CORRECT -- it is the fix for the A1 defect where a controller on user index 1 was invisible and every direction was silently dropped -- but on a machine with one pad it makes three calls per frame against DISCONNECTED indices, which is the one well-known slow path in that API, and it has never been timed here. This is an UNKNOWN, not a claim: it could be 20 microseconds and irrelevant. TEST (an instrument, not a design change): time each of the four XInputGetState calls SEPARATELY over 600 frames and report p50 and max PER INDEX, split connected vs disconnected, alongside the GetAsyncKeyState block. If the disconnected indices are expensive the fix is a cached connected-mask refreshed at a low rate -- but the refresh cadence is itself a correctness question, because A1 was a SILENT INPUT-LOSS bug, so any caching must keep --pad-probe honest.',
  status='open', confidence='high', date='2026-09-05', contribution_candidate=true;
UPSERT finding:supergun_speed_enables_and_does_not_enable SET
  statement='WHAT THE 0.32-PERCENT TICK STRUCTURALLY ENABLES, AND WHERE IT ENABLES NOTHING. ENABLES: (1) zero input delay with a very deep speculation window -- GGPOs MAX_PREDICTION_FRAMES = 8 exists because a typical engine costs 5-16 ms per frame, and the same wall-clock budget buys us about 250 frames, a different regime rather than a tuned constant; (2) re-simulating a whole match is free -- 3000 frames in 113 ms of CPU, so a three-minute match re-runs in about 0.4 s, which is what makes a server-side simulation receipt, a spectator feed and divergence detection cost essentially nothing; (3) the frame can BEGIN as late as the display allows, which is the one place the ticks smallness converts directly into input-to-photon latency rather than into headroom, and which only exists for an engine whose frame is short (INFERRED until the present-path gate runs); (4) rollback with no render-suppression hack and therefore none of that bug class -- the shipped game has G+0x770 precisely because it cannot afford to re-run render; (5) a 211,736-byte savestate costs 4 microseconds. ENABLES NOTHING for: (a) the device report interval (a USB descriptor); (b) scan-out and panel response (physics); (c) the network one-way delay -- deep speculation converts a FREEZE into a MISPREDICTION, a better failure but still a failure; (d) "our transport beats Steams", which is UNPROVEN and stays so until Gate N4, and must never be quoted as settled by the ticks smallness; (e) ANY claim of an engine-latency advantage over the shipped game, because finding:mvc2_zero_internal_input_latency proves there is no engine-side frame to reclaim and a competitor running the same recompiled binary starts from the same zero; (f) a contended desktop, where a preemption outside the frame function is unprotectable by any speculation cap (U-N11). THE ACTUAL MOAT is not the speed: it is that we hand-map the PE and drive FUN_140118950 ourselves, so we own the frame boundary -- when the pad is read, when the flip happens -- and the shipped game can change none of that (see finding:shipped_present_path_is_blt_model).',
  status='confirmed', confidence='medium', date='2026-09-05', contribution_candidate=true;

-- ---- sources ----------------------------------------------------------------
UPSERT source:latency_budget_gate_20260905 SET kind='measurement',
  ref='rr_runner.exe on d3dcap/ttd/runs/receipt-20260903-stage9-anchor/pre, 3 arms x 6 ticks --dump-every 1 (zero / RIGHT 0x20 / LEFT 0x80) + a determinism re-run of the zero arm (6/6 byte-identical) + a 1000-tick timing run with RIGHT held (--heap-mb 256, 4,873,880 B allocated so no heap wrap): min 0.033 p50 0.053 p90 0.075 p99 0.108 max 0.137 ms',
  strength='reproduction';
UPSERT source:frame_trace_20260903_000941 SET kind='measurement',
  ref='%TEMP%\\rrcap_emu\\frame_20260903-000941_{tick,chain}_r0.trace.bin -- whole-frame p-code traces, dtype kind:u1 pc:u8 addr:u8 size:u4, 313,754 records; blk base 0x146c1000, ctx 0x0f7c1000, dcram 0x0fbc1000; function attribution by body containment against re_map/steam_funcs.jsonl',
  strength='reproduction';
UPSERT source:ghidra_offline_export_20260905 SET kind='ghidra',
  ref='d3dcap/replay/re_map/cache/steam_disasm.jsonl + steam_funcs.jsonl -- the OFFLINE export of all 29,678 functions of mvc_dump.bin. The :8080 GhidraMCP bridge was NOT running this session and was not needed. Blind spot: indirect calls (FUN_140611a20 has no caller in the static export because it is reached through a function-pointer table), which is why the ordering claims are anchored on the p-code trace.',
  strength='code';
UPSERT source:supergun_gfx_play_sources_20260905 SET kind='code',
  ref='supergun/supersoaker/runner/sg_runner/src/gfx.rs :206 :208 :210 :214 :227 :228 :353 :431 :433-437; src/play.rs :86-124 :244 :247 :252 :295; src/win.rs :400 :432 :434 :599 :604. READ ONLY -- nothing in supergun/ was modified. NOTE: WORKSTREAM 4 P7b cites play.rs:217/:221 for the pad/tick pair; the current file has them at :244/:247 (the line numbers moved with the Rust port; the claim is unchanged).',
  strength='code';
UPSERT source:display_refresh_20260905 SET kind='measurement',
  ref='Win32_VideoController on the dev machine 2026-09-05: NVIDIA RTX 3090, 1920x1080, CurrentRefreshRate 239 (refresh period 4.184 ms). Machine-specific and load-bearing for every scan-out term.',
  strength='measurement';
UPSERT source:supergun_latency_budget_doc SET kind='doc',
  ref='C:\\Users\\trist\\projects\\supergun\\docs\\SUPERGUN-LATENCY-BUDGET.md';

-- ---- edges ------------------------------------------------------------------
RELATE steam_routine:FUN_14060b7d0->recompiles->routine:loc_8c010080;
RELATE steam_routine:FUN_14060b7d0->calls->steam_routine:FUN_140048630;
RELATE steam_routine:FUN_14060b7d0->writes->field:steam_input_dec;
RELATE steam_routine:FUN_140634920->reads->field:steam_input_dec;
RELATE steam_routine:FUN_140634920->writes->field:fighter_input_block;
RELATE steam_routine:FUN_140611a20->calls->steam_routine:FUN_140634920;
RELATE steam_routine:FUN_1408436a0->writes->global:ctx_ta_cursor;

RELATE finding:mvc2_zero_internal_input_latency->about->steam_routine:FUN_14060b7d0;
RELATE finding:mvc2_zero_internal_input_latency->about->steam_routine:FUN_140048630;
RELATE finding:mvc2_zero_internal_input_latency->about->steam_routine:FUN_140634920;
RELATE finding:mvc2_zero_internal_input_latency->about->field:steam_input_dec;
RELATE finding:mvc2_zero_internal_input_latency->cites->source:latency_budget_gate_20260905;
RELATE finding:mvc2_zero_internal_input_latency->cites->source:frame_trace_20260903_000941;
RELATE finding:mvc2_zero_internal_input_latency->cites->source:ghidra_offline_export_20260905;
RELATE finding:mvc2_zero_internal_input_latency->cites->source:supergun_latency_budget_doc;

RELATE finding:gate0_input_shift_is_tape_indexing->about->steam_routine:FUN_140634920;
RELATE finding:gate0_input_shift_is_tape_indexing->cites->source:latency_budget_gate_20260905;
RELATE finding:gate0_input_shift_is_tape_indexing->cites->source:supergun_latency_budget_doc;

RELATE finding:no_graphics_call_inside_the_tick->cites->source:frame_trace_20260903_000941;
RELATE finding:no_graphics_call_inside_the_tick->cites->source:supergun_latency_budget_doc;

RELATE finding:shipped_present_path_is_blt_model->about->steam_routine:FUN_1402b80f0;
RELATE finding:shipped_present_path_is_blt_model->cites->source:ghidra_offline_export_20260905;
RELATE finding:shipped_present_path_is_blt_model->cites->source:supergun_latency_budget_doc;

RELATE finding:supergun_present_path_built_but_not_wired->cites->source:supergun_gfx_play_sources_20260905;
RELATE finding:supergun_present_path_built_but_not_wired->cites->source:supergun_latency_budget_doc;

RELATE finding:supergun_latency_floor_is_device_and_display->cites->source:latency_budget_gate_20260905;
RELATE finding:supergun_latency_floor_is_device_and_display->cites->source:display_refresh_20260905;
RELATE finding:supergun_latency_floor_is_device_and_display->cites->source:supergun_latency_budget_doc;

RELATE finding:supergun_present_path_gate_specified->cites->source:supergun_gfx_play_sources_20260905;
RELATE finding:supergun_present_path_gate_specified->cites->source:supergun_latency_budget_doc;

RELATE finding:supergun_ctx_display_list_completeness_open->about->steam_routine:FUN_1408436a0;
RELATE finding:supergun_ctx_display_list_completeness_open->about->global:ctx_ta_cursor;
RELATE finding:supergun_ctx_display_list_completeness_open->cites->source:frame_trace_20260903_000941;
RELATE finding:supergun_ctx_display_list_completeness_open->cites->source:supergun_latency_budget_doc;

RELATE finding:supergun_pad_read_cost_unknown->cites->source:supergun_gfx_play_sources_20260905;
RELATE finding:supergun_pad_read_cost_unknown->cites->source:supergun_latency_budget_doc;

RELATE finding:supergun_speed_enables_and_does_not_enable->cites->source:latency_budget_gate_20260905;
RELATE finding:supergun_speed_enables_and_does_not_enable->cites->source:supergun_latency_budget_doc;
-- ============================================================================
