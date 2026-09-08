-- 145_rust_runner_port.surql -- the runner ported to Rust, gated on IDENTICAL STATE.
-- SUPERGUN ENGINE lane, 2026-09-04 (odd seeds). Doc: supergun/docs/RECEIPT-RUNNER-RUST-PORT.md
-- Idempotent and replayable; explicit edge ids; additive fields only on findings owned by earlier seeds.
USE NS re DB kb;

UPSERT source:tool_sg_runner_rust SET kind='tool', strength='code',
  ref='supergun/supersoaker/runner/sg_runner (Cargo bin sg_runner), a port of supersoaker/runner/rr_runner.cpp',
  note='No dependencies: win.rs hand-declares the ~20 Win32 calls needed. The unsafe surface is concentrated and documented -- fixed-address arena and image reservation, a PE image mapped and protected by hand from the packer section table, IAT patching with 32-byte trap thunks (mov ecx,id; mov rax,rr_trap; jmp rax), the UCRT pointer-cache rotate/xor against the live __security_cookie, and the call into game code. Everything else is safe Rust. panic=abort so a Rust panic can never unwind through a game frame. Semantics carried over verbatim because the gates depend on them: the bump heap that never frees and re-zeroes on wrap, and Fls* returning 0/FALSE so the CRT frees its fresh ptd.';

UPSERT finding:rust_runner_port_identical_state SET status='confirmed', confidence='high', date='2026-09-04',
  contribution_candidate=true,
  title='The Rust runner reproduces the C++ runner byte-for-byte on Gates 1, 2 and 3',
  statement='Gate 1: 300 of 300 blk dumps byte-identical to the CONFIRMED C++ dumps, which are themselves byte-exact against the p-code harness; gs_out, exe_dat_out, exe_str_out and ggpo_out all EQUAL. Gate 3: 300 of 300 blk dumps byte-identical with DC-RAM rebuilt from the arc and no memory dump. Gate 2: runner_tape_gate PASS with P 14, A 1281, F 1 and ZERO unexplained differences -- the same class counts and the same first-difference hashes as the C++ run. Independent observables that also matched exactly: the RNG at every logged checkpoint (ac71, bf0b, 9b51, 2358, 95fd, eee7, 1002); the external call counts (RtlAllocateHeap 1610 for 1,558,480 B, and 1610 each of HeapFree, GetLastError, SetLastError, FlsGetValue and FlsSetValue, traps hit 0); exactly one lazily committed host page 0x2a2b0000 first touched by the SAME RIP 0x14004d460 on tick 1; and the harvest write set of 952 DC-RAM pages / 3,899,392 B over 300 ticks, the figure RECEIPT-RUNNER-GATE2 recorded for the C++.',
  cost='Tick p50 0.058 ms (min 0.025, p90 0.099, p99 0.122, max 0.127) against the C++ p50 0.072 on the same receipt. NOT a claim that Rust is faster: one run of each on a busy machine, both through LLVM. Quoted only so the port is not suspected of costing anything.';

UPSERT finding:rust_port_ctx_delta_is_the_known_family SET status='confirmed', confidence='high', date='2026-09-04',
  title='The 599-byte ctx_out delta between the Rust and C++ runners is the known run-dependent family, proven by a same-build control',
  statement='ctx_out differs from the C++ run by 599 bytes. Classified: 171 runs, every one 3 or 4 bytes, at a consecutive-run stride of 0x130, and 599 of 599 bytes fall in the page-table-records bucket (ctx+0x108FC0..0x1E0030) with ZERO in the texture-slot table, the matrix stack, the TA record areas or the decoded texture pages. That is the RECEIPT-RUNNER-GATE1 s3.5 family: ctx page-table records memcpy-d from a stack buffer whose +0x6C/+0x114 fields hold uninitialised leftovers, never read by the tick. CONTROL, because bucket membership alone is not proof: two runs of the SAME Rust binary on the SAME image differ by 258 bytes in ctx_out while blk_t300 and gs_out are byte-identical, so the family is run-dependent within one build and the C++-versus-Rust delta is that noise rather than a systematic port difference. The C++ was previously shown to differ from itself by 128 bytes the same way.';

UPSERT finding:rust_port_not_done SET status='open', confidence='high', date='2026-09-04',
  title='Rollback and play modes are not ported, so Gate 4 / N1c has not been re-run against the Rust build',
  statement='sg_runner covers the batch tick path, the harvest dump, the IAT and UCRT setup, the self-checks and the reporting. It does NOT yet implement --rollback / --rb-set / --rb-ctx-extra (so GATE 4 N1c arm C is unverified against the Rust build) or --play / --pad-probe (so a human still plays through the C++ binary). Until both land, rr_runner.cpp is not deletable and the two binaries must be kept in step.',
  next_step='Port the rollback ring and the three-arm N1c save sets, re-run gate_n1c.py on the four gated stages, then port play mode; only then remove the C++.';

UPSERT finding:playable_loop_chain_closes SET
  rust_port_note='2026-09-04: the batch tick path is now also available as a Rust binary (finding:rust_runner_port_identical_state), which is the prerequisite for handing frames to rr-render in-process instead of through a file.';

RELATE finding:rust_runner_port_identical_state->cites:p145_tool->source:tool_sg_runner_rust;
RELATE finding:rust_port_ctx_delta_is_the_known_family->cites:p145_ctx_tool->source:tool_sg_runner_rust;
RELATE finding:rust_port_not_done->cites:p145_open_tool->source:tool_sg_runner_rust;
