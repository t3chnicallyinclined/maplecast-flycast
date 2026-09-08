-- 157_browser_live_path_hang.surql -- the browser half of the live path connects and then never renders.
-- Two code-level causes found by reading; the deciding measurement is NOT yet taken, so the root cause
-- is recorded as OPEN and the entry says exactly which observation would close it.
-- SUPERGUN ENGINE lane, 2026-09-04 (odd seeds). Doc: supergun/docs/RECEIPT-RUNNER-PLAY.md
USE NS re DB kb;

UPSERT source:code_live_player_wire SET kind='code', strength='code',
  ref='mvc-live-skins-quarters/d3dcap/replay/live-player.mjs (_wire, connect) and supersoaker/runner/sg_view.py (cmd_bridge)',
  note='Read directly, both halves of the browser live path.';

UPSERT finding:live_browser_silent_hang SET status='open', confidence='medium', date='2026-09-04',
  title='A swallowed decode error in the live client can hang connect() forever with no page error',
  statement='Observed on the live path: the browser connects, the runner sends 3,449 B of session info, exactly ONE FrameRecord of 4,379,060 B leaves the runner, the emitter runs exactly once (frame() n=1), 295 later chunks are dropped, and the runner then sees the socket reset (os error 10054). In the page, window.__rr is never set and the guard panel stays empty, and NO pageerror is raised. Code-level causes found by reading. FIRST: live-player.mjs _wire wraps decodeFrameRecord in a try/catch that increments a counter and returns, so a decode failure on record 0 means _resolveFirst is never called and the await in connect() never settles -- a permanent, silent hang that matches the symptom exactly, including the absence of a page error. The tape path does not have this shape: its _onMessage REJECTS the pending promise on a decode failure, so the same fault would surface there. SECOND: the runner observing 10054 means its peer, the BRIDGE process, went away -- sg_view.py cmd_bridge breaks its relay loop on any OSError from sendall and returns, and Python then closes the upstream socket with unread data queued, which Windows turns into an RST. So the 10054 is evidence about the bridge, NOT directly about the browser.',
  ruled_out='Three hypotheses are excluded by reading. The bridge does NOT truncate large frames: it uses sendall, not send, so a 4.4 MB payload is written in full. The RFC6455 length encoding covers all three forms including the 64-bit form. The session info is sent as opcode 1 TEXT, which is what the client requires since it tests typeof e.data === string, and frames as opcode 2 binary with binaryType arraybuffer set before connect.',
  open_items='UNSETTLED: whether _wire is ever entered for record 0. The deciding observation is one log line on entry to _wire plus one in the bridge per relayed frame. If _wire never sees the 4.4 MB record the fault is in transport or in the handler swap; if it sees it and throws, the fault is the swallowed catch and the fix is to surface the error the way the tape path does. Note the handler-swap race is UNLIKELY on inspection: the continuation after the info await runs as a microtask on the same message dispatch, so onmessage is replaced before the next message task can run.',
  priority='background -- superseded 2026-09-04 by the native window, which deletes this path for PLAYING. The browser remains the WATCHING product, so this stays open rather than closed.';

RELATE finding:live_browser_silent_hang->cites:p157_code->source:code_live_player_wire;
RELATE finding:live_browser_silent_hang->cites:p157_stream->source:tool_sg_live_stream;
