-- 166_instrument_traps.surql -- the tooling failures that produced wrong ANSWERS on 2026-09-05.
-- Doc: supergun/docs/INSTRUMENT-TRAPS.md   Read before quoting any measurement on this project.
USE NS re DB kb;

UPSERT source:doc_instrument_traps SET kind='tool', strength='code',
  ref='supergun/docs/INSTRUMENT-TRAPS.md',
  note='Nine traps, each found because a result was implausible and somebody checked the TOOL instead of believing the number.';

UPSERT finding:trap_cargo_no_rebuild SET status='confirmed', confidence='high', date='2026-09-05',
  title='cargo silently rebuilds NOTHING on this machine: the filesystem and shell clocks differ by ~3 hours',
  statement='Measured: date returned 07:11 while a freshly written file mtime read 10:08. cargo build finished in 0.06 s reporting success and produced a byte-identical binary, so a patched source appeared to have no effect. cargo clean -p also removed 0 files. One agent came within a sentence of reporting "your fix does nothing" as a result.',
  guard='Capture md5 of the artefact before and after every build and assert it changed, or build into a fresh CARGO_TARGET_DIR. Never conclude a change had no effect from a run whose rebuild was not verified.';

UPSERT finding:trap_runner_gate_stale_binary SET status='confirmed', confidence='high', date='2026-09-05',
  title='runner_gate.py runs a DIFFERENT binary from the one under test',
  statement='d3dcap/receipt/runner/runner_gate.py:31 hardcodes RUNNER = os.path.join(HERE, rr_runner.exe) -- a checked-in binary in the harness own directory, NOT supergun/supersoaker/runner/sg_runner/target/release/sg_runner.exe. On 2026-09-05 that file was 292 KB dated the previous day and did not contain the --arc flag being tested; running Gate 1 as documented would have silently tested yesterday runner and reported PASS. build_runner() only builds when the file is ABSENT, so a stale one is never refreshed.',
  guard='Override the module global from a wrapper (sys.path.insert, import runner_gate as G, G.RUNNER = ..., G.main()) rather than overwriting another lane binary -- it is a shared checkout. Run the wrapper from a directory that is NOT the shared scratchpad: a stray dis.py there shadows the stdlib and numpy fails to import.';

UPSERT finding:trap_self_referential_gate SET status='confirmed', confidence='high', date='2026-09-05',
  title='A gate that compares us to ourselves cannot catch an emitter fault',
  statement='The Stage B raster gate compares the native D3D11 renderer against our own headless-Chrome WebGPU renderer ON THE SAME TAPE. Both consume the same emitter output, so when the emitter is wrong both draw the same wrong quad and the diff reads 0.0000 percent. It passed green through severe visible corruption that a human spotted by eye -- stretched HUD name plates and flat coloured bands across the screen.',
  guard='For any fidelity claim the reference must be MvC2 ITSELF (a d3dcap capture of the real game). Pixel-perfect measured against our own second renderer is a statement about our consistency, not our fidelity. Always report PRECISION AND COVERAGE TOGETHER: a broken arm that draws extra geometry scores HIGHER on set-coverage alone -- fixing the aobjs bug correctly moved coverage 87.96 percent DOWN to 74.62 percent.';

UPSERT finding:trap_kb_seed_failures SET status='confirmed', confidence='high', date='2026-09-05',
  title='Two re_kb failure modes that look like the graph is broken',
  statement='(1) Backslashes in SurrealQL string literals cause Parse error: Invalid escape, and apply_seed.py may report the statement as simply failing -- use forward slashes in paths. (2) status=confirmed is REFUSED unless the finding cites a source of strength reproduction/code or a routine; this is the schema WORKING (it caught three findings written with no citation at all). Add the citation, do not relabel a source to slip past it. (3) apply_seed.py has a TWO-PASS mechanism for deferred status writes, so a first-pass "N failed" on a fresh seed is often resolved by pass 2 -- verify by querying the records, not by reading the apply summary (rule M2).';

RELATE finding:trap_cargo_no_rebuild -> cites -> source:doc_instrument_traps SET id='e_trap_cargo';
RELATE finding:trap_runner_gate_stale_binary -> cites -> source:doc_instrument_traps SET id='e_trap_gate';
RELATE finding:trap_self_referential_gate -> cites -> source:doc_instrument_traps SET id='e_trap_selfref';
RELATE finding:trap_kb_seed_failures -> cites -> source:doc_instrument_traps SET id='e_trap_kb';
