/* ============================================================================
   boot.mjs — the ONLY entrypoint for play.nobd.net.
   Orchestrates the bootstrap in the proven order; instantiates the subsystems
   and wires their callbacks. This is the only file that knows about all of them.

   Reuse-first architecture (Phase 1):
     RenderCore   -> facade over ../../js/renderer-bridge-webgpu.mjs  (the live game)
     Surround     -> reused SurrealDB init* fns (leaderboard/chat/queue/ticker/…)
     SeatController-> FCFS no-login join/leave over ../../js/ws-connection.mjs
     InputNet     -> gamepad.mjs + ws-connection.mjs + optional QUIC
     KingMarquee  -> new (king marquee is dead code in king.html)

   Latency is inherited from the reused modules — see docs: direct /play control
   WS for input, control_only handshake, ~1ms burst-poll, phase-aligned send,
   two-socket split, RAF latest-wins, lazy seat.
   ============================================================================ */
import { startFallbackArena } from './fallback-arena.mjs';

const $ = (id) => document.getElementById(id);

async function boot() {
  // 0. Decorative arena runs immediately so the cabinet is never a black box
  //    while the real render warms up. RenderCore.stop-hides it on first frame.
  const arena = startFallbackArena($('fallbackArena'));

  // ---- 1. RenderCore: the live game into #game-canvas ---------------------
  // TODO(next): const core = await RenderCore.attach($('game-canvas'), { resScale: 1 });
  //   -> ../../js/renderer-bridge-webgpu.mjs initRenderer(); on first real frame:
  //      arena.stop(); $('idleScreen').style.display = 'none';
  //   Highest-risk reuse — validate live /ws relay video renders before anything else.

  // ---- 2. Surround: live arcade-line data (anonymous, read-only) ----------
  // TODO(next): initLeaderboard / initChat(read) / initQueueLive / initSlotLive
  //   / startTicker / initLiveMatch / initPlayerCard  — reused init* fns,
  //   king.html bootstrap order, painting the fixed ids already in the DOM.

  // ---- 3. WatchCount: relay status.spectators (not SurrealDB) -------------
  // TODO(next): tap the render feed's status WS -> $('watchCount').

  // ---- 4. KingMarquee: build fresh (dead code) ---------------------------
  // TODO(next): derive from top best_streak or live_match winner -> #kingName/#kingStreak.

  // ---- 5. SeatController: FCFS no-login "I GOT NEXT" ----------------------
  // TODO(next): $('gotNextBtn')/$('gotNextBtn2') -> seat.join(); render {assigned}.

  // ---- 6. InputNet: gamepad + soft keyboard/on-screen --------------------
  // TODO(next): start polling on seat grant, stop on leave/kick; phase pump.

  // keep a handle for teardown/HMR
  window.__play = { arena };
}

if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', boot, { once: true });
} else {
  boot();
}
