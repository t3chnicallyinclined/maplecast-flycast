// ============================================================================
// DIAGNOSTICS.MJS — Tabbed diagnostics + latency settings modal (Phase B redesign)
// ============================================================================
//
// The old single-overlay was 9px monospace text in a 220px column overlapping
// the game canvas, with pointer-events: none — unreadable AND uninteractive.
// This rewrite renders into a real modal in king.html (.diag-modal) with:
//
//   - Closeable header (X button + Esc keybind)
//   - Three tabs: STATS, LATENCY, INPUT
//     * STATS    — what the old overlay showed: server fps, conn, render, P1/P2 stats
//     * LATENCY  — per-slot policy buttons, guard window display, frame phase,
//                  full latch_stats with histograms (the part you couldn't read before)
//     * INPUT    — gamepad state preview, future input mapping settings
//   - Persistent: selected tab is stored in localStorage so reopening goes
//     back to your last view. Modal open/closed state intentionally NOT
//     persisted (defaults closed on page load to avoid surprising people).
//
// Future Phase C: when SurrealDB profile sync ships, the localStorage stub
// here gets a tiny migration that uploads `_diagSettings` once on sign-in and
// pulls it down on subsequent loads. The shape of `_diagSettings` is what
// matters; treat it as the eventual profile schema.
//
// Public API (unchanged from the old file so king.html imports keep working):
//   toggleDiag()        — open/close the modal
//   updateDiag()        — re-render whichever tab is currently visible
//   startDiagInterval() — kick off the 1Hz refresh + ping (called from main)

import { state } from './state.mjs';
// Import telemetry from whichever renderer bridge is active
// WebGPU: renderer-bridge-webgpu.mjs, WASM: renderer-bridge.mjs
import { _telemetry } from './renderer-bridge-webgpu.mjs';

// ----------------------------------------------------------------------------
// Persistent settings (localStorage)
// ----------------------------------------------------------------------------
// Keep this object small + JSON-serializable. Future Phase C will sync it to
// the user's SurrealDB profile, so any new fields should be safe to round-trip.
const STORAGE_KEY = 'maplecast_diag_settings';
const _defaultSettings = {
  selectedTab: 'stats',     // 'stats' | 'latency' | 'input'
  // Latency tab — these are the player's PREFERENCES for the latch path.
  // The actual policy on the server is per-SLOT (today) — when stick-memory
  // ships these become the source of truth that gets pushed to flycast on
  // (re)connect.
  preferredLatchPolicy: 'latency',  // 'latency' | 'consistency'
};
let _diagSettings = { ..._defaultSettings };

function loadSettings() {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (raw) {
      const parsed = JSON.parse(raw);
      _diagSettings = { ..._defaultSettings, ...parsed };
    }
  } catch (e) {
    console.warn('[diag] failed to load settings:', e);
  }
}
function saveSettings() {
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(_diagSettings));
  } catch (e) {
    console.warn('[diag] failed to save settings:', e);
  }
}
loadSettings();

// Per-user latch policy: read the user's stored preference. Used by the
// queue.mjs / ws-connection.mjs join handshake to send the user's policy
// to flycast on every (re)assignment, so the slot inherits the player's
// choice instead of whatever the previous occupant left it at.
//
// Returns 'latency' or 'consistency' (the strings flycast's WS handler
// understands). Always defined — falls back to the default 'latency'.
export function getPreferredLatchPolicy() {
  return _diagSettings.preferredLatchPolicy || 'latency';
}

// ----------------------------------------------------------------------------
// Modal open / close / tab switching
// ----------------------------------------------------------------------------
function $(id) { return document.getElementById(id); }

export function toggleDiag() {
  const el = $('diagOverlay');
  if (!el) return;
  const wasOpen = el.classList.toggle('open');
  if (wasOpen) {
    // Re-render immediately so the user doesn't wait for the next 1Hz tick
    selectTab(_diagSettings.selectedTab);
    updateDiag();
  }
}

function selectTab(name) {
  const el = $('diagOverlay');
  if (!el) return;
  // Mark the matching tab + panel active, all others inactive
  el.querySelectorAll('.diag-tab').forEach(btn => {
    btn.classList.toggle('active', btn.dataset.tab === name);
  });
  el.querySelectorAll('.diag-tab-panel').forEach(p => {
    p.classList.toggle('active', p.dataset.panel === name);
  });
  if (_diagSettings.selectedTab !== name) {
    _diagSettings.selectedTab = name;
    saveSettings();
  }
}

// Public alias used by the settings.mjs compatibility shim — when something
// calls toggleSettings() (the old API), the shim opens the modal and force-
// switches to the GRAPHICS tab via this exported function.
export function selectDiagTab(name) {
  selectTab(name);
  updateDiag();
}

// Wire up tab clicks ONCE on first load. We attach to the document because
// king.html is parsed before this module runs in some hot paths.
function wireTabsOnce() {
  if (wireTabsOnce._wired) return;
  const el = $('diagOverlay');
  if (!el) return;
  el.querySelectorAll('.diag-tab').forEach(btn => {
    btn.addEventListener('click', () => {
      selectTab(btn.dataset.tab);
      updateDiag();
    });
  });
  wireTabsOnce._wired = true;
}

// ----------------------------------------------------------------------------
// Per-tab renderers
// ----------------------------------------------------------------------------
// Client-render counters reset between samples (1s window each tick)
let _diagLastFrames = 0;
let _diagLastDropped = 0;
let _diagLastSampleAt = 0;

// ----------------------------------------------------------------------------
// GRAPHICS tab — eagerly populated at module load
// ----------------------------------------------------------------------------
// The form controls (#opt_resolution, #css_bright, etc.) MUST exist in the
// DOM before settings.mjs loadRenderSettings() / applyLoadedSettings() runs,
// because those functions read element values via direct getElementById.
// So we render this panel ONCE at module init (DOMContentLoaded if needed)
// and never touch it again. State updates flow through the existing
// settings.mjs onchange handlers; this is a static skeleton.

const GRAPHICS_HTML = `
<div class="diag-section">QUALITY</div>
<label><span class="label-text">Resolution</span>
  <select id="opt_resolution" onchange="setOpt(0, this.value)">
    <option value="480" selected>480p (Native)</option>
    <option value="720">720p</option>
    <option value="960">960p</option>
    <option value="1440">1440p</option>
    <option value="1920">1080p (4x)</option>
  </select>
</label>
<label><span class="label-text">Aniso Filter</span>
  <select id="opt_aniso" onchange="setOpt(5, this.value)">
    <option value="1" selected>Off</option>
    <option value="2">2x</option>
    <option value="4">4x</option>
    <option value="8">8x</option>
    <option value="16">16x</option>
  </select>
</label>
<label><span class="label-text">Tex Filter</span>
  <select id="opt_texfilter" onchange="setOpt(6, this.value)">
    <option value="0" selected>Default</option>
    <option value="1">Nearest</option>
    <option value="2">Linear</option>
  </select>
</label>
<label><span class="label-text">Transparency</span>
  <select id="opt_layers" onchange="setOpt(7, this.value)">
    <option value="4" selected>4 (Fast)</option>
    <option value="8">8</option>
    <option value="16">16</option>
    <option value="32">32 (Best)</option>
  </select>
</label>

<div class="diag-section">FEATURES</div>
<label><input type="checkbox" id="opt_fog" onchange="setOpt(2, this.checked?1:0)"> <span class="label-text">Fog</span></label>
<label><input type="checkbox" id="opt_modvol" onchange="setOpt(3, this.checked?1:0)"> <span class="label-text">Modifier Volumes</span></label>
<label><input type="checkbox" id="opt_pstrip" onchange="setOpt(4, this.checked?1:0)"> <span class="label-text">Per-Strip Sort</span></label>

<div class="diag-section">POST-PROCESSING</div>
<label><input type="checkbox" id="css_scanlines" onchange="applyCSSEffects()"> <span class="label-text">CRT Scanlines</span></label>
<label><input type="checkbox" id="css_bloom" onchange="applyCSSEffects()"> <span class="label-text">Bloom Glow</span></label>
<label><input type="checkbox" id="css_sharpen" onchange="applyCSSEffects()"> <span class="label-text">Sharpen</span></label>
<label><input type="checkbox" id="css_smooth" onchange="applyCSSEffects()"> <span class="label-text">Smooth</span></label>
<label><input type="checkbox" id="css_vignette" onchange="applyCSSEffects()"> <span class="label-text">Vignette</span></label>
<label><input type="checkbox" id="css_curvature" onchange="applyCSSEffects()"> <span class="label-text">CRT Curve</span></label>
<label><span class="label-text">Bright</span>
  <input type="range" id="css_bright" min="80" max="150" value="100" oninput="applyCSSEffects()">
  <span class="slider-val" id="css_bright_val">100%</span>
</label>
<label><span class="label-text">Contrast</span>
  <input type="range" id="css_contrast" min="80" max="200" value="100" oninput="applyCSSEffects()">
  <span class="slider-val" id="css_contrast_val">100%</span>
</label>
<label><span class="label-text">Saturate</span>
  <input type="range" id="css_sat" min="50" max="200" value="100" oninput="applyCSSEffects()">
  <span class="slider-val" id="css_sat_val">100%</span>
</label>

<div class="diag-section">PRESETS</div>
<div class="policy-row">
  <button class="policy-btn" onclick="presetMaxPerformance()">MAX PERF</button>
  <button class="policy-btn" onclick="presetArcade()">ARCADE</button>
  <button class="policy-btn" onclick="presetMaxQuality()">MAX QUAL</button>
</div>
`;

function ensureGraphicsRendered() {
  const panel = document.querySelector('.diag-tab-panel[data-panel="graphics"]');
  if (!panel || panel.dataset.rendered === '1') return;
  // .diag-tab-panel uses white-space: pre by default for the monospace tabs;
  // override for the form tab so labels and selects flow normally.
  panel.style.whiteSpace = 'normal';
  panel.innerHTML = GRAPHICS_HTML;
  panel.dataset.rendered = '1';
}
// Run once as soon as the DOM is ready, regardless of whether the modal is open.
// settings.mjs's loadRenderSettings() needs the form elements to exist before
// it fires on app boot.
if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', ensureGraphicsRendered);
} else {
  ensureGraphicsRendered();
}

function renderStatsTab() {
  const panel = document.querySelector('.diag-tab-panel[data-panel="stats"]');
  if (!panel) return;

  const d = state.diag;
  const p1 = d.p1 || {};
  const p2 = d.p2 || {};
  const publishMs = ((d.publishUs || 0) / 1000).toFixed(2);
  const streamMbps = ((d.streamKbps || 0) / 1000).toFixed(1);
  const pingMs = (d.pingMs || 0).toFixed(1);

  const now = performance.now();
  const winSec = _diagLastSampleAt > 0 ? (now - _diagLastSampleAt) / 1000 : 1;
  const renderedDelta = _telemetry.framesRendered - _diagLastFrames;
  const droppedDelta  = _telemetry.framesDropped  - _diagLastDropped;
  _diagLastFrames    = _telemetry.framesRendered;
  _diagLastDropped   = _telemetry.framesDropped;
  _diagLastSampleAt  = now;
  const renderFps = (renderedDelta / winSec).toFixed(1);

  const jitterAvgMs = _telemetry.intervalCount > 0
    ? (_telemetry.intervalSumUs / _telemetry.intervalCount / 1000).toFixed(2)
    : '0.00';
  const jitterMaxMs = (_telemetry.intervalMaxUs / 1000).toFixed(2);

  // Mixed-content layout: monospace data lines + plain-text help blurbs.
  // Each section has a one-line plain-English explainer right under the
  // header so non-technical players know what they're looking at.
  panel.style.whiteSpace = 'normal';
  panel.innerHTML = `
<div class="diag-section">SERVER</div>
<div class="diag-help">The flycast emulator on the VPS. fps should be 60. publish is how long it takes to capture and ship one frame to your browser.</div>
<pre class="diag-data">status:    ${state.connState || '...'}
fps:       ${d.fps}
frame:     ${d.serverFrame}
publish:   ${publishMs} ms/frame
stream:    ${streamMbps} Mbps
dirty pgs: ${d.dirtyPages}</pre>

<div class="diag-section">CONNECTION</div>
<div class="diag-help">Your network latency to the VPS. Lower is better. Ping &lt; 30 ms = same region; 50–100 ms = cross-country; &gt; 150 ms = international.</div>
<pre class="diag-data">ping:      ${pingMs} ms</pre>

<div class="diag-section">CLIENT RENDER</div>
<div class="diag-help">How fast your browser is decoding and drawing the stream. fps should match the server. drops or high jitter mean your CPU/GPU can't keep up — try lowering Resolution in GRAPHICS.</div>
<pre class="diag-data">fps:       ${renderFps}
drops:     ${droppedDelta}
jitter:    ${jitterAvgMs} ms avg / ${jitterMaxMs} ms max</pre>

<div class="diag-section">P1 ${(p1.pps||0) > 1000 ? '(NOBD STICK)' : '(Web Gamepad)'}</div>
<div class="diag-help">Player 1's input rate. NOBD sticks send ~12,000 packets/sec; web gamepads send ~60–250/sec. chg = button-state changes per second.</div>
<pre class="diag-data">name:      ${p1.name || '-'}
input:     ${p1.pps||0}/s  chg ${p1.cps||0}/s</pre>

<div class="diag-section">P2 ${(p2.pps||0) > 1000 ? '(NOBD STICK)' : '(Web Gamepad)'}</div>
<pre class="diag-data">name:      ${p2.name || '-'}
input:     ${p2.pps||0}/s  chg ${p2.cps||0}/s</pre>

<div class="diag-section">YOU</div>
<pre class="diag-data">slot:      ${state.mySlot >= 0 ? 'P'+(state.mySlot+1) : (state.wsInQueue ? 'QUEUED' : 'SPECTATOR')}
pad:       ${d.gamepadActive ? 'YES' : 'no'}
sent:      ${d.inputSendCount}</pre>
`;
}

function fmtLatchLine(label, ls, policy) {
  if (!ls) return `${label}: -`;
  const total = ls.total_latches | 0;
  const data = ls.latches_with_data | 0;
  const freshPct = total > 0 ? ((100 * data) / total).toFixed(1) : '0.0';
  const avg = ((ls.avg_delta_us || 0) / 1000).toFixed(2);
  const p99 = ((ls.p99_delta_us || 0) / 1000).toFixed(2);
  const min = ((ls.min_delta_us || 0) / 1000).toFixed(2);
  const max = ((ls.max_delta_us || 0) / 1000).toFixed(2);
  const seq = ls.last_packet_seq | 0;
  const policyTag = policy ? ` [${policy.toUpperCase()}]` : '';
  return `${label}${policyTag}\n` +
         `  latches:  ${total}    fresh: ${freshPct}%    seq: ${seq}\n` +
         `  delta_us: avg ${avg} ms    p99 ${p99} ms\n` +
         `            min ${min} ms    max ${max} ms`;
}

function renderLatencyTab() {
  const panel = document.querySelector('.diag-tab-panel[data-panel="latency"]');
  if (!panel) return;
  panel.style.whiteSpace = 'normal';

  const d = state.diag;
  const ls = d.latchStats || {};
  const lp = d.latchPolicy || {};
  const fp = d.framePhase || null;

  let html = '';

  // --- Top: explainer for the whole tab ---
  html += `<div class="diag-help">The Dreamcast reads your controller once every frame (16.7 ms). This tab shows how your input packets line up with that read, and lets you choose how MapleCast handles inputs that arrive between reads.</div>`;

  // --- Latch policy chooser (the big interactive part) ---
  // Per-user gate: only the player who owns a slot can toggle that slot's
  // policy. Spectators see the read-only "current policy" line. The UI hide
  // is cosmetic — the load-bearing check is the server-side gate in
  // maplecast_ws_server.cpp's set_latch_policy handler.
  const mySlot = state.mySlot;
  html += `<div class="diag-section">LATCH POLICY (your seat)</div>`;
  html += `<div class="diag-help"><b style="color:#aaa">LATENCY</b>: lowest possible delay, but very fast taps that land between two frame reads can be lost. Best for fast NOBD sticks. <b style="color:#aaa">CONSISTENCY</b>: never loses a press (even tap-and-release between reads), but adds up to 1 frame of delay on inputs near the boundary. Best for browser gamepads or lossy networks. <i>This is your personal preference \u2014 it travels with you across slot changes via your saved settings.</i></div>`;

  if (mySlot === 0) {
    html += renderPolicyRow(0, lp.p1);
    html += `<div class="diag-help">P2's current policy: <b style="color:#aaa">${(lp.p2 || 'latency').toUpperCase()}</b> (P2 controls this)</div>`;
  } else if (mySlot === 1) {
    html += `<div class="diag-help">P1's current policy: <b style="color:#aaa">${(lp.p1 || 'latency').toUpperCase()}</b> (P1 controls this)</div>`;
    html += renderPolicyRow(1, lp.p2);
  } else {
    // Spectator: no toggles, just show what each player chose
    html += `<div class="diag-help">You're not in a seat. Click <b style="color:#aaa">I GOT NEXT</b> to play and choose your latch policy.</div>`;
    html += `<pre class="diag-data">P1: ${(lp.p1 || 'latency').toUpperCase()}\nP2: ${(lp.p2 || 'latency').toUpperCase()}</pre>`;
  }

  // --- Frame phase block ---
  html += `<div class="diag-section">FRAME PHASE</div>`;
  html += `<div class="diag-help">period = how long one game frame takes on the server (16.67 ms = 60 fps). guard = the window before each frame read where CONSISTENCY mode defers near-boundary inputs to the next frame.</div>`;
  if (fp) {
    const period = (fp.frame_period_us / 1000).toFixed(2);
    const guard = (fp.guard_us / 1000).toFixed(2);
    html += `<pre class="diag-data">period:    ${period} ms\nguard:     ${guard} ms\nframe:     ${fp.frame}</pre>`;
  } else {
    html += `<pre class="diag-data">(no data — server hasn't sent frame_phase yet)</pre>`;
  }

  // --- Per-slot latch stats ---
  html += `<div class="diag-section">LATCH STATS (last ~4 seconds)</div>`;
  html += `<div class="diag-help"><b style="color:#aaa">latches</b>: total frame reads since boot. <b style="color:#aaa">fresh%</b>: how often a new packet arrived between reads. <b style="color:#aaa">delta_us</b>: how old the most recent packet was at frame read time. <b style="color:#aaa">min near 0 ms</b> = some packets land RIGHT at the read boundary — that's the danger zone where the dashing bug fires under LATENCY mode.</div>`;
  if (!ls.p1 && !ls.p2) {
    html += `<pre class="diag-data">(no data yet)</pre>`;
  } else {
    html += `<pre class="diag-data">${escapeHtml(fmtLatchLine('P1', ls.p1, lp.p1))}\n\n${escapeHtml(fmtLatchLine('P2', ls.p2, lp.p2))}</pre>`;
  }

  // --- How to read your numbers ---
  html += `<div class="diag-section">HOW TO READ YOUR NUMBERS</div>`;
  html += `<div class="diag-help">If your <b style="color:#aaa">min delta_us &lt; 1 ms</b>, you're getting bit by the dashing bug under LATENCY — try CONSISTENCY. If your <b style="color:#aaa">min &gt; 5 ms</b>, you're safe under LATENCY and CONSISTENCY would only add jitter. If <b style="color:#aaa">fresh%</b> is below 30%, you have a slow polling rate (browser gamepad) and CONSISTENCY will help more.</div>`;

  panel.innerHTML = html;

  // Wire up the policy button click handlers AFTER innerHTML replaces them
  panel.querySelectorAll('.policy-btn').forEach(btn => {
    btn.addEventListener('click', () => {
      const slot = parseInt(btn.dataset.slot, 10);
      const policy = btn.dataset.policy;
      if (Number.isNaN(slot) || (policy !== 'latency' && policy !== 'consistency')) return;
      sendLatchPolicy(slot, policy);
    });
  });
}

function renderPolicyRow(slot, currentPolicy) {
  const cur = (currentPolicy || 'latency').toLowerCase();
  const slotName = slot === 0 ? 'P1' : 'P2';
  const latActive = cur === 'latency' ? 'active' : '';
  const conActive = cur === 'consistency' ? 'active' : '';
  return `
<div class="policy-row">
  <span style="min-width:30px;font-weight:bold;color:#aaa">${slotName}</span>
  <button class="policy-btn ${latActive}" data-slot="${slot}" data-policy="latency"
          title="Today's behavior — instantaneous, lowest latency. Best for fast NOBD sticks.">LATENCY</button>
  <button class="policy-btn ${conActive}" data-slot="${slot}" data-policy="consistency"
          title="Accumulator + edge preservation. Trades \u00b11 frame of latency for guaranteed press registration. Best for browser players or lossy networks.">CONSISTENCY</button>
</div>`;
}

function renderInputTab() {
  const panel = document.querySelector('.diag-tab-panel[data-panel="input"]');
  if (!panel) return;
  panel.style.whiteSpace = 'normal';

  const d = state.diag;

  let html = '';
  html += `<div class="diag-help">Information about your local gamepad and how it's connected to your slot. Future versions of this tab will let you remap buttons, set deadzones, and bind a stick to your username so your latch policy follows your stick everywhere.</div>`;

  html += `<div class="diag-section">GAMEPAD</div>`;
  html += `<div class="diag-help">Whether your browser sees a gamepad connected. Chrome requires you to press a button on the pad before it shows up in the list.</div>`;
  html += `<pre class="diag-data">connected: ${state.gamepadConnected ? 'YES' : 'no'}\nid:        ${state.gamepadId || '-'}\nactive:    ${d.gamepadActive ? 'YES (sending)' : 'no'}\nsent:      ${d.inputSendCount}</pre>`;

  html += `<div class="diag-section">SLOT</div>`;
  html += `<div class="diag-help">Which player slot you're sitting in (or whether you're in the queue or just spectating).</div>`;
  html += `<pre class="diag-data">slot:      ${state.mySlot >= 0 ? 'P'+(state.mySlot+1) : '-'}\nqueued:    ${state.wsInQueue ? 'YES' : 'no'}</pre>`;

  html += `<div class="diag-section">COMING SOON</div>`;
  html += `<div class="diag-help">Button mapping editor, analog deadzone slider, vibration test, and per-stick latch policy memory (so your CONSISTENCY/LATENCY choice follows your stick across slot changes).</div>`;

  panel.innerHTML = html;
}

function escapeHtml(s) {
  return String(s)
    .replaceAll('&', '&amp;')
    .replaceAll('<', '&lt;')
    .replaceAll('>', '&gt;');
}

// ----------------------------------------------------------------------------
// Public update entry — re-renders the currently active tab
// ----------------------------------------------------------------------------
export function updateDiag() {
  const el = $('diagOverlay');
  if (!el || !el.classList.contains('open')) return;

  // Lazy-wire the tab click handlers + initial selection
  wireTabsOnce();

  const which = _diagSettings.selectedTab;
  if (which === 'latency')      renderLatencyTab();
  else if (which === 'input')   renderInputTab();
  else                          renderStatsTab();
}

// ----------------------------------------------------------------------------
// Latch policy WS push — used by the F1 hotkey AND the LATENCY tab buttons
// ----------------------------------------------------------------------------
// Per-user gate (client-side mirror of the server-side gate):
//   - You can only set the policy for the slot YOU own.
//   - Spectators (mySlot < 0) cannot set anything.
//   - Trying to set the OTHER player's slot is a no-op + console warn.
//
// The server-side gate in maplecast_ws_server.cpp's set_latch_policy handler
// is the load-bearing security check. This client check just prevents wasted
// round-trips and spurious localStorage writes for spectators.
function sendLatchPolicy(slot, policy) {
  if (state.mySlot !== slot) {
    console.warn(`[diag] sendLatchPolicy: cannot set slot ${slot+1} — you are in ${state.mySlot >= 0 ? 'P'+(state.mySlot+1) : 'no seat'}`);
    return;
  }

  // Prefer the direct control WS (flycast); fall back to the relay WS.
  // Both relay AND flycast accept set_latch_policy because the relay
  // forwards JSON ctrl messages back upstream.
  const ws = state.controlWs?.readyState === WebSocket.OPEN
    ? state.controlWs
    : (state.ws?.readyState === WebSocket.OPEN ? state.ws : null);
  if (!ws) {
    console.warn('[diag] sendLatchPolicy: no WS open');
    return;
  }
  ws.send(JSON.stringify({ type: 'set_latch_policy', slot, policy }));
  console.log(`[diag] requested P${slot+1}: ${policy}`);

  // Stash the user's preference locally — this is THEIR personal setting,
  // not the slot's. The next time they (re)take any slot the join handshake
  // pushes this to flycast. Phase C will sync this same field to their
  // SurrealDB profile so it travels across devices.
  _diagSettings.preferredLatchPolicy = policy;
  saveSettings();
}

function toggleLatchPolicy(slot) {
  const lp = state.diag.latchPolicy || {};
  const cur = (slot === 0 ? lp.p1 : lp.p2) || 'latency';
  const next = cur === 'latency' ? 'consistency' : 'latency';
  sendLatchPolicy(slot, next);
}

// ----------------------------------------------------------------------------
// Keybinds: F1 toggles YOUR slot's policy, Esc closes the modal
// ----------------------------------------------------------------------------
// Per-user gate: you can only toggle the slot you currently own. F1 is the
// "toggle my latch policy" hotkey — does nothing if you're a spectator.
// (F2 was P2's toggle in the per-slot model; removed under per-user since
// each player only owns one slot at a time.)
window.addEventListener('keydown', (e) => {
  const el = $('diagOverlay');
  if (!el || !el.classList.contains('open')) return;

  if (e.key === 'F1') {
    e.preventDefault();
    if (state.mySlot === 0 || state.mySlot === 1) {
      toggleLatchPolicy(state.mySlot);
    } else {
      console.log('[diag] F1: not in a seat — toggle ignored');
    }
  } else if (e.key === 'Escape') {
    e.preventDefault();
    toggleDiag();
  }
});

// ----------------------------------------------------------------------------
// Periodic refresh — 1Hz, also pings the relay so RTT stats stay fresh
// ----------------------------------------------------------------------------
export function startDiagInterval() {
  setInterval(() => {
    updateDiag();
    if (state.ws?.readyState === 1) {
      state.ws.send(JSON.stringify({ type: 'ping', t: performance.now() }));
    }
  }, 1000);
}
